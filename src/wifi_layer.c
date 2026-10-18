#include "wifi_layer.h"
#include "uart.h"
#include "sys.h"
#include "h3_sdc.h"
#include "version.h"

/* Отдельный слой WiFi (SDIO RTL8189FTV). Вызывается из пункта меню
   "Transfer" — инициализация/прошивка/калибровка чипа выполняется только
   при входе в экран, а не при каждом старте прошивки. */

extern const unsigned char fw_blob_start[];
extern const unsigned char fw_blob_end[];

int wifi_active = 0;

static int wifi_started = 0;

int wifi_layer_started(void)
{
    return wifi_started;
}

/* Печать байта как %02X (две hex-цифры). */
static void print_u8_hex(uint8_t v)
{
    static const char hexd[] = "0123456789ABCDEF";
    uart_putc(hexd[(v >> 4) & 0xF]);
    uart_putc(hexd[v & 0xF]);
}

int wifi_layer_start(void)
{
    if (wifi_started)
        return 0;

    uart_print("wifi init...\r\n");

    /* Включить чип (WIFI_EN=PL7), инициализировать SDC1 и SDIO-карту. */
    WLOG("  pl_before=0x"); WLOG_HEX(*(volatile uint32_t *)0x01F02C10);
    WLOG("\r\n");
    h3_sdc1_init();
    WLOG("  pl_after=0x"); WLOG_HEX(*(volatile uint32_t *)0x01F02C10);
    WLOG("\r\n");

    /* Диагностика после инициализации контроллера (расширенный лог). */
    WLOG("  gctl=0x"); WLOG_HEX(*(volatile uint32_t *)0x01C10000);
    WLOG(" ckcr=0x");  WLOG_HEX(*(volatile uint32_t *)0x01C10004);
    WLOG(" sclk=0x");  WLOG_HEX(*(volatile uint32_t *)0x01C2008C);
    WLOG(" star=0x");  WLOG_HEX(*(volatile uint32_t *)0x01C1003C);
    WLOG(" rst0=0x");  WLOG_HEX(*(volatile uint32_t *)0x01C202C0);
    WLOG(" pg_cfg0=0x");WLOG_HEX(*(volatile uint32_t *)0x01C208D8);
    WLOG(" pg_cfg1=0x");WLOG_HEX(*(volatile uint32_t *)0x01C208DC);
    WLOG(" pl_cfg=0x");WLOG_HEX(*(volatile uint32_t *)0x01F02C00);
    WLOG(" pl_dat=0x");WLOG_HEX(*(volatile uint32_t *)0x01F02C10);
    WLOG("\r\n");

    /* Ручной probe CMD5 (диагностика контроллера). */
    {
        volatile uint32_t *SDC = (volatile uint32_t *)0x01C10000;
        SDC[0x38/4] = 0xFFFFFFFF;               /* очистить RISR */
        SDC[0x1C/4] = 0x00E00000;               /* ARG: 3.0-3.6V */
        SDC[0x18/4] = (1u<<31) | 5u | (1u<<6);  /* START|CMD5|RESP_RCV */
        for (int w = 0; w < 1000000; w++)
            if (SDC[0x38/4] & (1u<<2)) break;   /* CMD_DONE */
        WLOG("  cmdr=0x"); WLOG_HEX(SDC[0x18/4]);
        WLOG(" risr=0x");  WLOG_HEX(SDC[0x38/4]);
        WLOG(" resp0=0x"); WLOG_HEX(SDC[0x20/4]);
        uart_print("\r\n");
    }

    {
        int rc = h3_sdio_init_chip();
        uart_print("  sdio_init rc=");
        uart_print_int(rc);
        uart_print("\r\n");
        if (rc == 0) {
            wifi_active = 1;
            uint8_t ccid0 = 0, ccid1 = 0;
            h3_sdc_cmd52_read(0, 0x00, &ccid0);   /* CCCR/SDIO revision */
            h3_sdc_cmd52_read(0, 0x01, &ccid1);   /* SDIO function info */
            uart_print("  ccid0=0x");
            uart_print_hex(ccid0);
            uart_print(" ccid1=0x");
            uart_print_hex(ccid1);
            uart_print("\r\n");

            /* ID чипа из CIS: manf (0x024C Realtek) + card PID (0xF179 8189F). */
            {
                uint16_t manf = 0, card = 0;
                int id_rc = h3_sdio_read_chip_id(&manf, &card);
                uart_print("  chip_id rc=");
                uart_print_int(id_rc);
                if (id_rc == 0) {
                    uart_print(" manf=0x");
                    uart_print_hex(manf);
                    uart_print(" card=0x");
                    uart_print_hex(card);
                }
                uart_print("\r\n");
            }

            /* Power-on чипа: CARDEMU -> ACTIVE. */
            {
                int pon = h3_wifi_power_on_chip();
                uart_print("  pwon rc=");
                uart_print_int(pon);
                if (pon != 0) {
                    uart_print(" step=");
                    uart_print_int((int)sdc_diag_pseq_step);
                    uart_print(" base=");
                    uart_print_int((int)sdc_diag_pseq_base);
                    uart_print(" off=0x");
                    uart_print_hex((uint32_t)sdc_diag_pseq_off);
                    uart_print(" val=0x");
                    uart_print_hex((uint32_t)sdc_diag_pseq_val);
                }
                uart_print("\r\n");
            }

            /* Этап 3: включить I/O-функцию 1 (CCCR 0x02, бит IOEx=0x02). */
            {
                uint8_t ioen;
                h3_sdc_cmd52_read(0, 0x02, &ioen);
                h3_sdc_cmd52_write(0, 0x02, (uint8_t)(ioen | 0x02));
                h3_sdc_cmd52_read(0, 0x02, &ioen);
                uart_print("  fn1 ioen=0x");
                uart_print_hex(ioen);
                uart_print("\r\n");
            }

            /* Этап 4: загрузка прошивки чипа (NIC firmware). */
            {
                uint32_t fwlen = (uint32_t)(fw_blob_end - fw_blob_start);
                int frc = h3_wifi_download_fw(fw_blob_start, fwlen);
                uart_print("  fwdl rc=");
                uart_print_int(frc);
                uart_print(" len=0x");
                uart_print_hex(fwlen);
                if (frc != 0) {
                    uart_print(" blk=");
                    uart_print_int(sdc_diag_fail_blk);
                    uart_print(" cmd=");
                    uart_print_hex(sdc_diag_cmd);
                    uart_print(" arg=0x");
                    uart_print_hex(sdc_diag_arg);
                    uart_print(" cmdr=0x");
                    uart_print_hex(sdc_diag_cmdr);
                    uart_print(" risr=");
                    uart_print_hex(sdc_diag_risr);
                    uart_print(" sta=");
                    uart_print_hex(sdc_diag_sta);
                    if (frc == -33) {
                        uart_print(" mcufwdl=0x");
                        uart_print_hex((uint32_t)sdc_diag_fw_mcufwdl);
                    }
                } else {
                    /* Этап 5: читаем eFuse (заводской MAC); если пуст — задаём
                       программно (как драйвер 8189fs) и пишем в REG_MACID. */
                    uint8_t mac[6];
                    int erc = h3_wifi_efuse_read_mac(mac);
                    uart_print("  efuse rc=");
                    uart_print_int(erc);
                    if (erc == 0) {
                        int valid = 1;
                        for (int k = 0; k < 6; k++)
                            if (mac[k] == 0xFF || mac[k] == 0x00) { valid = 0; break; }
                        if (!valid) {
                            uint32_t t = micros();
                            mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x00;
                            mac[3] = (uint8_t)(t >> 16);
                            mac[4] = (uint8_t)(t >> 8);
                            mac[5] = (uint8_t)(t);
                        }
                    }
                    uart_print(" mac=");
                    for (int k = 0; k < 6; k++) {
                        print_u8_hex(mac[k]);
                        if (k < 5) uart_print(":");
                    }
                    uart_print("\r\n");

                    /* Запись MAC в REG_MACID (0x610-0x615). */
                    {
                        int wk = h3_wifi_set_mac(mac);
                        uart_print("  mac_set=");
                        uart_print(wk == 0 ? "OK" : "FAIL");
                        uart_print("\r\n");
                    }

                    /* Пауза, затем чтение REG_MACID обратно. */
                    udelay(10000);
                    {
                        uint32_t re_lo = 0, re_hi = 0;
                        uint32_t base = (8u << 13) | (0x610u & 0x7FFFu);
                        int rrc = 0;
                        if (h3_sdc_cmd53_read_bytes_noinc(1, base, 4, &re_lo) != 0) rrc = -1;
                        if (h3_sdc_cmd53_read_bytes_noinc(1, base + 4, 2, &re_hi) != 0) rrc = -1;
                        uart_print("  macid_re=");
                        if (rrc == 0) {
                            print_u8_hex((uint8_t)(re_lo & 0xFF));
                            uart_print(":");
                            print_u8_hex((uint8_t)((re_lo >> 8) & 0xFF));
                            uart_print(":");
                            print_u8_hex((uint8_t)((re_lo >> 16) & 0xFF));
                            uart_print(":");
                            print_u8_hex((uint8_t)((re_lo >> 24) & 0xFF));
                            uart_print(":");
                            print_u8_hex((uint8_t)(re_hi & 0xFF));
                            uart_print(":");
                            print_u8_hex((uint8_t)((re_hi >> 8) & 0xFF));
                        } else uart_print("FAIL");
                        uart_print("\r\n");
                    }
                }

                /* W.345: A0 (power state) сразу после fwdl+mac. Если упал
                   с 3 (после pwon) до 0 — FW ушла в RESET на этапе fwdl/мак. */
                {
                    int a0ps = h3_wifi_power_state();
                    uart_print("  a0_post_fw=0x");
                    uart_print_hex((uint32_t)(a0ps & 0xFF));
                    uart_print("\r\n");
                }

                /* RF-калибровка LCK. */
                h3_wifi_rf_lck();

                /* JOINBSS: целевая сеть ДО скана (фикс. SSID для проверки движка
                   host-Auth/Assoc; ввод пароля — отдельно). Парсер RX-скана
                   захватит BSSID/channel при совпадении SSID. */
                {
                    /* Открытая точка пользователя (без WPA2) для доводки assoc.
                       A05s пользователя М — русские буквы (UTF-8). Цель задаём
                       по ASCII-префиксу "A05s"; полный UTF-8 SSID берём из
                       beacon'а (Parsing теперь пропускает байты >=0x80). */
                    static const char tgt_ssid[] = "A05s";
                    h3_wifi_set_target(tgt_ssid, (uint8_t)(sizeof(tgt_ssid) - 1));
                    uart_print("join: target=");
                    uart_print(tgt_ssid);
                    uart_print("\r\n");
                }

                /* Retry: до 3 сканов, если целевая сеть не поймалась
                   (AP может быть на границе видимости). */
                int jrc = 0;
                {
                    uint8_t bssid[6];
                    uint8_t ch = 0;
                    int tries;
                    for (tries = 0; tries < 3; tries++) {
                        h3_wifi_rf_rx_test();
                        if (h3_wifi_target_captured(bssid, &ch) == 0) break;
                        uart_print("join: target not found, retry ");
                        uart_print_int(tries + 1);
                        uart_print("/3\r\n");
                        udelay(500000);
                    }

                    if (tries < 3) {
                        uart_print("join: bssid=");
                        for (int k = 0; k < 6; k++) { print_u8_hex(bssid[k]); if (k < 5) uart_print(":"); }
                        uart_print(" ch="); uart_print_int((int)ch);
                        uart_print("\r\n");
                        jrc = h3_wifi_join_ap();
                    } else {
                        uart_print("join: target not found after 3 scans\r\n");
                        jrc = -4;
                    }
                }
                uart_print("join: rc=");
                uart_print_int(jrc);
                uart_print("\r\n");

                uart_print("\r\n");
            }
        }
    }

    uart_print("wifi ok\r\n");
    wifi_started = 1;
    return 0;
}

/* Подключение к ВЫБРАННОЙ сети из списка (UI «Transfer»): по индексу
   берём её BSSID/channel из буфера scan_aps[] (заполнен парсером скана
   в h3_wifi_rf_rx_test) и захватываем как цель JOINBSS-движка
   (h3_wifi_scan_capture_target -> h3_wifi_join_ap БЕЗ повторного скана).
   Возвращает: 0 = ассоциация подтверждена; -1 = плохой индекс; иначе
   код от h3_wifi_join_ap (например -4 — AuthResp не пришла). */
int wifi_layer_connect(int index)
{
    struct h3_wifi_ap ap;
    int rc;

    if (!wifi_started)
        return -2;                       /* слой не инициализирован */
    if (index < 0 || h3_wifi_scan_ap_get(index, &ap) != 0)
        return -1;                       /* нет такой сети */

    uart_print("join: sel=");
    uart_print_int(index);
    uart_print(" bssid=");
    for (int k = 0; k < 6; k++) { uart_print_hex((uint32_t)ap.bssid[k]); if (k < 5) uart_print(":"); }
    uart_print(" ch=");
    uart_print_int((int)ap.channel);
    uart_print(" ssid=");
    for (int k = 0; k < ap.ssid_len; k++) uart_putc(ap.ssid[k]);
    uart_print("\r\n");

    /* Захватить выбранную сеть как цель ассоциации — полностью (SSID, BSSID,
       канал). Без установки SSID join_target остаётся от старого таргета (A05s
       из kmain), и AssocReq к другой сети несёт чужой SSID — AP молча дропает
       (rc=-6, W.382). */
    h3_wifi_set_target(ap.ssid, ap.ssid_len);
    h3_wifi_scan_capture_target(ap.bssid, ap.channel);
    /* WPA2: передать RSN IE выбранной сети в цель — без него AssocReq к
       WPA2-AP уходит без Privacy+RSN (cap=1) и AP не ассоциирует (rc=-6). */
    h3_wifi_set_target_rsn(&ap);
    /* W.412: для WPA2-сети (rsn_len>0) зафиксировать PSK passphrase целевой
       сети MTS_GPON_9290e0. PMK = PBKDF2-HMAC-SHA1(pass, ssid, 4096, 32)
       вычисляется при приёме EAPOL msg1 в 4-way handshake. */
    if (ap.rsn_len > 0) {
        h3_wifi_set_passphrase("B236f8g4", 8);
        uart_print("  wpa2 passphrase set (rsn_len=");
        uart_print_int((int)ap.rsn_len);
        uart_print(")\r\n");
    }

    rc = h3_wifi_join_ap();
    uart_print("join: rc=");
    uart_print_int(rc);
    uart_print("\r\n");
    return rc;
}