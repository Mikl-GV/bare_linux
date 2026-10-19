#include "h3_sdc.h"
#include "h3.h"
#include "h3_ccu.h"
#include "ports.h"
#include "sys.h"
#include "uart.h"
#include "bb_phy_reg.h"
#include "rf_radioa.h"
#include "mac_reg.h"
#include "wifi_net.h"
#include "wpa_crypto.h"

/* W.232: полный трейс SDIO-обращений к чипу (для сравнения с эталонным
   логом Armbian). При включении каждая CMD52/CMD53 печатается в виде:
   "><52R f0 r0x001E ->0x00 rc0" / "><53W f1 b0x00000200 l4 rc0".
   Отключить — закомментировать. */
/* #define WIFI_SDIO_TRACE 1 */
#ifdef WIFI_SDIO_TRACE
#define SDIO_TRACE(...) uart_print(__VA_ARGS__)
#define SDIO_TRACE_HEX(n) uart_print_hex((uint32_t)(n))
#define SDIO_TRACE_INT(n) uart_print_int((int)(n))
#else
#define SDIO_TRACE(...) do {} while (0)
#define SDIO_TRACE_HEX(n) do {} while (0)
#define SDIO_TRACE_INT(n) do {} while (0)
#endif

/* WLOG (расширенный лог) — определён в include/h3_sdc.h. */

/* Диагностика последней блочной передачи: заполняется при ошибке. */
uint32_t sdc_diag_risr = 0;
uint32_t sdc_diag_sta  = 0;
uint32_t sdc_diag_cmdr = 0;
uint32_t sdc_diag_cmd  = 0;
uint32_t sdc_diag_arg  = 0;
int      sdc_diag_fail_blk = -1;   /* номер блока, на котором упала загрузка FW */
/* Диагностика power-on seq. */
uint32_t sdc_diag_pseq_step = 0;   /* номер шага */
uint32_t sdc_diag_pseq_base = 0;
uint32_t sdc_diag_pseq_off  = 0;
uint32_t sdc_diag_pseq_cmd  = 0;
uint32_t sdc_diag_pseq_val  = 0;   /* прочитанное значение при ошибке */
uint32_t sdc_diag_fw_mcufwdl = 0;  /* REG_MCUFWDL при падении FreeToGo */

/* JOINBSS: целевая сеть для host-ассоциации (SSID + BSSID/channel из скана).
   Определена в начале, т.к. используется из парсера RX-скана. */
static struct {
    char ssid[H3_WIFI_SSID_MAX + 1];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t bssid_captured;   /* 1 = BSSID/channel найдены в скане */
    /* WPA2: RSN IE из beacon'а AP (W.373) — включаем в AssocReq, чтобы
       WPA2-AP принял ассоциацию (без него MTS_GPON_9290e0 даёт rc=-6).
       rsn_len=0 если сеть открытая (без RSN). */
    uint8_t rsn_ie[64];
    uint8_t rsn_len;
    /* W.412: PSK passphrase для 4-way handshake (PMK = PBKDF2(pass, ssid)).
       Задаётся через h3_wifi_set_passphrase. pass_len=0 = сеть открытая. */
    char passphrase[64];
    uint8_t pass_len;
    /* 4-way handshake (WPA2-PSK): заполняется при приёме
       EAPOL-Key msg1; на основе PTK строится msg2. */
    uint8_t anonce[32];          /* AP nonce из msg1 */
    uint8_t replay[8];           /* replay counter из msg1 */
    uint8_t have_msg1;           /* 1 = msg1 получен (обновить msg2 при следующем poll) */
    uint8_t ptk[48];             /* PTK: KCK[0..16) KEK[16..32) TK[32..48) */
    uint8_t pmk[32];             /* PSK->PMK, PBKDF2 */
    uint8_t pmk_valid;           /* W.453: PMK предвычислен (в h3_wifi_set_passphrase) */
    uint8_t snonce[32];          /* наш nonce */
    uint8_t secure;              /* 1 = 4-way завершён и PTK/GTK в CAM — TX data шифровать (Protected) */
} join_target;

/* Список найденных сетей для UI «Transfer» (заполняется парсером RX-скана). */
static struct h3_wifi_ap scan_aps[H3_WIFI_AP_MAX];
static int scan_ap_count = 0;

void h3_wifi_scan_ap_reset(void)
{
    scan_ap_count = 0;
}

void h3_wifi_scan_ap_add(struct h3_wifi_ap *ap)
{
    if (scan_ap_count >= H3_WIFI_AP_MAX) return;
    /* W.349: дедуп по BSSID (каждый физический AP отдельной строкой), а не по
       SSID — одна сеть с несколькими BSSID/каналов должна показываться вся,
       иначе из 15-30 реальных AP в списке остаются 3-4. */
    for (int i = 0; i < scan_ap_count; i++) {
        int same = 1;
        for (int j = 0; j < 6; j++)
            if (scan_aps[i].bssid[j] != ap->bssid[j]) { same = 0; break; }
        if (same) return;   /* тот же физический AP уже есть */
    }
    /* Пофайловое копирование (не `= *ap`): большая struct (rsn_ie[64]) уходит
       в memcpy, а bare-metal линкуется без libc. */
    {
        struct h3_wifi_ap *dst = &scan_aps[scan_ap_count];
        for (int j = 0; j < H3_WIFI_SSID_MAX + 1; j++) dst->ssid[j] = ap->ssid[j];
        dst->ssid_len = ap->ssid_len;
        dst->channel = ap->channel;
        for (int j = 0; j < 6; j++) dst->bssid[j] = ap->bssid[j];
        dst->rssi_lvl = ap->rssi_lvl;
        dst->rsn_len = ap->rsn_len;
        for (int j = 0; j < 64; j++) dst->rsn_ie[j] = ap->rsn_ie[j];
    }
    scan_ap_count++;
}

int h3_wifi_scan_ap_count(void)
{
    return scan_ap_count;
}

int h3_wifi_scan_ap_get(int i, struct h3_wifi_ap *ap)
{
    if (i < 0 || i >= scan_ap_count) return -1;
    /* Пофайловое копирование, не `= scan_aps[i]` — см. h3_wifi_scan_ap_add. */
    {
        const struct h3_wifi_ap *src = &scan_aps[i];
        for (int j = 0; j < H3_WIFI_SSID_MAX + 1; j++) ap->ssid[j] = src->ssid[j];
        ap->ssid_len = src->ssid_len;
        ap->channel = src->channel;
        for (int j = 0; j < 6; j++) ap->bssid[j] = src->bssid[j];
        ap->rssi_lvl = src->rssi_lvl;
        ap->rsn_len = src->rsn_len;
        for (int j = 0; j < 64; j++) ap->rsn_ie[j] = src->rsn_ie[j];
    }
    return 0;
}

/* SD/MMC хост-контроллер H3 (SDC1) — низкоуровневый доступ.
 *
 * Используется для SDIO-связи с WiFi-чипом RTL8189FTV. Реализация опирается
 * на даташит H3 (раздел 5.3.7) и Linux-драйвер sunxi-mmc.
 *
 * Шаги инициализации:
 *   1) включить WiFi-чип: WIFI_EN = PL7 = 1 (порт L, R_PIO 0x01F02C00);
 *   2) включить такт шины (GATING0 bit9 MMC1) и снять reset (RST0 bit9);
 *   3) настроить модульный такт SDMMC1_CLK_REG (OSC24M, делители);
 *   4) настроить пины PG0..PG5 на режим SDC1 (function 2);
 *   5) сбросить контроллер (SOFT_RST), очистить FIFO, настроить шину/блок.
 */

#define SDC_BASE H3_SDC1_BASE

#define SDC_REG(off) (*(volatile uint32_t *)(SDC_BASE + (off)))

/* Порт L (R_PIO): регистры cfg0, data. PL7 = WIFI_EN. */
#define R_PIO_CFG0  (*(volatile uint32_t *)(H3_R_PIO_BASE + 0x00))
#define R_PIO_DATA  (*(volatile uint32_t *)(H3_R_PIO_BASE + 0x10))

/* Биты SDC_CKCR (0x04): CCLK_ENB=bit16, CCLK_DIV=bits7:0 (делитель 2n). */
#define SDC_CKCR_CCLK_ENB   (1u << 16)
#define SDC_CKCR_CCLK_DIV(n) ((n) & 0xFFu)

/* Ожидание бита статуса с таймаутом. Возвращает 0 при успехе, -1 по таймауту. */
static int sdc_wait(volatile uint32_t *reg, uint32_t mask, int set)
{
    uint32_t t;
    for (t = 0; t < 1000000; t++) {
        uint32_t v = *reg;
        if (set ? (v & mask) : !(v & mask))
            return 0;
    }
    return -1;
}

/* Включить WiFi-чип: WIFI_EN (PL7) = 1.
 * Делаем power-cycle: PL7=0, пауза, PL7=1, пауза.
 * Используем ПРЯМУЮ запись полного значения PL_DAT (не RMW), чтобы
 * исключить влияние кэша чтения-модификации-записи на бит 7. */
static void wifi_power_on(void)
{
    uint32_t cfg0 = R_PIO_CFG0;
    cfg0 &= ~(0xFu << 28);      /* очистить PL7 (28-31) */
    cfg0 |=  (1u << 28);        /* PL7 = output (mode 1) */
    R_PIO_CFG0 = cfg0;
    __asm volatile("dsb" ::: "memory");

    /* Выключить: PL7=0, выждать. Пишем полное значение с сохранением LED. */
    R_PIO_DATA = (R_PIO_DATA & ~(1u << 7));   /* сохраняем прочие биты, гасим bit7 */
    __asm volatile("dsb" ::: "memory");
    udelay(30000);

    /* Включить: PL7=1, выждать инициализацию чипа. */
    R_PIO_DATA = (R_PIO_DATA | (1u << 7));    /* поднимаем bit7, остальное сохраняем */
    __asm volatile("dsb" ::: "memory");
    udelay(200000);   /* 200 мс: перекрывает TPOR(2-8мс) + Tnon_rdy(1-10мс) с запасом */
}

static int sdc_update_clk(void)
{
    /* Записать PRG_CLOCK команду: CMD_LOAD | PRG_CLK | WAIT_PRE, ждать LOAD. */
    SDC_REG(SDC_CMDR) = SDC_CMDR_START | SDC_CMDR_PRG_CLK | SDC_CMDR_WAIT_PRE;
    uint32_t t;
    for (t = 0; t < 0xfffff; t++) {
        if (!(SDC_REG(SDC_CMDR) & SDC_CMDR_START))
            break;
    }
    if (t >= 0xfffff) return -1;
    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;   /* очистить статус */
    return 0;
}

void h3_sdc1_init(void)
{
    /* 1) Включить WiFi-чип. */
    wifi_power_on();

    /* 2) Такт шины: gate + полный цикл CCU reset (assert -> deassert). */
    H3_CCU_GATING0 |= H3_CCU_GATE_MMC1;
    __asm volatile("dsb" ::: "memory");
    H3_CCU_RST0 &= ~H3_CCU_RST_MMC1;
    __asm volatile("dsb" ::: "memory");
    udelay(10);
    H3_CCU_RST0 |= H3_CCU_RST_MMC1;
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    /* 3) Модульный такт MMC1: OSC24M, OLD mode (как в boot0 для 400кГц).
          SDMMC1_CLK = 0x0002000f (N=2, M=16) + SCLK_GATING.
          SCLK = OSC24M / N / M = 24M/2/16 ~= 750 кГц.
          Затем делитель в CKC даст нужный CCLK (~375 кГц, как SD-init). */
    H3_CCU_SDMMC1_CLK = H3_SDMMC1_SCLK_GATE
                      | H3_SDMMC1_CLK_SRC_OSC24M
                      | (2u << 16)   /* N=2 */
                      | (15u << 0);  /* M=16 */
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    /* 4) Пины PG0..PG5 в режим SDC1 (function 2). */
    uint32_t c0 = *(volatile uint32_t *)(PORTG + 0x00);
    c0 &= ~(0xFFFFu);
    c0 |=  (0x2222u);
    *(volatile uint32_t *)(PORTG + 0x00) = c0;

    uint32_t c1 = *(volatile uint32_t *)(PORTG + 0x04);
    c1 &= ~(0xFFu);
    c1 |=  (0x22u);
    *(volatile uint32_t *)(PORTG + 0x04) = c1;

    /* 4b) W.237: pinctrl ВЫКЛЮЧЕН (стенд: на 50 МГц drive-strength 30мА +
         pull-up на PG0-5 ломают даже 1-bit data-фазу — W.236: fwdl page0
         CMD53W FAIL при рабочем CMD52; в эталоне тот же dtb работает, но
         у Lakka pinctrl применяется до init-такта 400кГц, а не на 50 МГц).
         Рабочая шина стенда — 1-bit/50 МГц без pinctrl (W.235: pwon/fwdl/
         AGC/RF/LCK чисто). Вернём после решения порядка применения. */
    __asm volatile("dsb" ::: "memory");

    /* 5) Сброс контроллера: GCTL_RESET (SOFT+FIFO+DMA), ждать автсброса. */
    SDC_REG(SDC_GCTL) = SDC_GCTL_RESET;
    sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_GCTL), SDC_GCTL_RESET, 0);

    /* 6) HWRST: вывод контроллера из reset (по референсу boot0).
          W.228 (диагностика): reset-delay возвращён к 1 мс (до правки
          W.221 он был 1 мс и pwon проходил); эталон u-boot 0015 требует
          20 мс — вернём после локализации причины rc=-41. */
    SDC_REG(SDC_HWRST) = 1u;   /* active */
    SDC_REG(SDC_HWRST) = 0u;   /* reset */
    udelay(1000);
    SDC_REG(SDC_HWRST) = 1u;   /* active */

    /* 7) Такт карты через CKC: очистить делитель, затем включить CCLK. */
    SDC_REG(SDC_CKCR) = 0;
    sdc_update_clk();
    SDC_REG(SDC_CKCR) = SDC_CKCR_CCLK_ENB | SDC_CKCR_CCLK_CTRL
                      | (1u & SDC_CKCR_CCLK_MASK);   /* DIV=1 -> ~375 кГц */
    sdc_update_clk();

    /* 8) SD_FUNS: включить функции контроллера (0xceAA, как в sunxi-mmc). */
    SDC_REG(SDC_FUNS) = 0xceAAu;
    __asm volatile("dsb" ::: "memory");

    /* 9) Прочее: INT_ENB, таймаут, ширина (0=1-bit, 4-bit ставится позже
      в h3_sdio_init_chip после CMD7 — эталон dtb bus-width=<4>), блок 512. */
    SDC_REG(SDC_GCTL) = SDC_GCTL_INT_ENB | SDC_GCTL_CD_DBC;
    SDC_REG(SDC_IMKR) = 0;
    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;
    SDC_REG(SDC_TMOR) = 0xFFFFFF40u;
    SDC_REG(SDC_CTYPE) = 0x0u;
    SDC_REG(SDC_BLKSZ) = 512u;
    __asm volatile("dsb" ::: "memory");
}

int h3_sdc_cmd_no_data_flags(uint32_t cmd, uint32_t arg, uint32_t resp_reg,
                             int has_resp, int check_crc)
{
    (void)resp_reg;
    uint32_t cmdval = SDC_CMDR_START | (cmd & SDC_CMDR_IDX_MASK)
                    | (has_resp ? SDC_CMDR_RESP_RCV : 0)
                    | (check_crc ? SDC_CMDR_CHK_CRC : 0);

    /* Если это CMD0 — добавить init sequence (80 тактов). */
    if (cmd == 0)
        cmdval |= SDC_CMDR_SEND_INIT;

    /* Очистить статус. */
    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;

    SDC_REG(SDC_ARG) = arg;
    __asm volatile("dmb sy" ::: "memory");
    SDC_REG(SDC_CMDR) = cmdval;
    __asm volatile("dmb sy" ::: "memory");

    /* Ждать CMD_COMPLETE или ошибки. */
    uint32_t t, status;
    for (t = 0; t < 0xffffff; t++) {
        status = SDC_REG(SDC_RISR);
        if (status & SDC_RISR_CMD_DONE)
            break;
        if (status & SDC_RISR_RAW_ISTA)   /* ошибка ответа/таймаут */
            break;
    }
    if (t >= 0xffffff) {   /* таймаут ожидания */
        SDC_REG(SDC_RISR) = 0xFFFFFFFFu;
        return -3;
    }

    /* Если ошибка (не CMD_COMPLETE) — сбросить контроллер и вернуть код. */
    if (!(status & SDC_RISR_CMD_DONE)) {
        SDC_REG(SDC_GCTL) = SDC_GCTL_RESET;
        sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_GCTL), SDC_GCTL_RESET, 0);
        sdc_update_clk();
        SDC_REG(SDC_GCTL) = SDC_GCTL_INT_ENB | SDC_GCTL_CD_DBC;
        SDC_REG(SDC_RISR) = 0xFFFFFFFFu;
        return -2;
    }

    /* Успех: очистить флаги. */
    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;
    return 0;
}

int h3_sdc_cmd_no_data(uint32_t cmd, uint32_t arg, uint32_t resp_reg)
{
    return h3_sdc_cmd_no_data_flags(cmd, arg, resp_reg, 1, 1);
}

/* SDIO CMD52 — байтовый доступ к регистру функции чипа.
 * Формат аргумента (SDIO spec):
 *   bit31    = write flag (1=write, 0=read)
 *   bits30:28= function number (0..6)
 *   bit26    = RAW
 *   bits25:9 = register/address
 *   bits7:0  = write data (для чтения не используется)
 * Ответ (R5): bits[7:0] = read data.
 */
int h3_sdc_cmd52_read(uint32_t func, uint32_t reg, uint8_t *out)
{
    uint32_t arg = (0u << 31) | ((func & 7u) << 28) | ((reg & 0x1FFFF) << 9);
    int rc = h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
    if (rc != 0) {
        SDIO_TRACE("><52R f");
        SDIO_TRACE_INT((int)(func & 7u));
        SDIO_TRACE(" r0x");
        SDIO_TRACE_HEX(reg & 0x1FFFF);
        SDIO_TRACE(" !rc");
        SDIO_TRACE_INT(rc);
        SDIO_TRACE("\r\n");
        return rc;
    }
    *out = (uint8_t)(SDC_REG(SDC_RESP0) & 0xFF);
    SDIO_TRACE("><52R f");
    SDIO_TRACE_INT((int)(func & 7u));
    SDIO_TRACE(" r0x");
    SDIO_TRACE_HEX(reg & 0x1FFFF);
    SDIO_TRACE(" ->0x");
    SDIO_TRACE_HEX((uint32_t)*out);
    SDIO_TRACE(" rc0\r\n");
    return 0;
}

int h3_sdc_cmd52_write(uint32_t func, uint32_t reg, uint8_t val)
{
    uint32_t arg = (1u << 31) | ((func & 7u) << 28) | ((reg & 0x1FFFF) << 9)
                 | (val & 0xFF);
    int rc = h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
    SDIO_TRACE("><52W f");
    SDIO_TRACE_INT((int)(func & 7u));
    SDIO_TRACE(" r0x");
    SDIO_TRACE_HEX(reg & 0x1FFFF);
    SDIO_TRACE(" =0x");
    SDIO_TRACE_HEX((uint32_t)val);
    SDIO_TRACE(" rc");
    SDIO_TRACE_INT(rc);
    SDIO_TRACE("\r\n");
    return rc;
}

/* Установить блок-размер для функции 1 (WLAN IOREG) через CCCR 0x110-0x111.
   По спецификации SDIO перед CMD53 в block mode чип должен иметь установленный
   block size; для Realtek RTL8189 по умолчанию он 0 (недопустимо). 512 байт. */
int h3_sdc_set_fn1_blocksize(uint16_t blksz)
{
    int rc;
    rc = h3_sdc_cmd52_write(0, 0x110, (uint8_t)(blksz & 0xFF));          /* младший */
    if (rc) return rc;
    rc = h3_sdc_cmd52_write(0, 0x111, (uint8_t)((blksz >> 8) & 0xFF));  /* старший */
    return rc;
}

/* SDIO CMD53 — запись в byte mode (len байт, без автоинкремента-блока).
   Используется для загрузки прошивки чипа: драйвер Realtek пишет прошивку
   по 32-бит словам (rtw_write32 -> sdio_write32 -> CMD53 byte mode, 4 байта).
   Аргумент: write | func | byte_mode(27=0) | inc(26) | addr(25:9) | byte count. */
int h3_sdc_cmd53_write_bytes(uint32_t func, uint32_t addr, uint32_t len, const void *data)
{
    uint32_t arg = (1u << 31)                       /* write */
                 | ((func & 7u) << 28)              /* функция */
                 | (0u << 27)                       /* byte mode */
                 | (1u << 26)                       /* increment addr */
                 | ((addr & 0x1FFFF) << 9)          /* база */
                 | (len & 0x1FF);                   /* число байт (1..511) */
    int rc = h3_sdc_block_transfer(53, arg, len, 1, 1, (void *)data);
    SDIO_TRACE("><53W f");
    SDIO_TRACE_INT((int)(func & 7u));
    SDIO_TRACE(" b0x");
    SDIO_TRACE_HEX(addr & 0x1FFFF);
    SDIO_TRACE(" l");
    SDIO_TRACE_INT((int)len);
    SDIO_TRACE(" rc");
    SDIO_TRACE_INT(rc);
    SDIO_TRACE("\r\n");
    return rc;
}

/* SDIO CMD53 — чтение в byte mode (len байт). Зеркальная write_bytes:
   для регистров WLAN IOREG >= 0x100 (REG_MACID 0x610 и т.д.), куда
   CMD52 не ходит. */
int h3_sdc_cmd53_read_bytes(uint32_t func, uint32_t addr, uint32_t len, void *data)
{
    uint32_t arg = (0u << 31)                       /* read */
                 | ((func & 7u) << 28)              /* функция */
                 | (0u << 27)                       /* byte mode */
                 | (1u << 26)                       /* increment addr */
                 | ((addr & 0x1FFFF) << 9)          /* база */
                 | (len & 0x1FF);                   /* число байт (1..511) */
    int rc = h3_sdc_block_transfer(53, arg, len, 1, 0, data);
    SDIO_TRACE("><53R f");
    SDIO_TRACE_INT((int)(func & 7u));
    SDIO_TRACE(" b0x");
    SDIO_TRACE_HEX(addr & 0x1FFFF);
    SDIO_TRACE(" l");
    SDIO_TRACE_INT((int)len);
    SDIO_TRACE(" rc");
    SDIO_TRACE_INT(rc);
    SDIO_TRACE("\r\n");
    return rc;
}

/* SDIO CMD53 — запись в byte mode БЕЗ инкремента адреса (inc=0).
   Используется для регистров, где аппаратный инкремент на границе
   4 байт глючит (REG_MACID: 0x610 и 0x614) — каждый байт по явному адресу. */
int h3_sdc_cmd53_write_bytes_noinc(uint32_t func, uint32_t addr, uint32_t len, const void *data)
{
    uint32_t arg = (1u << 31)                       /* write */
                 | ((func & 7u) << 28)              /* функция */
                 | (0u << 27)                       /* byte mode */
                 | (0u << 26)                       /* НЕ increment */
| ((addr & 0x1FFFF) << 9)          /* база */
                  | (len & 0x1FF);                   /* число байт (1..511) */
    int rc = h3_sdc_block_transfer(53, arg, len, 1, 1, (void *)data);
    SDIO_TRACE("><53W f");
    SDIO_TRACE_INT((int)(func & 7u));
    SDIO_TRACE(" b0x");
    SDIO_TRACE_HEX(addr & 0x1FFFF);
    SDIO_TRACE(" l");
    SDIO_TRACE_INT((int)len);
    SDIO_TRACE(" ni rc");
    SDIO_TRACE_INT(rc);
    SDIO_TRACE("\r\n");
    return rc;
}

/* SDIO CMD53 — чтение в byte mode БЕЗ инкремента адреса (inc=0). */
int h3_sdc_cmd53_read_bytes_noinc(uint32_t func, uint32_t addr, uint32_t len, void *data)
{
    uint32_t arg = (0u << 31)                       /* read */
                 | ((func & 7u) << 28)              /* функция */
                 | (0u << 27)                       /* byte mode */
                 | (0u << 26)                       /* НЕ increment */
                 | ((addr & 0x1FFFF) << 9)          /* база */
                 | (len & 0x1FF);                   /* число байт (1..511) */
    int rc = h3_sdc_block_transfer(53, arg, len, 1, 0, data);
    SDIO_TRACE("><53R f");
    SDIO_TRACE_INT((int)(func & 7u));
    SDIO_TRACE(" b0x");
    SDIO_TRACE_HEX(addr & 0x1FFFF);
    SDIO_TRACE(" l");
    SDIO_TRACE_INT((int)len);
    SDIO_TRACE(" ni rc");
    SDIO_TRACE_INT(rc);
    SDIO_TRACE("\r\n");
    return rc;
}

/* Чтение 32-бит регистра WLAN-функции (function 1) чипа.
   Realtek отображает ММIO-регистры в «WLAN IOREG»: SDIO-адрес =
   (device_id 8 << 13) | (reg & 0x7FFF). Для reg < 0x100 — CMD52 (байты,
   младший первым); для reg >= 0x100 — CMD53 byte-mode (sdio_read32) —
   CMD52 к старшим адресам не ходит (доказано на стенде). */
int h3_sdc_read_wlan_reg32(uint32_t reg, uint32_t *val)
{
    uint32_t ftaddr = (8u << 13) | (reg & 0x7FFFu);
    uint8_t b[4];
    int rc;
    if ((reg & 0x7FFFu) < 0x100) {
        if ((rc = h3_sdc_cmd52_read(1, ftaddr, &b[0])) != 0) return rc;
        if ((rc = h3_sdc_cmd52_read(1, ftaddr + 1, &b[1])) != 0) return rc;
        if ((rc = h3_sdc_cmd52_read(1, ftaddr + 2, &b[2])) != 0) return rc;
        if ((rc = h3_sdc_cmd52_read(1, ftaddr + 3, &b[3])) != 0) return rc;
    } else {
        if ((rc = h3_sdc_cmd53_read_bytes(1, ftaddr, 4, b)) != 0) return rc;
    }
    *val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

/* Чтение 16-бит регистра WLAN-функции (function 1): CMD52 (<0x100) или
   CMD53 byte-mode (>=0x100). */
int h3_sdc_read_wlan_reg16(uint32_t reg, uint16_t *val)
{
    uint32_t ftaddr = (8u << 13) | (reg & 0x7FFFu);
    uint8_t b0, b1;
    int rc;
    if ((reg & 0x7FFFu) < 0x100) {
        if ((rc = h3_sdc_cmd52_read(1, ftaddr, &b0)) != 0) return rc;
        if ((rc = h3_sdc_cmd52_read(1, ftaddr + 1, &b1)) != 0) return rc;
    } else {
        uint8_t b[2];
        if ((rc = h3_sdc_cmd53_read_bytes(1, ftaddr, 2, b)) != 0) return rc;
        b0 = b[0]; b1 = b[1];
    }
    *val = (uint16_t)(b0 | ((uint16_t)b1 << 8));
    return 0;
}

/* Запись 32-бит регистра WLAN-функции (function 1): CMD52 для reg<0x100
   (4 байта, младший первым), CMD53 byte-mode для >=0x100 (sdio_write32). */
int h3_sdc_write_wlan_reg32(uint32_t reg, uint32_t val)
{
    uint32_t ftaddr = (8u << 13) | (reg & 0x7FFFu);
    uint8_t b[4];
    int rc;
    b[0] = (uint8_t)(val & 0xFF);
    b[1] = (uint8_t)((val >> 8) & 0xFF);
    b[2] = (uint8_t)((val >> 16) & 0xFF);
    b[3] = (uint8_t)((val >> 24) & 0xFF);
    if ((reg & 0x7FFFu) < 0x100) {
        if ((rc = h3_sdc_cmd52_write(1, ftaddr,     b[0])) != 0) return rc;
        if ((rc = h3_sdc_cmd52_write(1, ftaddr + 1, b[1])) != 0) return rc;
        if ((rc = h3_sdc_cmd52_write(1, ftaddr + 2, b[2])) != 0) return rc;
        if ((rc = h3_sdc_cmd52_write(1, ftaddr + 3, b[3])) != 0) return rc;
    } else {
        if ((rc = h3_sdc_cmd53_write_bytes(1, ftaddr, 4, b)) != 0) return rc;
    }
    return 0;
}

/* Запись 16-бит регистра WLAN-функции (function 1): CMD52 (<0x100) или
   CMD53 byte-mode (>=0x100). */
int h3_sdc_write_wlan_reg16(uint32_t reg, uint16_t val)
{
    uint32_t ftaddr = (8u << 13) | (reg & 0x7FFFu);
    int rc;
    if ((reg & 0x7FFFu) < 0x100) {
        if ((rc = h3_sdc_cmd52_write(1, ftaddr, (uint8_t)(val & 0xFF))) != 0) return rc;
        if ((rc = h3_sdc_cmd52_write(1, ftaddr + 1, (uint8_t)((val >> 8) & 0xFF))) != 0) return rc;
        return 0;
    }
    {
        uint8_t b[2];
        b[0] = (uint8_t)(val & 0xFF);
        b[1] = (uint8_t)((val >> 8) & 0xFF);
        return h3_sdc_cmd53_write_bytes(1, ftaddr, 2, b);
    }
}

/* Запись 8-бит регистра WLAN-функции (function 1) через CMD52. */
int h3_sdc_write_wlan_reg8(uint32_t reg, uint8_t val)
{
    if ((reg & 0x7FFFu) < 0x100)
        return h3_sdc_cmd52_write(1, (8u << 13) | (reg & 0x7FFFu), val);
    return h3_sdc_cmd53_write_bytes(1, (8u << 13) | (reg & 0x7FFFu), 1, &val);
}

int h3_sdc_read_wlan_reg8(uint32_t reg, uint8_t *val)
{
    if ((reg & 0x7FFFu) < 0x100)
        return h3_sdc_cmd52_read(1, (8u << 13) | (reg & 0x7FFFu), val);
    return h3_sdc_cmd53_read_bytes(1, (8u << 13) | (reg & 0x7FFFu), 1, val);
}

/* ---- Power-up sequence чипа RTL8189FTV (реализация rtl8188F_card_enable_flow) ----
 *
 * Перед загрузкой прошивки чип надо перевести из CARDEMU в ACTIVE по PowerSeq.
 * Записи: { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value }.
 *   base: 0=MAC, 3=SDIO. CMD52-адрес регистра = (8<<13) | (offset & 0x7FFF).
 *   cmd:  1=WRITE (read&msk | value), 2=POLLING ((read&msk)==value), 3=DELAY, 4=END.
 */

/* Чтение 8-бит регистра чипа по base.
   base: 0=MAC -> WLAN IOREG (device id 8).
         3=SDIO -> SDIO local (device id 0).
   Проверено на стенде: SDIO local регистры (0x86, 0x100) отвечают НА ФУНКЦИИ 0
   (CMD52 с функцией 1 к ним не отвечает и зависает). Используем функцию 0. */
static int pseq_read8(int base, uint16_t offset, uint8_t *out)
{
    if (base == 3) {
        return h3_sdc_cmd52_read(0, (uint32_t)offset & 0x0FFFu, out);
    }
    return h3_sdc_cmd52_read(1, (8u << 13) | ((uint32_t)offset & 0x7FFFu), out);
}

/* Запись 8-бит регистра чипа по base. */
static int pseq_write8(int base, uint16_t offset, uint8_t val)
{
    if (base == 3) {
        return h3_sdc_cmd52_write(0, (uint32_t)offset & 0x0FFFu, val);
    }
    return h3_sdc_cmd52_write(1, (8u << 13) | ((uint32_t)offset & 0x7FFFu), val);
}

/* Обобщённый интерпретатор PowerSeq записи. */
static int pseq_exec(const uint8_t *seq, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        /* Каждая запись: offset(2) cut(1) fab_if(1) base_cmd(1) msk(1) val(1) = 7 байт */
        uint16_t off = (uint16_t)(seq[i*7] | (seq[i*7+1] << 8));
        uint8_t  cut = seq[i*7+2];
        uint8_t  fab_if = seq[i*7+3];
        uint8_t  base_cmd = seq[i*7+4];
        uint8_t  msk = seq[i*7+5];
        uint8_t  val = seq[i*7+6];
        int cmd = base_cmd & 0x0F;
        int base = (base_cmd >> 4) & 0x0F;

        (void)cut; (void)fab_if;   /* в нашем случае CUT_ALL, всё подходит */

        WLOG("  pseq#");
        WLOG_INT(i);
        WLOG(" b=");
        WLOG_INT(base);
        WLOG(" o=0x");
        WLOG_HEX((uint32_t)off);
        WLOG(" c=");
        WLOG_INT(cmd);
        WLOG(" m=0x");
        WLOG_HEX((uint32_t)msk);
        WLOG(" v=0x");
        WLOG_HEX((uint32_t)val);
        WLOG("\r\n");

        if (cmd == 0x01) {          /* WRITE: read&msk | value */
            uint8_t cur = 0;
            int rc = pseq_read8(base, off, &cur);
            if (rc) return -10 - i;
            cur = (uint8_t)((cur & ~msk) | (val & msk));
            rc = pseq_write8(base, off, cur);
            if (rc) return -20 - i;
        } else if (cmd == 0x02) {   /* POLLING: (read&msk)==value */
            for (j = 0; j < 10000; j++) {
                uint8_t cur = 0;
                int rc = pseq_read8(base, off, &cur);
                if (rc) return -30 - i;
                if ((cur & msk) == (val & msk)) break;
                udelay(50);
            }
            if (j >= 10000) {
                uint8_t cur = 0;
                pseq_read8(base, off, &cur);
                sdc_diag_pseq_step = (uint32_t)i;
                sdc_diag_pseq_base = (uint32_t)base;
                sdc_diag_pseq_off  = (uint32_t)off;
                sdc_diag_pseq_cmd  = (uint32_t)cmd;
                sdc_diag_pseq_val  = (uint32_t)cur;
                return -40 - i;
            }
        } else if (cmd == 0x03) {   /* DELAY */
            if (val == 0) udelay(off);          /* us */
            else udelay(off * 1000u);           /* ms */
        } else if (cmd == 0x04) {   /* END */
            break;
        }
        /* PWR_CMD_READ (0) — пропускаем */
    }
    return 0;
}

/* Состояние питания чипа (диагностика, эталон rtl8188f_hal_init.c:1115-1130):
   0xA3[2:0] = 0b010 (power state select), затем читать 0xA0[1:0]; 0x01 = ACTIVE.
   Возвращает 0xA0[1:0] или -1 при ошибке доступа. */
int h3_wifi_power_state(void)
{
    uint8_t t = 0;

    if (pseq_read8(0, 0xA3, &t) != 0) return -1;
    t = (uint8_t)((t & 0xF8u) | 0x02u);
    if (pseq_write8(0, 0xA3, t) != 0) return -1;
    if (pseq_read8(0, 0xA0, &t) != 0) return -1;
    return (int)(t & 0x03u);
}

/* Чтение SDIO-local регистров (device id 0).
   Эталон (sdio_ops.c:880-903 _sdio_local_read + sdio_ops_linux.c:747
   _sd_read) при включённом MAC читает их CMD53 по function 1 одним
   доступом (sdio_memcpy_fromio(func1, addr, cnt)), НЕ CMD52 fn0.
   Раньше читали побайтно CMD52 fn0 — на 0x20 (FREE_TXPG) это давало
   стабильный 0, хотя регистр живой (W.238). */
static int sdio_local_read_bytes(uint32_t off, uint8_t *buf, uint32_t len)
{
    return h3_sdc_cmd53_read_bytes(1, off & 0x1FFFFu, len, buf);
}

/* Свободные страницы очереди HIQ в TX FIFO.
   Эталон: HalQueryTxBufferStatus8188FSdio (sdio_ops.c:2273-2299) читает 8 байт из
   SDIO_REG_FREE_TXPG (hal_com_reg.h:1602, offset 0x20) и берёт счётчики по чётным
   байтам: [0]=HIQ, [2]=MIDQ, [4]=LOWQ, [6]=PUBQ (16-бит little-endian).
   Возвращает число свободных страниц HIQ или -1 при ошибке доступа. */
int h3_wifi_tx_free_pages_hiq(void)
{
    uint8_t buf[8];

    if (sdio_local_read_bytes(0x0020u, buf, 8) != 0) return -1;
    return (int)((uint32_t)buf[0] | ((uint32_t)buf[1] << 8));
}

/* Свободные страницы LOW-очереди (BE/data, байт [4][5] FREE_TXPG).
   Используется для отправки data-фреймов (DHCP/ARP/IP) — они идут не в HIQ
   (mgmt), а в LOQ (device 6, BE). W.389. */
int h3_wifi_tx_free_pages_loq(void)
{
    uint8_t buf[8];

    if (sdio_local_read_bytes(0x0020u, buf, 8) != 0) return -1;
    return (int)((uint32_t)buf[4] | ((uint32_t)buf[5] << 8));
}

/* Полный power-on чипа: тянет карту из CARDEMU в ACTIVE. */
int h3_wifi_power_on_chip(void)
{
    uart_print("  pwon: CARDEMU->ACTIVE start\r\n");
    /* rtl8188F_card_enable_flow = CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT + END.
       Вручную раскрываем две последовательности из Hal8188FPwrSeq.h. */
    /* CARDEMU_TO_ACT (макрос RTL8188F_TRANS_CARDEMU_TO_ACT, Hal8188FPwrSeq.h:40-49).
       Шаги 2 и 6 — поллинги готовности (0x06.bit1 = power ready, 0x05.bit0 = 0):
       pseq_exec ограничивает ожидание таймаутом, поэтому зависнуть не может. */
    static const uint8_t seq_ce2ac[] = {
        /* 1) 0x0005 MAC WRITE BIT2 0 — disable SW LPS */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x04, 0x00,
        /* 2) 0x0006 MAC WRITE BIT1 BIT1 — power ready (W.228: поллинг
              (c=2) НЕ сходится на этом чипе — 0x06 не поднимает BIT1 в
              CARDEMU; по стенду W.139+ работают только WRITE-шаги) */
        0x06,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x02, 0x02,
        /* 3) 0x0005 MAC WRITE BIT7 0 — disable HWPDN */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x80, 0x00,
        /* 4) 0x0005 MAC WRITE BIT3 0 — disable WL suspend */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x08, 0x00,
        /* 5) 0x0005 MAC WRITE BIT0 BIT0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x01, 0x01,
        /* 6) 0x0005 MAC WRITE BIT0 0 (W.228: было POLLING BIT0==0 —
              не сходится; WRITE работает) */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x01, 0x00,
        /* 7) 0x0027 MAC WRITE 0xff 0x35 — xtal_qsel=1 (меньше RF-шума) */
        0x27,0x00, 0xFF, (0<<4)|0x00, 0x01, 0xFF, 0x35,
        /* END */
        0xFF,0xFF, 0xFF, (0<<4)|0x00, 0x04, 0x00, 0x00,
    };

    /* Разблокировать ISO/CLK/Power-регистры: эталон делает это первым шагом
       CardEnable (sdio_halinit.c:43-45), иначе часть записей PwrSeq не проходит. */
    if (pseq_write8(0, 0x1C, 0x00) != 0) {
        uart_print("  pwon: RSV_CTRL unlock FAIL\r\n");
        return -1;
    }

    /* Чип уже вне suspend (sdio86=0x00), поэтому CARDDIS_TO_CARDEMU с его
       поллингом «ждать suspend» не сходится. Запускаем сразу CARDEMU_TO_ACT. */
    {
        int rp = pseq_exec(seq_ce2ac, sizeof(seq_ce2ac)/7);
        uart_print("  pwon rc=");
        uart_print_int(rp);
        uart_print("\r\n");

        /* W.253: ВКЛЮЧИТЬ BB/AFE clock — 0x2B[4:2]=111 (Hal8188FPwrSeq.h:131,
           STEP CARDEMU_TO_ACT: {0x002B, ..., 0x1c, 0x1c}
           "0x2b[4:2] = 3b'111 to enable BB,AFE clock").
           Наша ручная последовательность seq_ce2ac этого шага НЕ содержала.
           Без BB/AFE-такта таблицы BB/RF и LCK проходят (регистровый доступ
           работает), но приёмник/передатчик физически не тактируются —
           ровно наш симптом: всё настраивается, а RXPKT_NUM=0 и TX не уходит.
           Ставим ДО BB/RF/LCK (pwon вызывается раньше). */
        {
            uint8_t b2b = 0;
            if (pseq_read8(0, 0x2B, &b2b) == 0) {
                uint8_t b2b_old = b2b;
                pseq_write8(0, 0x2B, (uint8_t)((b2b & ~0x1Cu) | 0x1Cu));
                pseq_read8(0, 0x2B, &b2b);
                uart_print("  pwon: bb_afe_clk 0x2b ");
                uart_print_hex((uint32_t)b2b_old);
                uart_print("->0x");
                uart_print_hex((uint32_t)b2b);
                uart_print("\r\n");
            } else {
                uart_print("  pwon: bb_afe_clk FAIL\r\n");
            }
        }

        /* Диагностика состояния питания чипа: 1 = ACTIVE. */
        int st = h3_wifi_power_state();
        uart_print("  pwon: power_state=");
        uart_print_int(st);
        uart_print("\r\n");
        return rp;
    }
}

/* ---- Чтение eFuse (заводская калибровка / MAC) ----
 * Протокол 8188F (rtw_efuse.c): включить EFUSE-контроллер (0xCF=0x69),
 * затем для каждого байта: адрес [7:0] в 0x31, [9:8] в 0x32[1:0],
 * триггер чтения 0x33[7]=0, поллить 0x33[7]=1, данные в 0x30.
 * Возвращает 0 при успехе (buf заполнен), -1 при недоступности. */
int h3_wifi_efuse_read_mac(uint8_t mac[6])
{
    int byte_off;
    /* 1) Включить EFUSE-контроллер (EFUSE_ACCESS_ON). */
    if (h3_sdc_cmd52_write(1, (8u << 13) | 0x00CF, 0x69)) {
        uart_print("  efuse: ACCESS_ON FAIL (CF)\r\n");
        return -1;
    }
    udelay(10);

    /* 2) MAC лежит по адресу 0x11A..0x11F внутри карты EFUSE. */
    for (byte_off = 0; byte_off < 6; byte_off++) {
        uint16_t addr = (uint16_t)(0x11A + byte_off);
        uint8_t v, a8, a9;
        /* Адрес [7:0] в 0x31. */
        if (h3_sdc_cmd52_write(1, (8u << 13) | 0x0031, (uint8_t)(addr & 0xFF))) return -1;
        /* Адрес [9:8] в 0x32[1:0], остальное не трогаем. */
        if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0032, &a8)) return -1;
        a8 = (uint8_t)((a8 & 0xFC) | ((addr >> 8) & 0x03));
        if (h3_sdc_cmd52_write(1, (8u << 13) | 0x0032, a8)) return -1;
        /* Триггер: 0x33[7] = 0 (start read). */
        if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0033, &a9)) return -1;
        if (h3_sdc_cmd52_write(1, (8u << 13) | 0x0033, (uint8_t)(a9 & 0x7F))) return -1;
        /* Поллить готовность: 0x33[7] = 1. */
        int t;
        for (t = 0; t < 10000; t++) {
            if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0033, &a9)) return -1;
            if (a9 & 0x80) break;
        }
        if (t >= 10000) {
            uart_print("  efuse: READ TIMEOUT @0x");
            uart_print_hex((uint32_t)addr);
            uart_print("\r\n");
            return -1;
        }
        /* Данные в 0x30. */
        if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0030, &v)) return -1;
        mac[byte_off] = v;
    }
    /* W.310: снять EFUSE_ACCESS_ON (0xCF=0x00) — как эталон rtw_efuse.c
       всегда делает в конце. Без этого чип остаётся в eFuse-режиме на всю
       оставшуюся инициализацию (LCK/IQK/работа 8051). */
    h3_sdc_cmd52_write(1, (8u << 13) | 0x00CF, 0x00);
    return 0;
}

/* Чтение одного байта карты EFUSE (протокол 8188F, тот же доступ 0x30/0x31/
   0x32/0x33, что и h3_wifi_efuse_read_mac). Возвращает 0 при успехе (v
   заполнен), -1 при недоступности контроллера. */
int h3_wifi_efuse_read_byte(uint16_t addr, uint8_t *v)
{
    uint8_t a8, a9;
    int t;
    if (h3_sdc_cmd52_write(1, (8u << 13) | 0x00CF, 0x69)) return -1;
    udelay(10);
    if (h3_sdc_cmd52_write(1, (8u << 13) | 0x0031, (uint8_t)(addr & 0xFF))) return -1;
    if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0032, &a8)) return -1;
    a8 = (uint8_t)((a8 & 0xFC) | ((addr >> 8) & 0x03));
    if (h3_sdc_cmd52_write(1, (8u << 13) | 0x0032, a8)) return -1;
    if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0033, &a9)) return -1;
    if (h3_sdc_cmd52_write(1, (8u << 13) | 0x0033, (uint8_t)(a9 & 0x7F))) return -1;
    for (t = 0; t < 10000; t++) {
        if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0033, &a9)) return -1;
        if (a9 & 0x80) break;
    }
    if (t >= 10000) {
        uart_print("  efuse: READ TIMEOUT @0x");
        uart_print_hex((uint32_t)addr);
        uart_print("\r\n");
        return -1;
    }
    if (h3_sdc_cmd52_read(1, (8u << 13) | 0x0030, v)) return -1;
    /* W.310: снять EFUSE_ACCESS_ON (0xCF=0x00), как эталон в конце. */
    h3_sdc_cmd52_write(1, (8u << 13) | 0x00CF, 0x00);
    return 0;
}

/* Запись MAC в REG_MACID (0x610..0x615).
   Эталон пишет MAC через sdio_writel — CMD53 byte mode С инкрементом адреса
   (sdio_ops_linux.c:572-653). На стенде инкремент на 0x610 ранее «глючил»,
   поэтому: сначала пробуем инкрементный путь (6 байт одним запросом) и сверяем
   чтением; если не сошлось — откат на запись 4+2 байта без инкремента.
   Возвращает 0 при подтверждённой записи, -1 если не подтвердилась. */
int h3_wifi_set_mac(const uint8_t mac[6])
{
    const uint32_t base = (8u << 13) | (0x610u & 0x7FFFu);
    uint8_t rb[6];
    int i;

    /* Путь 1 — как драйвер: один инкрементный запрос на 6 байт. */
    if (h3_sdc_cmd53_write_bytes(1, base, 6, mac) == 0) {
        udelay(2000);
        if (h3_sdc_cmd53_read_bytes(1, base, 6, rb) == 0) {
            int ok = 1;
            for (i = 0; i < 6; i++)
                if (rb[i] != mac[i]) { ok = 0; break; }
            if (ok) return 0;
            uart_print("  mac: inc-path mismatch, fallback to noinc\r\n");
        }
    }

    /* Путь 2 — обход без инкремента (4 + 2 байта), как было на стенде. */
    {
        uint32_t lo = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8)
                    | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
        uint32_t hi = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
        uint32_t rl = 0, rh = 0;

        if (h3_sdc_cmd53_write_bytes_noinc(1, base, 4, &lo) != 0) return -1;
        if (h3_sdc_cmd53_write_bytes_noinc(1, base + 4, 2, &hi) != 0) return -1;
        udelay(2000);
        if (h3_sdc_cmd53_read_bytes_noinc(1, base, 4, &rl) != 0) return -1;
        if (h3_sdc_cmd53_read_bytes_noinc(1, base + 4, 2, &rh) != 0) return -1;
        if (rl != lo || (rh & 0xFFFFu) != hi) return -1;
    }
    return 0;
}

/* ---- Доступ к RF-регистрам чипа (RF6052, LSSI Path A) ----
 * Протокол 8188F (phy_RFSerialRead/Write_8188F, rtl8188f_phycfg.c):
 *   запись: BB 0x840 = (rf_off << 20) | (data & 0xFFFFF);
 *   чтение: BB 0x824 (rFPGA0_XA_HSSIParameter2) — адрес в [30:23] (<<23),
 *     edge-триггер bit31 (снять -> поднять), пауза; readback из BB 0x8A0
 *     (LSSI) или 0x8B8 (PI, если BB 0x820 bit8=1); значение & 0xFFFFF. */
static void rf_write(uint8_t off, uint32_t data)
{
    h3_sdc_write_wlan_reg32(0x840, (((uint32_t)off & 0xFF) << 20) | (data & 0xFFFFF));
}

static uint32_t rf_read(uint8_t off)
{
    uint32_t t2 = 0, p1 = 0, rb = 0;

    /* 1) адрес в 0x824 [30:23], edge снят (триггер чтения не активен). */
    h3_sdc_read_wlan_reg32(0x824, &t2);
    t2 = (t2 & ~0x7F800000u) | (((uint32_t)off & 0xFF) << 23);
    h3_sdc_write_wlan_reg32(0x824, t2 & ~0x80000000u);
    /* 2) повторить со снятым edge, затем поднять edge=1 (запуск LSSI-чтения). */
    h3_sdc_read_wlan_reg32(0x824, &t2);
    h3_sdc_write_wlan_reg32(0x824, t2 & ~0x80000000u);
    h3_sdc_write_wlan_reg32(0x824, t2 | 0x80000000u);
    udelay(100);
    /* 3) readback: PI (0x8B8) или LSSI (0x8A0) по биту 8 в 0x820. */
    h3_sdc_read_wlan_reg32(0x820, &p1);
    if (p1 & (1u << 8))
        h3_sdc_read_wlan_reg32(0x8B8, &rb);
    else
        h3_sdc_read_wlan_reg32(0x8A0, &rb);
    return rb & 0xFFFFF;
}

/* ---- LCK: калибровка утечки гетеродина (phy_LCCalibrate_8188F, 1T1R path A) ----
 * Полная последовательность:
 *  0) BB/PLL: BB-конфиг (bb_phy_reg, 209), AGC-таблица (bb_agc_tab, 66),
 *     RF-таблица RadioA (rf_radioa, 136 пар С ПРОПУСКОМ записи 0x18 —
 *     таблица ставит 0xFC07, что ломает LCK-триггер), затем RF 0x18 = 0x401;
 *  1) REG_TXPAUSE(0x522) = 0xFF — блокировать TX-очереди (чип не передаёт);
 *  2) RF 0xB0 = 0xDFBF0 (LDO ON);
 *  3) RF 0x00 = 0 — перевести RF в LCK-режим (как в драйвере перед триггером);
 *  4) RF_CHNLBW (0x18) |= 0x08000 (бит 15 = LCK trigger);
 *  5) опрос: ждать сброса bit15 (LCK завершён), таймаут ~1 с (100 x 10 мс);
 *  6) RF 0xB0 = 0xDFFF0 (LDO OFF);
 *  7) восстановить канал: RF 0x18 = 0x401 (явно, с очисткой бит [15:8]);
 *  8) восстановить RF 0x00 = 0x33D95 (последняя запись RF-таблицы,
 *     ant-switch; НЕ 0x30000 — дефолт ломает тракт, заданный таблицей);
 *  9) REG_TXPAUSE = 0x00 — снять блокировку.
 * После LCK RF-таблица ОСТАЁТСЯ загруженной (чип не сбрасывается между
 * фазами), поэтому IQK и RX-тест НЕ перезагружают её повторно. */
void h3_wifi_rf_lck(void)
{
    uint32_t lc, ch;
    uint32_t rf_ac;   /* W: сохр. факт. RF_AC — эталон для packet-TX его НЕ трогает */

    uart_print("  lck: start\r\n");
    /* 0) Включить BB/RF — как драйвер (PHY_BBConfig8188F + _DisableRFAFEAndResetBB):
          a) SYS_FUNC_EN (0x02) 16-бит |= BIT13|BIT0|BIT1 (питание RF/BB);
          b) сброс BB-машины: SYS_FUNC_EN байт0 = 0x16 (BB_RSTn=1), затем 0x14
             (BB_GLB_RSTn=0 -> BB в сбросе);
          c) REG_RF_CTRL (0x1F) = 0x07 (RF_EN|RF_RSTB|RF_SDMRSTB) и RF reg 1 =
             0x780 (RF power-on) — ПОСЛЕ сброса BB (иначе сброс гасит RF);
          d) SYS_FUNC_EN байт0 = 0x17 (BB из сброса);
          e) REG_CR (0x100) = 0x63F;
          f) загрузить полный BB-конфиг (bb_phy_reg[]) — настраивает RF-интерфейс
             (0x840 LSSI mode, 0x860/0x864/0x870 SW control), без него LSSI
             не доходит до RF (readback=0). */
    {
        uint16_t sf16 = 0;
        uint8_t sf8 = 0;
        h3_sdc_read_wlan_reg16(0x0002, &sf16);
        WLOG("  lck: sf_pre=0x"); WLOG_HEX(sf16); WLOG("\r\n");
        h3_sdc_write_wlan_reg16(0x0002, (uint16_t)(sf16 | 0x2003u));  /* BIT13|BIT0|BIT1 */
        udelay(50);
        h3_sdc_write_wlan_reg8(0x02, 0x16);       /* BB: глобальный RSTn снят */
        udelay(50);
        h3_sdc_write_wlan_reg8(0x02, 0x14);       /* BB в сбросе */
        udelay(50);
        h3_sdc_write_wlan_reg8(0x1F, 0x07);       /* REG_RF_CTRL: RF_EN|RF_RSTB|RF_SDMRSTB */
        udelay(100);
        rf_write(0x01, 0x780);                    /* RF reg 1: RF power on (после сброса!) */
        udelay(100);
        h3_sdc_write_wlan_reg8(0x02, 0x17);       /* BB из сброса (младший байт) */
        udelay(100);
        h3_sdc_read_wlan_reg8(0x02, &sf8);
        WLOG("  lck: sf8=0x"); WLOG_HEX(sf8); WLOG("\r\n");
        h3_sdc_write_wlan_reg32(0x0100, 0x0000063Fu);
        udelay(100);
        /* W.254: MAC-таблица 8188F (Array_MP_8188F_MAC_REG). Драйвер грузит
           её в PHY_MACConfig8188F (sdio_halinit.c:1088) ДО BB/RF, байтовыми
           записями (odm_ConfigMAC_8188F -> ODM_Write1Byte). Мы её пропускали
           целиком — не ставились 0x283 (RXDMA-область), 0x421 (FWHW_TXQ_CTRL+1),
           0x4C8/0x4C9, 0x700-0x70B (protocol-контроль), 0x652/0x66E и др.
           Грузим ДО LCK, поэтому LCK-запись 0x24 (crystalcap) её перекрывает. */
        {
            unsigned int mi;
            for (mi = 0; mi < sizeof(mac_reg_8188f)/sizeof(mac_reg_8188f[0]); mi += 2)
                h3_sdc_write_wlan_reg8(mac_reg_8188f[mi], (uint8_t)mac_reg_8188f[mi+1]);
            udelay(100);
            uart_print("  lck: mac_reg loaded (");
            uart_print_int((int)(sizeof(mac_reg_8188f)/sizeof(mac_reg_8188f[0])/2));
            uart_print(" regs)\r\n");
        }
        {
            unsigned int k2;
            for (k2 = 0; k2 < sizeof(bb_phy_reg)/sizeof(bb_phy_reg[0]); k2 += 2)
                h3_sdc_write_wlan_reg32(bb_phy_reg[k2], bb_phy_reg[k2+1]);
            udelay(100);
            uart_print("  lck: bb_config loaded (");
            uart_print_int((int)(sizeof(bb_phy_reg)/sizeof(bb_phy_reg[0])/2));
            uart_print(" regs)\r\n");
        }
        /* f2) AGC_TAB — RSSI-таблица приёмного тракта. Драйвер грузит её
           ОТДЕЛЬНО после PHY_REG (phy_BB8188f_Config_ParaFile: шаг 2 —
           "Read BB AGC table Initialization"). Без неё 0xC78/0xC50 остаются
           в дефолтных значениях из PHY_REG и RX работает с дефолтной
           таблицей усиления. */
        {
            unsigned int k2;
            for (k2 = 0; k2 < sizeof(bb_agc_tab)/sizeof(bb_agc_tab[0]); k2 += 2)
                h3_sdc_write_wlan_reg32(bb_agc_tab[k2], bb_agc_tab[k2+1]);
            udelay(100);
            uart_print("  lck: agc_tab loaded (");
            uart_print_int((int)(sizeof(bb_agc_tab)/sizeof(bb_agc_tab[0])/2));
            uart_print(" regs)\r\n");
        }
        {
            uint32_t a50, a54, a58, a5c, a78;
            h3_sdc_read_wlan_reg32(0x0C50, &a50);
            h3_sdc_read_wlan_reg32(0x0C54, &a54);
            h3_sdc_read_wlan_reg32(0x0C58, &a58);
            h3_sdc_read_wlan_reg32(0x0C5C, &a5c);
            h3_sdc_read_wlan_reg32(0x0C78, &a78);
            WLOG("  lck: agc c50=0x"); WLOG_HEX(a50);
            WLOG(" c54=0x"); WLOG_HEX(a54);
            WLOG(" c58=0x"); WLOG_HEX(a58);
            WLOG(" c5c=0x"); WLOG_HEX(a5c);
            WLOG(" c78=0x"); WLOG_HEX(a78);
            WLOG("\r\n");
        }
    }
    /* 0g) Загрузить полную RF-таблицу RadioA (rf_radioa[], 136 пар) —
           как драйвер phy_RF6052_Config_ParaFile (rtl8188f_rf6052.c:147-249):
           BB config → RF config → LCK. РАНЬШЕ RF-таблица грузилась ТОЛЬКО
           внутри IQK (отключён), поэтому после LCK аналоговый тракт
           (LNA/mixer/PLL/bias, регистры 0x30/0x31/0x32/0xDF/0x56/0xB0 и др.)
           оставался в power-on дефолте и RX не мог принимать сигнал.
           Условные записи (0x80000000/0x40000000/...) в таблице пропущены
           (массив уже отфильтрован). */
    {
        unsigned int k2;
        uint32_t rtmp;
        for (k2 = 0; k2 < sizeof(rf_radioa)/sizeof(rf_radioa[0]); k2 += 2) {
            /* Пропускаем записи в RF 0x18 (CHNLBW) — таблица ставит
               0xFC07 (канал 7 + тестовые биты 11-14 + bit15=1), что ломает
               LCK-триггер (done за 0 мс). Канал выставим явно после цикла. */
            if ((rf_radioa[k2] & 0xFF) == 0x18)
                continue;
            rf_write((uint8_t)(rf_radioa[k2] & 0xFF), rf_radioa[k2+1]);
            udelay(1);
        }
        /* Явно ставим RF 0x18 = 0x401 (канал 1, BW20 МГц) — как драйвер
           RfRegChnlVal[0]=0x201/0x401 после PHY_RF6052_Config. */
        rf_write(0x18, 0x401u);
        udelay(100);
        udelay(200);
        uart_print("  lck: rf_tab loaded (");
        uart_print_int((int)(sizeof(rf_radioa)/sizeof(rf_radioa[0])/2));
        uart_print(" regs)\r\n");
        WLOG("  lck: rfread df=0x"); WLOG_HEX(rf_read(0xDF));
        WLOG(" 56=0x"); WLOG_HEX(rf_read(0x56));
        WLOG(" 30=0x"); WLOG_HEX(rf_read(0x30));
        WLOG(" 31=0x"); WLOG_HEX(rf_read(0x31));
        WLOG(" 32=0x"); WLOG_HEX(rf_read(0x32));
        WLOG(" b0=0x"); WLOG_HEX(rf_read(0xB0));
        rtmp = rf_read(0x00);
        WLOG(" 00=0x"); WLOG_HEX(rtmp);
        WLOG("\r\n");
        /* rf18 после таблицы: должен быть 0x401 (чисто). */
        {
            uint32_t chnl = rf_read(0x18);
            WLOG("  lck: rf18_after_tab=0x");
            WLOG_HEX(chnl);
            WLOG("\r\n");
            (void)chnl;   /* при выкл. verbose не используется */
        }
        /* 0h) CrystalCap из efuse — как драйвер (rtl8188f_hal_init.c:4461
             Hal_EfuseParseXtal_8188F -> hal_com.c:7811 hal_set_crystal_cap):
             кристалл 40 МГц настраивается по байту EEPROM_XTAL (0xB9) в
             REG_AFE_XTAL_CTRL (0x24) биты [16:11] и [22:17] (два идентичных
             поля). Рассинхрон кристалла сильно бьёт по RX-чувствительности
             при живом TX. Дефолт (0x20) применяется, если efuse не прочитан
             (AutoLoadFail/0xFF). */
        {
            uint8_t xcap = 0xFF;
            int xr = h3_wifi_efuse_read_byte(0xB9, &xcap);
            if (xr != 0 || xcap == 0xFF)
                xcap = 0x20;  /* EEPROM_Default_CrystalCap_8188F */
            xcap &= 0x3F;
            {
                /* W.310: CrystalCap по эталону hal_com.c:7811
                   PHY_SetBBReg(REG_AFE_XTAL_CTRL, 0x007FF800, crystal_cap|crystal_cap<<6).
                   Маска [22:11] (12 бит), value сдвигается на 11. Старый код
                   делал <<17, задевая [27:23] — коррапт при xcap с битами 4-5. */
                uint32_t cc = (uint32_t)(xcap | (xcap << 6)) & 0x0FFFu;
                h3_sdc_read_wlan_reg32(0x0024, &rtmp);
                rtmp = (rtmp & ~0x007FF800u) | (cc << 11);
                h3_sdc_write_wlan_reg32(0x0024, rtmp);
            }
            WLOG("  lck: crystalcap=eF=0x");
            WLOG_HEX(xr != 0 ? 0xFF : (uint32_t)xcap);
            WLOG(" 0x24=0x");
            {
                uint32_t xt24 = 0;
                h3_sdc_read_wlan_reg32(0x0024, &xt24);
                WLOG_HEX(xt24);
            }
            WLOG("\r\n");
        }
    }
    /* Проверка живости BB/RF (0xE30 rTx_IQK_Tone_A пишем и читаем; RF 0x18
       через LSSI — должен вернуть не 0). */
    {
        uint32_t tst = 0;
        h3_sdc_write_wlan_reg32(0x0E30, 0x18008C1Cu);
        udelay(10);
        h3_sdc_read_wlan_reg32(0x0E30, &tst);
        WLOG("  lck: bb_tone=0x");
        WLOG_HEX(tst);
        WLOG(" rf18=0x");
        WLOG_HEX(rf_read(0x18));
        WLOG("\r\n");
    }
    /* 1) блокировать TX. */
    h3_sdc_write_wlan_reg8(0x0522, 0xFF);
    /* 2) Выставить канал 1 (2412 МГц) в RF_CHNLBW — LCK калибрует утечку
          гетеродина на текущей частоте. ВАЖНО: после RF-таблицы RF 0x18
          = 0x401 (мы её грузим с пропуском записи 0x18 и явно ставим
          0x401). Устанавливаем чистый 0x401 явно — как RfRegChnlVal[0]
          драйвера после PHY_RF6052_Config (канал 1, BW20). */
    rf_write(0x18, 0x401u);
    udelay(100);
    /* Лог после установки канала (ожидаем 0x401 — чисто). */
    ch = rf_read(0x18);
    WLOG("  lck: chnlbw_set=0x");
    WLOG_HEX(ch);
    WLOG("\r\n");
    (void)ch;   /* при выкл. verbose не используется */
    /* 3) LDO ON. */
    rf_write(0xB0, 0xDFBF0);
    /* 4) Сохранить RF_AC (RF 0x00) и НЕ занулять его: эталон
          phy_LCCalibrate_8188F (halphyrf_8188f.c:2096-2120) для packet-TX
          (0xd03 & 0x70 == 0) RF_AC не трогает — только TXPAUSE + 0xB0 + 0x18.
          Принудительное 0x33D95 после LCK не совпадает с живым чипом
          (0x28017, D7) и расстраивает аналоговый тракт. */
    {
        rf_ac = rf_read(0x00);
        WLOG("  lck: rf_ac_save=0x");
        WLOG_HEX(rf_ac);
        WLOG("\r\n");
    }
    udelay(50);
    lc = rf_read(0x00);
    WLOG("  lck: rf00=0x");
    WLOG_HEX(lc);
    WLOG("\r\n");
    /* 5) триггер LCK: bit15 = 1. */
    lc = rf_read(0x18);
    rf_write(0x18, lc | 0x08000);
    WLOG("  lck: chnlbw_trig=0x");
    WLOG_HEX(lc | 0x08000);
    WLOG("\r\n");
    /* 6) LCK-пауза: эталон phy_LCCalibrate_8188F (halphyrf_8188f.c:2133)
          ждёт фиксированные 100 мс БЕЗ поллинга бита 15 — у 8188F бит 15
          может оставаться взведённым после завершения (D7). */
    udelay(100000);
    lc = rf_read(0x18);
    WLOG("  lck: chnlbw_done=0x");
    WLOG_HEX(lc);
    WLOG("\r\n");
    /* 7) LDO OFF. */
    rf_write(0xB0, 0xDFFF0);
/* 8) Восстановить канал: LCK затирает биты [7:0] RF 0x18; ставим явно
          чистый 0x401 (канал 1, BW20). */
    rf_write(0x18, 0x401u);
    udelay(100);
    lc = rf_read(0x18);
    WLOG("  lck: chnlbw_restored=0x");
    WLOG_HEX(lc);
    WLOG("\r\n");
    /* 9) Восстановить RF_AC (RF 0x00) из сохранённого значения. LCK не
          должен менять режим RF-тракта; возвращаем то, что было до LCK
          (не жёсткий 0x33D95 из таблицы, который расходится с живым
          0x28017 и расстраивает аналоговый тракт). */
    rf_write(0x00, rf_ac);
    udelay(100);
    lc = rf_read(0x00);
    WLOG("  lck: rf00_restored=0x");
    WLOG_HEX(lc);
    WLOG("\r\n");
    /* 10) снять блокировку TX. */
    h3_sdc_write_wlan_reg8(0x0522, 0x00);
    uart_print("  lck: done\r\n");
}

/* ---- IQK: калибровка дисбаланса I/Q (phy_IQCalibrate_8188F, 1T1R path A) ----
 * Порт 8188F-версии (halphyrf_8188f.c), строго по драйверу:
 *  - Tx IQK (phy_PathA_IQK_8188F): вход в IQK-режим ЧЕРЕЗ 0x80800000
 *    (bMaskH3Bytes=0xffffff00 сдвигает 0x808000 на 8 влево), RF 0x30/0x31/0x32
 *    с RF_WE_LUT bit19=1 (RMW), one-shot 0xe48=0xF9000000 -> 0xF8000000,
 *    25 мс пауза, проверка regEAC/E94/E9C;
 *  - Rx IQK (phy_PathA_RxIQK8188F): двухпроходный — combined Tx/Rx
 *    (у4tmp в rTx_IQK из e94/e9c) и чистый Rx-рефайн (rTx_IQK=u4tmp из
 *    прошлого прохода, reload RF 0x8 = LOK); проверка regEAC/EA4.
 * Эталонные значения — ИМЕННО 8188F. MAC/ADDA-подготовка драйвера
 * (BCN_CTRL, GPIO_MUXCFG, ADDA-пути) опущена: калибровка идёт до трафика,
 * TXPAUSE блокирует передачу. */
void h3_wifi_rf_iqk(void)
{
    /* BB-регистры IQK (Hal8188FPhyReg.h). */
    const uint32_t rFPGA0_IQK      = 0x0E28;
    const uint32_t rTx_IQK_Tone_A  = 0x0E30;
    const uint32_t rRx_IQK_Tone_A  = 0x0E34;
    const uint32_t rTx_IQK_PI_A    = 0x0E38;
    const uint32_t rRx_IQK_PI_A    = 0x0E3C;
    const uint32_t rTx_IQK         = 0x0E40;
    const uint32_t rRx_IQK         = 0x0E44;
    const uint32_t rIQK_AGC_Pts    = 0x0E48;
    const uint32_t rIQK_AGC_Rsp    = 0x0E4C;
    const uint32_t rTxPwBefore     = 0x0E94;
    const uint32_t rTxPwAfter      = 0x0E9C;
    const uint32_t rRxPwBefore     = 0x0EA4;
    const uint32_t rRxPwAfter      = 0x0EAC;

    uint32_t e94, e9c, eac, ea4, u4tmp, we_lut, v;
    uint32_t lok_result = 0;            /* W.265-D8: LOK_Result из RF 0x08 */
    unsigned int i;
    static uint32_t psel_bb = 0, psel_rf = 0;   /* W.262: Path_SEL save/restore */
    static uint32_t bb_800 = 0, bb_c04 = 0, bb_c08 = 0, bb_874 = 0; /* W.263: BB save/restore */

    uart_print("  iqk: start\r\n");

    /* W.262-D4: сохранить Path_SEL_BB (BB 0x948) и Path_SEL_RF (RF 0xB0)
       ДО калибровки — драйвер reload'ит их после (halphyrf_8188f.c:
       1857-1858 сохраняет, 2054-2055 восстанавливает). Без этого после
       IQK RF-путь остаётся в состоянии калибровки: RF 0x08 уезжает на
       0x85000 (живой чип 0xF800), RF 0xB0=0xFF9F0 вместо 0xFFFF0 —
       приёмник физически расстроен (rxrpt=0 в логе W.261). */
    {
        h3_sdc_read_wlan_reg32(0x0948, &psel_bb);
        psel_rf = rf_read(0xB0);
        WLOG("  iqk: psel 948=0x"); WLOG_HEX(psel_bb);
        WLOG(" rfB0=0x"); WLOG_HEX(psel_rf);
        WLOG("\r\n");
    }

    /* Диагностика: RF 0x00 (RF_AC) — после LCK должен быть 0x33D95
       (последняя запись RF-таблицы). НЕ перезаписываем его в 0x30000:
       это НЕ штатное значение для IQK — драйвер в phy_IQCalibrate_8188F
       RF_AC не трогает (RF_AC=0x10000 ставит только phy_LCK_8188F на время
       LCK и возвращает). Оставляем как есть, из RF-таблицы. */
    v = rf_read(0x00);
    WLOG("  iqk: rf00_pre=0x");
    WLOG_HEX(v);
    WLOG("\r\n");

    /* RF_WE_LUT (0xEF) bit19=1 ставится ВНУТРИ каждой фазы Tx/Rx —
       RF-таблица базового блока заканчивается записью 0xEF=0x1A0,
       которая сбрасывает бит 19 (write-enable LUT). Если ставить его
       до таблицы, защищённые регистры (0x30/0x31/0x32/0xDF/0x56)
       «пишутся» только в shadow и не применяются к аналоговому тракту,
       из-за чего IQK даёт насыщение 0x142/0x42. */

    /* W.279: H2C 0x6D={1} BT_WLAN_CALIBRATION УБРАН. В W.276-W.278 FW НЕ
       обрабатывает эту команду: box0 (0x1CC bit0) навсегда остаётся занят
       (hmetfr=0x01 весь свип), все последующие H2C (SET_PWR_MODE уже прошёл,
       AP_REQ_TXRPT в c2h_ping) теряются, C2H не приходит, пакеты не
       выкладываются в SDIO FIFO. 0x6D — опциональная обвязка BT/WLAN coex;
       у нас нет BT, а IQK host-driven (мы сами пишем BB/RF, триггерим one-shot,
       читаем e94/e9c/eac) и проходит без участия FW. Убираем отправку, чтобы
       освободить box0 для настоящих H2C. 0x1E7 оставлен как диагностика. */
    {
        uint8_t en = 0;
        h3_sdc_read_wlan_reg8(0x03, &en);
        WLOG("  iqk: mcu_pre=0x"); WLOG_HEX((uint32_t)en); WLOG("\r\n");
        uart_print("  iqk: 0x6D removed (box0 free)\r\n");
    }

    /* MAC-настройки калибровки (_PHY_MACSettingCalibration8188F,
       halphyrf_8188f.c:1569-1573, вызывается из phy_IQCalibrate_8188F
       :1883): TXPAUSE=0x3F (НЕ 0xFF как раньше — разблокировать high-
       очередь), BCN_CTRL(0x550)/BCN_CTRL_1(0x551) &= ~BIT3 (EN_BCN off),
       GPIO_MUXCFG(0x40) &= ~BIT5. У нас этого блока НЕ БЫЛО — ставили
       просто 0xFF. */
    {
        uint8_t mreg;
        h3_sdc_write_wlan_reg8(0x0522, 0x3F);        /* TXPAUSE: 0x3F */
        h3_sdc_read_wlan_reg8(0x0550, &mreg);
        h3_sdc_write_wlan_reg8(0x0550, (uint8_t)(mreg & ~(1u << 3))); /* BCN_CTRL */
        h3_sdc_read_wlan_reg8(0x0551, &mreg);
        h3_sdc_write_wlan_reg8(0x0551, (uint8_t)(mreg & ~(1u << 3))); /* BCN_CTRL_1 */
        h3_sdc_read_wlan_reg8(0x0040, &mreg);
        h3_sdc_write_wlan_reg8(0x0040, (uint8_t)(mreg & ~(1u << 5))); /* GPIO_MUXCFG */
        uart_print("  iqk: mac_cal set\r\n");
    }

    /* BB-подготовка тракта (phy_IQCalibrate_8188F, до циклов):
       RFMOD bit24=0, путь, мультиплексор, RF-интерфейс.
       W.263: СНАЧАЛА сохранить эти BB-регистры — драйвер reload'ит их
       после калибровки (IQK_BB_REG_92C, halphyrf_8188f.c:1785-1790,
       2041: rOFDM0_TRxPathEnable/rOFDM0_TRMuxPar/rFPGA0_XCD_RFInterfaceSW/
       rFPGA0_RFMOD). Без восстановления RFMOD остаётся с bit24=0
       (CCK-блок выключен!) — приёмник физически не принимает после IQK. */
    h3_sdc_read_wlan_reg32(0x0800, &bb_800);
    h3_sdc_read_wlan_reg32(0x0C04, &bb_c04);
    h3_sdc_read_wlan_reg32(0x0C08, &bb_c08);
    h3_sdc_read_wlan_reg32(0x0874, &bb_874);
    uart_print("  iqk: bb_save 800=0x"); uart_print_hex(bb_800);
    uart_print(" c04=0x"); uart_print_hex(bb_c04);
    uart_print(" c08=0x"); uart_print_hex(bb_c08);
    uart_print(" 874=0x"); uart_print_hex(bb_874);
    uart_print("\r\n");
    h3_sdc_read_wlan_reg32(0x0800, &v);
    h3_sdc_write_wlan_reg32(0x0800, v & ~(1u << 24));
    h3_sdc_write_wlan_reg32(0x0C04, 0x03A05600);   /* rOFDM0_TRxPathEnable */
    h3_sdc_write_wlan_reg32(0x0C08, 0x000800E4);   /* rOFDM0_TRMuxPar */
    h3_sdc_write_wlan_reg32(0x0874, 0x25204000);   /* rFPGA0_XCD_RFInterfaceSW */

    /* PI-режим RF-интерфейса (_PHY_PIModeSwitch8188F, halphyrf_8188f.c:
       1606-1630, вызывается из phy_IQCalibrate_8188F :1847-1854 ПЕРЕД
       калибровкой): 0x860/0x868 = 0x01000100. РАНЬШЕ ЭТОГО ШАГА НЕ БЫЛО —
       без PI-режима движок IQK не получает тон и даёт насыщение
       0x142/0x42. В конце восстанавливается 0x01000000 (SI-режим,
       :2025-2032). */
    h3_sdc_write_wlan_reg32(0x0860, 0x01000100);   /* XA_HSSIParameter1 PI */
    h3_sdc_write_wlan_reg32(0x0868, 0x01000100);   /* XB_HSSIParameter1 PI */
    udelay(100);
    {
        uint32_t pi_rd;
        h3_sdc_read_wlan_reg32(0x0860, &pi_rd);
        uart_print("  iqk: pi_on 860=0x"); uart_print_hex(pi_rd);
        h3_sdc_read_wlan_reg32(0x0868, &pi_rd);
        uart_print(" 868=0x"); uart_print_hex(pi_rd);
        uart_print("\r\n");
    }

    /* RF_CHNLBW: канал 1 (0x401) — как драйвер после PHY_RF6052_Config.
          RF-таблица уже загружена, канал чистый 0x401 (бит15 не ставим —
          он переводит чип в режим калибровки). */
    rf_write(0x18, 0x401u);
    udelay(50);
    uart_print("  iqk: rf18=0x");
    uart_print_hex(rf_read(0x18));
    uart_print("\r\n");

    /* ADDA ON (_PHY_PathADDAOn8188F): включить пути ADDA (16 рег.,
       все = 0x03c00014, как в драйвере для 1T1R). Без активного тракта
       движок IQK не видит тон и даёт насыщение 0x142/0x42. */
    {
        static const uint32_t adda_reg[16] = {
            0x085C, 0x0E6C, 0x0E70, 0x0E74, 0x0E78, 0x0E7C, 0x0E80,
            0x0E84, 0x0E88, 0x0E8C, 0x0ED0, 0x0ED4, 0x0ED8, 0x0EDC,
            0x0EE0, 0x0EEC
        };
        unsigned int k;
        for (k = 0; k < 16; k++)
            h3_sdc_write_wlan_reg32(adda_reg[k], 0x03C00014);
    }

    /* RF-таблица (Array_RadioA_8188F) — НЕ перезагружаем: она уже загружена
          в LCK (h3_wifi_rf_lck, с пропуском 0x18).
          Повторная загрузка здесь перезатрёт канал 1 и поднимет опасные
          биты таблицы. Только убеждаемся, что RF 0x18 = 0x401. */
    {
        uint32_t rf18 = rf_read(0x18);
        if ((rf18 & 0xFFu) != 0x01u) {
            rf_write(0x18, (rf18 & ~0xFFu) | 0x01u);  /* канал 1 на всякий случай */
            udelay(50);
        }
        uart_print("  iqk: rf18_pre=0x");
        uart_print_hex(rf_read(0x18));
        uart_print("\r\n");
    }

    /* ===== Tx IQK (phy_PathA_IQK_8188F), retry=2 ===== */
    for (i = 0; i < 2; i++) {
        uart_print("  iqk: tx try=");
        uart_print_int((int)i);
        uart_print("\r\n");

        /* выйти из IQK-режима, включить RF_WE_LUT (bit19=1) — разрешить
           запись защищённых LUT-регистров, затем настроить RF-усиление. */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        we_lut = rf_read(0xEF);
        rf_write(0xEF, (we_lut & 0xFFFFF) | 0x80000u);
        udelay(20);
        rf_write(0x30, 0x20000);        /* RF_RCK_OS  */
        rf_write(0x31, 0x0000F);        /* RF_TXPA_G1 */
        rf_write(0x32, 0x07FF7);        /* RF_TXPA_G2 */
        rf_write(0xDF, 0x00980);        /* PA/PAD gain */
        rf_write(0x56, 0x5102A);        /* 8188F */

        /* Readback RF: доходят ли записи (0x30/0x31/0x32/0xDF/0x56),
           и не сбил ли их 8051/физика между попытками. */
        WLOG("  iqk: rf30=0x"); WLOG_HEX(rf_read(0x30));
        WLOG(" rf31=0x"); WLOG_HEX(rf_read(0x31));
        WLOG(" rf32=0x"); WLOG_HEX(rf_read(0x32));
        WLOG(" rfdf=0x"); WLOG_HEX(rf_read(0xDF));
        WLOG(" rf56=0x"); WLOG_HEX(rf_read(0x56));
        WLOG(" rfEF=0x"); WLOG_HEX(rf_read(0xEF));
        WLOG("\r\n");
        (void)rf_read(0xEF);   /* (без вывода) */
        /* 0x948 (Path_SEL_BB): НЕ трогаем! 0x948=0x80 ставит драйвер
           только в phy_PathB_IQK_8188F («switch to path B», halphyrf_8188f.
           c:942). У нас Path A — запись 0x948=0x80 переключала движок на
           путь B и ломала калибровку пути A. Оставляем 0x99000000 из
           BB-таблицы. */
        {
            uint32_t d;
            h3_sdc_read_wlan_reg32(0x0948, &d);
            WLOG("  iqk: sel948=0x"); WLOG_HEX(d); WLOG("\r\n");
        }

        /* e94/e9c/eac ДО триггера — убедиться, что они обновляются
           движком, а не висят на дефолте. */
        {
            uint32_t d;
            h3_sdc_read_wlan_reg32(0x0E94, &d);
WLOG("  iqk: pre e94=0x"); WLOG_HEX(d);
            WLOG(" e9c=0x"); WLOG_HEX(d);
            WLOG(" eac=0x"); WLOG_HEX(d);
            WLOG("\r\n");
        }
        /* Сброс 0xE48 (rIQK_AGC_Pts) перед триггером — дефолт 0xFB000000
           из BB-конфига не должен влиять на запуск one-shot. */
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0x00000000);
        udelay(100);

        /* войти в IQK-режим: 0x808000 << 8 (bMaskH3Bytes). */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x80800000);

        /* IQK setting (phy_IQCalibrate_8188F:1900-1901): режимы TX/RX-тона.
           Без rTx_IQK=0x01007C00 движок Tx работает в нулевом режиме
           и не может сгенерировать тон — IQK даёт насыщение 0x142/0x42. */
        h3_sdc_write_wlan_reg32(rTx_IQK, 0x01007C00);
        h3_sdc_write_wlan_reg32(rRx_IQK, 0x01004800);

        /* path-A тоны и PI (8188F). */
        h3_sdc_write_wlan_reg32(rTx_IQK_Tone_A, 0x18008C1C);
        h3_sdc_write_wlan_reg32(rRx_IQK_Tone_A, 0x38008C1C);
        h3_sdc_write_wlan_reg32(rTx_IQK_PI_A,   0x821403FF);
        h3_sdc_write_wlan_reg32(rRx_IQK_PI_A,   0x28160000);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Rsp,   0x00462911);

        /* Контрольный readback: если 8051 перезаписывает настройки,
           движок IQK молча выдаёт насыщение 0x142/0x42. */
        {
            uint32_t rb;
            h3_sdc_read_wlan_reg32(rFPGA0_IQK, &rb);
            WLOG("  iqk: e28=0x"); WLOG_HEX(rb);
            h3_sdc_read_wlan_reg32(rTx_IQK_Tone_A, &rb);
            WLOG(" e30=0x"); WLOG_HEX(rb);
            h3_sdc_read_wlan_reg32(rRx_IQK_Tone_A, &rb);
            WLOG(" e34=0x"); WLOG_HEX(rb);
            h3_sdc_read_wlan_reg32(rTx_IQK_PI_A, &rb);
            WLOG(" e38=0x"); WLOG_HEX(rb);
            h3_sdc_read_wlan_reg32(rIQK_AGC_Rsp, &rb);
            WLOG(" e4c=0x"); WLOG_HEX(rb);
            h3_sdc_read_wlan_reg32(rIQK_AGC_Pts, &rb);
            WLOG(" e48=0x"); WLOG_HEX(rb);
            WLOG("\r\n");
        }

        /* one shot: LOK & IQK. */
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF9000000);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF8000000);
        udelay(25000);                  /* IQK_DELAY_TIME_8188F = 25 мс */

        /* reload RF 0xdf и выход из IQK-режима. */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        rf_write(0xDF, 0x00180);
        /* W.265-D8: СОХРАНИТЬ LOK_Result из RF 0x08 после Tx one-shot —
           драйвер: LOK_Result = GetRFReg(0x8) (halphyrf_8188f.c:737).
           Раньше только читали; без сохранения в Rx-фазе нечем
           восстанавливать RF 0x08, и после Rx one-shot он остаётся
           0x85000 (результат Rx-LOK) вместо эталонного 0xF800 —
           RX-тракт расстроен. */
        lok_result = rf_read(0x08);
        WLOG("  iqk: lok08=0x");
        WLOG_HEX(lok_result);
        WLOG("\r\n");

        h3_sdc_read_wlan_reg32(rTxPwBefore, &e94);
        h3_sdc_read_wlan_reg32(rTxPwAfter,  &e9c);
        h3_sdc_read_wlan_reg32(rRxPwAfter,  &eac);
        WLOG("  iqk: tx e94=0x"); WLOG_HEX(e94);
        WLOG(" e9c=0x"); WLOG_HEX(e9c);
        WLOG(" eac=0x"); WLOG_HEX(eac);
        WLOG("\r\n");
        /* критерий драйвера: Tx OK если eac[28]=0, e94[25:16]!=0x142,
           e9c[25:16]!=0x42. */
        if (!(eac & (1u << 28)) &&
            (((e94 & 0x03FF0000u) >> 16) != 0x142) &&
            (((e9c & 0x03FF0000u) >> 16) != 0x42)) {
            uart_print("  iqk: Tx OK\r\n");
            u4tmp = 0x80007C00u | (e94 & 0x3FF0000u) |
                    ((e9c & 0x3FF0000u) >> 16);
            h3_sdc_write_wlan_reg32(rTx_IQK, u4tmp);
            break;
        }
        uart_print("  iqk: Tx retry\r\n");
    }

    /* ===== Rx IQK (phy_PathA_RxIQK8188F), двухпроходный, retry=2 ===== */
    for (i = 0; i < 2; i++) {
        uart_print("  iqk: rx try=");
        uart_print_int((int)i);
        uart_print("\r\n");

        /* Проход 1 (combined Tx/Rx): RF_WE_LUT + RF-усиление + тоны. */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        we_lut = rf_read(0xEF);
        rf_write(0xEF, (we_lut & 0xFFFFF) | 0x80000u);
        udelay(20);
        rf_write(0x30, 0x30000);        /* RF_RCK_OS  */
        rf_write(0x31, 0x0000F);        /* RF_TXPA_G1 */
        rf_write(0x32, 0xF1173);        /* RF_TXPA_G2 */
        rf_write(0xDF, 0x00980);
        rf_write(0x56, 0x5102A);
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x80800000);
        h3_sdc_write_wlan_reg32(rTx_IQK, 0x01007C00);
        h3_sdc_write_wlan_reg32(rRx_IQK, 0x01004800);
        h3_sdc_write_wlan_reg32(rTx_IQK_Tone_A, 0x10008C1C);
        h3_sdc_write_wlan_reg32(rRx_IQK_Tone_A, 0x30008C1C);
        h3_sdc_write_wlan_reg32(rTx_IQK_PI_A,   0x82160FFF);
        h3_sdc_write_wlan_reg32(rRx_IQK_PI_A,   0x28160000);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Rsp,   0x00462911);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF9000000);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF8000000);
        udelay(25000);
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        rf_write(0xDF, 0x00180);
        /* W.265-D8: reload LOK_Result в RF 0x08 после Rx one-shot —
           драйвер: SetRFReg(0x8, LOK_Result) (halphyrf_8188f.c:894).
           Иначе Rx-LOK оставляет RF 0x08=0x85000 вместо 0xF800. */
        rf_write(0x08, lok_result);
        WLOG("  iqk: rx1 eac=0x"); WLOG_HEX(eac);
        WLOG(" e94=0x"); WLOG_HEX(e94);
        WLOG(" e9c=0x"); WLOG_HEX(e9c);
        WLOG("\r\n");
        /* если Tx не OK — Rx не калибруем (как в драйвере). */
        if ((eac & (1u << 28)) ||
            (((e94 & 0x03FF0000u) >> 16) == 0x142) ||
            (((e9c & 0x03FF0000u) >> 16) == 0x42)) {
            uart_print("  iqk: Rx skip (Tx fail)\r\n");
            continue;
        }
        u4tmp = 0x80007C00u | (e94 & 0x3FF0000u) |
                ((e9c & 0x3FF0000u) >> 16);
        h3_sdc_write_wlan_reg32(rTx_IQK, u4tmp);

        /* Проход 2 (чистый Rx-рефайн). */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        we_lut = rf_read(0xEF);
        rf_write(0xEF, (we_lut & 0xFFFFF) | 0x80000u);
        udelay(20);
        rf_write(0x30, 0x30000);
        rf_write(0x31, 0x0000F);
        rf_write(0x32, 0xF7FF2);        /* RF_TXPA_G2 (refine) */
        rf_write(0xDF, 0x00980);
        rf_write(0x56, 0x51000);        /* RF_RCK_OS refine */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x80800000);
        h3_sdc_write_wlan_reg32(rRx_IQK, 0x01004800);
        h3_sdc_write_wlan_reg32(rTx_IQK_Tone_A, 0x30008C1C);
        h3_sdc_write_wlan_reg32(rRx_IQK_Tone_A, 0x10008C1C);
        h3_sdc_write_wlan_reg32(rTx_IQK_PI_A,   0x82160000);
        h3_sdc_write_wlan_reg32(rRx_IQK_PI_A,   0x281613FF);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Rsp,   0x0046A911);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF9000000);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF8000000);
        udelay(25000);
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        rf_write(0xDF, 0x00180);
        /* W.265-D8: reload LOK_Result в RF 0x08 после Rx-refine one-shot. */
        rf_write(0x08, lok_result);

        h3_sdc_read_wlan_reg32(rRxPwAfter,  &eac);
        h3_sdc_read_wlan_reg32(rRxPwBefore, &ea4);
        WLOG("  iqk: rx2 eac=0x"); WLOG_HEX(eac);
        WLOG(" ea4=0x"); WLOG_HEX(ea4);
        WLOG("\r\n");
        /* критерий драйвера: Rx OK если eac[27]=0, ea4[25:16]!=0x132,
           eac[25:16]!=0x36. */
        if (!(eac & (1u << 27)) &&
            (((ea4 & 0x03FF0000u) >> 16) != 0x132) &&
            (((eac & 0x03FF0000u) >> 16) != 0x36)) {
            uart_print("  iqk: Rx OK\r\n");
            break;
        }
        uart_print("  iqk: Rx retry\r\n");
    }

    /* Выход из IQK и восстановление тонов по умолчанию (как в драйвере
       после калибровки): 0xe28=0, 0xe30/0xe34=0x01008C00,
       и SI-режим RF-интерфейса (_PHY_PIModeSwitch8188F(FALSE),
       halphyrf_8188f.c:2025-2032): 0x860/0x868 = 0x01000000. */
    h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
    h3_sdc_write_wlan_reg32(rTx_IQK_Tone_A, 0x01008C00);
    h3_sdc_write_wlan_reg32(rRx_IQK_Tone_A, 0x01008C00);
    h3_sdc_write_wlan_reg32(0x0860, 0x01000000);   /* XA_HSSIParameter1 SI */
    h3_sdc_write_wlan_reg32(0x0868, 0x01000000);   /* XB_HSSIParameter1 SI */
    /* W.262-D4: restore Path_SEL_BB (BB 0x948) и Path_SEL_RF (RF 0xB0) —
       как драйвер (halphyrf_8188f.c:2054-2055). Без этого калибровочный
       RF-путь (0xB0/0x08) остаётся и ломает RX. */
    {
        h3_sdc_write_wlan_reg32(0x0948, psel_bb);
        rf_write(0xB0, psel_rf);
        WLOG("  iqk: psel restored 948=0x");
        WLOG_HEX(psel_bb);
        WLOG(" rfB0=0x");
        WLOG_HEX(psel_rf);
        WLOG("\r\n");
    }
    /* W.263-D4: restore BB после IQK (IQK_BB_REG_92C) — RFMOD bit24
       (bCCKEn) назад, TRxPathEnable/TRMuxPar/RFInterfaceSW из бэкапа. */
    {
        h3_sdc_write_wlan_reg32(0x0800, bb_800);
        h3_sdc_write_wlan_reg32(0x0C04, bb_c04);
        h3_sdc_write_wlan_reg32(0x0C08, bb_c08);
        h3_sdc_write_wlan_reg32(0x0874, bb_874);
        WLOG("  iqk: bb restored 800=0x");
        WLOG_HEX(bb_800);
        WLOG(" c04=0x");
        WLOG_HEX(bb_c04);
        WLOG(" c08=0x");
        WLOG_HEX(bb_c08);
        WLOG(" 874=0x");
        WLOG_HEX(bb_874);
        WLOG("\r\n");
    }
    h3_sdc_write_wlan_reg8(0x0522, 0x00);   /* TXPAUSE снять */
    /* Восстановить BCN_CTRL (EN_BCN) — _PHY_ReloadMACRegisters8188F
       (halphyrf_8188f.c:2039): 0x550/0x551 возвращаем бит 3. */
    {
        uint8_t mreg;
        h3_sdc_read_wlan_reg8(0x0550, &mreg);
        h3_sdc_write_wlan_reg8(0x0550, (uint8_t)(mreg | (1u << 3)));
        h3_sdc_read_wlan_reg8(0x0551, &mreg);
        h3_sdc_write_wlan_reg8(0x0551, (uint8_t)(mreg | (1u << 3)));
    }

    /* W.279: H2C 0x6D={0} УБРАН (вместе с 0x6D={1} до IQK). Как и при входе,
       FW не обрабатывает 0x6D, и отправка финального кадра вешала box0.
       Вместо него — диагностика HMETFR: подтвердить, что box0 свободен
       после IQK (эталонный путь: после калибровки FW свободна для H2C). */
    {
        uint32_t hm4 = 0;
        h3_sdc_read_wlan_reg32(0x01CC, &hm4);
        WLOG("  iqk: hmetfr_after=0x");
        WLOG_HEX(hm4);
        WLOG("\r\n");
    }
    /* W.345 (аудит 8): вернуть RF 0x30/0x31/0x32/0x56 к ЭТАЛОННЫМ 8188F
       значениям TX (halphyrf_8188f.c:855-857): 0x30=0x30000, 0x31=0xF,
       0x32=0xF7FF2, 0x56=0x5102A. НЕ использовать предложенные в плане
       0x1A060/0x28000/0x7EFE — их нет в 8188F (0x7EFE это RF 0xB0 Path_SEL,
       не TXPA). После refine IQK эти значения уже выставлены (строки
       1555-1559), reload здесь — контроль/страховка для JOINBSS. */
    {
        rf_write(0x30, 0x30000);     /* RF_RCK_OS */
        rf_write(0x31, 0x0000F);     /* RF_TXPA_G1 */
        rf_write(0x32, 0xF7FF2);     /* RF_TXPA_G2 */
        rf_write(0x56, 0x5102A);     /* RF_RCK_OS refine */
        WLOG("  iqk: rf_tx_rel reload 30/31/32/56 (эталон 8188F)\r\n");
    }
    uart_print("  iqk: done\r\n");
}

/* ---- RX-тест: проверка живости RX-пути ----
 * Полная конфигурация приёмника по драйверу rtl8189fs:
 *  0) пробуждение FW (RPWM/CPWM), H2C SET_PWR_MODE=ACTIVE (0x20),
 *     прерывания FBR/CCCR/HIMR (RX_REQUEST), C2H-очистка (0x1AF);
 *  1) RF_CHNLBW = 0x401 (канал 1, 20 МГц) — БЕЗ бита 15 (0x8C01 — это
 *     LCK/IQK-режим, блокирует приём);
 *  1b) RFMOD (0x800) |= bCCKEn(bit24) | bOFDMEn(bit25) — включить
 *      CCK/OFDM-блоки BB (в BB-таблице они сброшены), иначе приёмник
 *      физически не декодирует эфир; 0x577=3, 0x976=0 (2nd CCA);
 *  2) LLT (0x224 |= BIT16) + границы FIFO (0x116=0x3EFF, TX 0xF1);
 *  2c) BWOPMODE (0x603)=0x04, SIFS/RL/EDCA (из _InitEDCA/_InitAdaptiveCtrl);
 *  3) RCR=0x7000604E / RRSR=0xFFF / RXFLTMAP / MAR — приём mgmt+bcast;
 *  4) REG_CR |= HCI_RXDMA(1)|RXDMA_EN(3)|MACTXEN(6)|MACRXEN(7);
 *  4d) RX-DMA: PBP=0x11, DRVINFO=4, AGG_PG_TH=0x010F, 0x290=0x0E;
 *  5) свип каналов 1→6→11 со слушанием 150 мс, диагностика
 *     rxlen(0x1C)/rxpkt(0x284)/cca(0xA08)/trssi(0xA50). */
void h3_wifi_rf_rx_test(void)
{
    uint8_t num = 0;
    uint16_t sf16 = 0;
    uint32_t d = 0;
    unsigned int t;

    uart_print("  rx: start\r\n");

    /* W.345: A0 в третьей точке — прямо перед probe. Значение 0 здесь при
       3 после pwon/post_fw = FW ушла в RESET между инициализацией и сканом. */
    {
        int st = h3_wifi_power_state();
        uart_print("  a0_pre_probe=0x");
        uart_print_hex((uint32_t)(st & 0xFF));
        uart_print("\r\n");
    }

    /* W.361: A0 address check — определить, где реально SYS_CFG.
       h3_wifi_power_state() читает WLAN 0x100A0 (pseq_read8(0,0xA0) ->
       cmd52_read(1,(8<<13)|0xA0)). Прямая запись A0->1 в W.353 шла в
       SDIO-local 0x00A0 (cmd52_read(1,0x00A0)) — другой регистр, поэтому
       A0 не менялся. Читаем оба для подтверждения. */
    {
        uint8_t a0_w = 0, a0_l = 0;
        pseq_read8(0, 0xA0, &a0_w);            /* WLAN 0x100A0 (SYS_CFG) */
        h3_sdc_cmd52_read(1, 0x00A0, &a0_l);   /* SDIO local 0x00A0 */
        uart_print("  a0_wlan=0x");
        uart_print_hex((uint32_t)a0_w);
        uart_print(" a0_sdio=0x");
        uart_print_hex((uint32_t)a0_l);
        uart_print("\r\n");
    }

    /* 0) Разбудить firmware — ПО ДРАЙВЕРУ rtw_set_rpwm (rtw_pwrctrl.c:
           528-637): нельзя писать 0x00 «active» — активный запрос это
           S4 (PS_ST_ACTIVE|PS_ALL_ON = 0x0C) | накапливаемый toggle(bit7),
           при переходе +PS_ACK(bit6=0x40), маска записи 0xC1
           (HW_VAR_SET_RPWM, sdio_halinit.c:1695-1697). Поллинг CPWM —
           по СМЕНЕ toggle bit7: (cpwm_orig ^ cpwm_now) & 0x80, а не по
           cpwm==0x00. Раньше RPWM=0x00 + ждать cpwm==0 — FW не выходила
           в активный режим (отсюда мёртвый RX). */
    {
        uint8_t rpwm = 0, cpwm_orig = 0, cpwm_now = 0;
        uint8_t tog = 0x80;
        unsigned int wt;
        /* MCUFWDL / SYS_FUNC_EN до пробуждения. */
        h3_sdc_read_wlan_reg8(0x0080, &rpwm);
        WLOG("  rx: mcu=0x");
        WLOG_HEX((uint32_t)rpwm);
        WLOG(" (fw_rdy=");
        WLOG_INT((int)((rpwm >> 1) & 1));
        WLOG(" chksum=");
        WLOG_INT((int)((rpwm >> 2) & 1));
        WLOG(" rst=");
        WLOG_INT((int)((rpwm >> 3) & 1));
        WLOG(")\r\n");
        h3_sdc_read_wlan_reg16(0x0002, &sf16);
        WLOG("  rx: sf16=0x");
        WLOG_HEX((uint32_t)sf16);
        WLOG(" (cpu_en=");
        WLOG_INT((int)((sf16 >> 2) & 1));
        WLOG(")\r\n");
        /* CPWM текущий (toggle) — как HW_VAR_CPWM у драйвера.
           ВАЖНО (W.320): RPWM/CPWM — это SDIO-local регистры (device id 0),
           и драйвер обращается к ним через fn1 (SdioLocalCmd52Read/Write,
           _sdio_local_read/write, sdio_ops.c:1056/1117 → HalSdioGetCmdAddr=fn1
           c адресом (0<<13)|offset). Раньше писали через fn0 — мимо FW,
           FW не получала запрос S4 и не переходила в ACTIVE (cpwm=0). */
        h3_sdc_cmd52_read(1, 0x0038, &cpwm_orig);
        /* rpwm ПО ДРАЙВЕРУ (rtw_set_rpwm, rtw_pwrctrl.c:586-591):
             rpwm = pslv | tog, pslv=PS_STATE_S4=0x0C (PS_ST_ACTIVE|PS_ALL_ON),
             + PS_ACK(0x40) только при переходе из S0/S1 в S2+.
           Маска записи 0xC1 (HW_VAR_SET_RPWM, sdio_halinit.c:1690-1697)
           сохраняет биты 0,6,7: clock(bit0), ACK(bit6), toggle(bit7). */
        tog &= 0x80u;
        rpwm = (uint8_t)((0x0Cu | tog) & 0xC1u);            /* S4|toggle, маска 0xC1 */
        if (((cpwm_orig & 0x0Fu)) < 2u)                     /* cpwm < S2? */
            rpwm = (uint8_t)(rpwm | 0x40u);                 /* +PS_ACK */
        h3_sdc_cmd52_write(1, 0x0080, rpwm);                /* fn1 (SDIO-local) */
        tog += 0x80u;                                        /* накопление toggle */
        WLOG("  rx: rpwm=0x");
        WLOG_HEX((uint32_t)rpwm);
        WLOG(" (write masked, per driver)\r\n");

        /* W.337 замер показал rb=0xCC (регистр принимает полный байт).
           Финальный аппаратный эксперимент: пишем 0xCC (S4|ACK|toggle, с
           state) и смотрим в CPWM — меняются ли state[3:0] и появляется ли
           реальный переход в S4. Если cpwm_after state==0x0C — FW вышла в
           ACTIVE; если 0x00 при этом — подтверждается точка зрения драйвера. */
        {
            uint8_t exp_state_before = (uint8_t)(cpwm_orig & 0x0Fu);
            h3_sdc_cmd52_write(1, 0x0080, 0xCC);
            udelay(5000);
            uint8_t cpwm_after = 0;
            h3_sdc_cmd52_read(1, 0x0038, &cpwm_after);
            (void)exp_state_before;   /* при выкл. verbose не используется */
            WLOG("  rx: rpwm_exp w=0xCC cpwm=0x");
            WLOG_HEX((uint32_t)cpwm_after);
            WLOG(" (state_before=0x");
            WLOG_HEX((uint32_t)exp_state_before);
            WLOG(" state_after=0x");
            WLOG_HEX((uint32_t)(cpwm_after & 0x0Fu));
            WLOG(")\r\n");
            /* Вернуть к штатному rpwm (не ломать поллинг ниже). */
            h3_sdc_cmd52_write(1, 0x0080, rpwm);
        }
        /* Поллим CPWM: ждём смену toggle bit7 (как драйвер :630). */
        wt = 0;
        do {
            udelay(1000);
            h3_sdc_cmd52_read(1, 0x0038, &cpwm_now);
        } while (!((cpwm_orig ^ cpwm_now) & 0x80u) && (++wt < 100));
        WLOG("  rx: cpwm=0x");
        WLOG_HEX((uint32_t)cpwm_now);
        WLOG(" (tog_sw=");
        WLOG_INT((int)(((cpwm_orig ^ cpwm_now) & 0x80u) ? 1 : 0));
        WLOG(")\r\n");
    }

    /* 0b) H2C SET_PWR_MODE = ACTIVE (rtl8188f_set_FwPwrMode_cmd, psmode=0):
         перевести FW в активный режим приёма (PS_STATE=AllON). Без этого
         8051 может остаться в power-save и не слушать эфир. Формат (6 байт):
           [0]=MODE=0, [1]=rlbm(2)<<0|smart_ps(0)<<4=0x02, [2]=awake_intvl=4,
           [3]=all_queue_uapsd=0, [4]=PWR_STATE=0x0C (AllON), [5]=byte5=0x40.
         H2C id = 0x20 (H2C_8188F_SET_PWR_MODE), CmdLen=6, box0 (0x1D0). */
    {
        uint8_t hb = 0;
        uint32_t h2c = 0, h2c_ex = 0;
        /* Формат FillH2CCmd8188F: CmdLen<=3 -> box0; CmdLen>3 ->
           первые 3 байта payload в box0 (0x1D0) с id, остальные — в
           EX-box REG_HMEBOX_EXT0 (0x1F0). 6-байт: box0={id,0,2,4},
           ex={0x0C,0x40}. ВАЖНО: РАНЬШЕ делали write 0x00 в 0x1CC
           (HMETFR) — это НЕ в драйвере (rtl8188f_cmd.c:96-127): драйвер
           ТОЛЬКО читает 0x1CC, ждёт valid==0 (FW забрала прошлую
           команду), пишет EX-box, потом box0. Запись 0x00 в 0x1CC
           сбивала valid и FW не получала команду (1cc=0x01 в логе). */
        {
            unsigned int t3 = 0;
            uint8_t hm3 = 0xFF;
            /* ждать, пока FW заберёт предыдущую команду (valid==0) */
            do {
                udelay(1000);
                h3_sdc_read_wlan_reg8(0x01CC, &hm3);
            } while ((hm3 & 0x01u) && (++t3 < 100));
            uart_print("  rx: h2c_pre 1cc=0x");
            uart_print_hex((uint32_t)hm3);
            uart_print("\r\n");
        }
        /* Сформировать H2C: box0={id 0x20, mode 0, rlbm 2, awake 4},
           EX-box={uapsd 0, PowerState 0x0C, byte5 0x40}. */
        h2c = 0x20u;
        h2c |= 0x00u << 8;
        h2c |= 0x02u << 16;
        h2c |= 0x04u << 24;
        h2c_ex = 0x00u;
        h2c_ex |= 0x0Cu << 8;
        h2c_ex |= 0x40u << 16;
        h3_sdc_write_wlan_reg32(0x01F0, h2c_ex);       /* EX-box: b4..b6 */
        h3_sdc_write_wlan_reg32(0x01D0, h2c);          /* box0: id+b1..b3 */
        {
            unsigned int t2 = 0;
            do {
                udelay(1000);
                h3_sdc_read_wlan_reg8(0x01CC, &hb);   /* HMETFR: FW забрала */
            } while ((hb & 0x01u) && (++t2 < 100));
            uart_print("  rx: h2c_pwrmode 1cc=0x");
            uart_print_hex((uint32_t)hb);
            uart_print("\r\n");
        }
        udelay(5000);
    }

    /* W.345: поллинг C2H (0x1A0) 100 мс сразу после H2C SET_PWR_MODE.
       SET_PWR_MODE — команда, гарантированно отвечающая ACK-ом через C2H
       (rtl8188f_cmd.c _rtw_cmd_hdl). Если id!=0 — FW отвечает, и C2H-путь
       жив; тогда C2H-ABSENT от AP_REQ_TXRPT (0x43) — особенность команды в
       NOLINK. Если id==0 — FW молчит на любых H2C, копать HIMR/0x1AF. */
    {
        uint32_t c2h = 0;
        unsigned int poll;
        for (poll = 0; poll < 100; poll++) {
            udelay(1000);
            h3_sdc_read_wlan_reg32(0x01A0, &c2h);
            if (c2h & 0x0F) break;
        }
        uart_print("  rx: c2h_after_pwrmode=0x");
        uart_print_hex(c2h);
        uart_print(" (id=0x");
        uart_print_hex(c2h & 0x0F);
        uart_print(" len=0x");
        uart_print_hex((c2h >> 8) & 0xFF);
        uart_print(")\r\n");
    }

    /* W.362-1: A0 сразу после H2C SET_PWR_MODE. Если state 3->1 — H2C
       работает и IQK после сбрасывает; если остался 3 — H2C не переводит
       чип из S3 (Card Enable, RF off), TX невозможен. */
    {
        uint8_t a0h = 0;
        pseq_read8(0, 0xA0, &a0h);
        uart_print("  a0_post_h2c=0x");
        uart_print_hex((uint32_t)a0h);
        uart_print(" (state=");
        uart_print_hex((uint32_t)(a0h & 0x03));
        uart_print(")\r\n");
    }

    /* W.277: H2C RSSI_SETTING (0x42) УБРАН — в W.276 FW застревает на нём:
       после записи 0x42 бокс0 остаётся занят (1cc=0x01), и все последующие
       H2C (0x6D={1}/0x6D={0}, AP_REQ_TXRPT) теряются; cal_ready 1e7=0.
       RSSI_SETTING в драйвере шлётся из ODM в связанном состоянии (STA с
       MACID), для NOLINK/scan он not applicable. Вместо него — диагностика
       HMETFR (0x1CC, 32-бит): valid-биты 0..3 = занятость боксов 0..3. */
    {
        uint32_t hmfr = 0;
        h3_sdc_read_wlan_reg32(0x01CC, &hmfr);
        uart_print("  rx: hmetfr=0x");
        uart_print_hex(hmfr);
        uart_print(" (box");
        for (int bx = 0; bx < 4; bx++)
            uart_print_int((int)((hmfr >> bx) & 1));
        uart_print(")\r\n");
        udelay(5000);
    }

/* 0b2) Вход в скан (эталон sitesurvey_cmd_hdl, SCAN_ENTER,
         rtw_mlme_ext.c:13855-13873):
         1) sitesurvey_set_igi(1) — IGI = 0x1E (REG rOFDM0_XAAGCCore1 0xC50,
            phydm_dig.c ODM_Write_DIG; иначе AGC не настроен для скана);
         2) rtw_phydm_func_for_offchannel — DIG отключён (у нас DIG не
            запущен, пропускаем);
         3) Set_MSR(_HW_STATE_NOLINK_) = 0 — при скане MSR = NOLINK,
            а НЕ STA (наш прежний MSR=0x01 противоречил эталону);
         4) HW_VAR_MLME_SITESURVEY=1 (RCR/RXFLTMAP — ниже в конфиге). */
    {
        uint8_t msr = 0;
        /* IGI для скана: 0x1E (sitesurvey_set_igi). */
        h3_sdc_write_wlan_reg8(0x0C50, 0x1E);
        h3_sdc_read_wlan_reg8(0x0C50, &msr);
        WLOG("  rx: igi_set=0x");
        WLOG_HEX((uint32_t)msr);
        WLOG(" (0x1e)\r\n");

        /* MSR = NOLINK (0): scan state. */
        h3_sdc_read_wlan_reg8(0x0102, &msr);
        WLOG("  rx: msr_before=0x");
        WLOG_HEX((uint32_t)msr);
        WLOG("\r\n");
        h3_sdc_write_wlan_reg8(0x0102, (uint8_t)(msr & 0x0C));
        h3_sdc_read_wlan_reg8(0x0102, &msr);
        WLOG("  rx: msr_set=0x");
        WLOG_HEX((uint32_t)msr);
        WLOG(" (NOLINK/scan)\r\n");
    }

    /* 0c) SDIO Interrupt Enable:
         FBR (fn0, 0x100) bit0 = INT_EN (прерывания функции 1) и
         CCCR (fn0, 0x04): bit0 = IENM (общее разрешение), bit1 = IEN1
         (разрешение прерываний ФУНКЦИИ 1). ВАЖНО (W.260): драйвер через
         sdio_claim_irq (os_dep/linux/sdio_intf.c:137) ставит IENM|IEN1 = 0x03
         (Linux mmc: ien |= 1<<func_num). РАНЬШЕ МЫ ПИСАЛИ ТОЛЬКО 0x01 (IENM) —
         из-за чего HISR-линия функции 1 не доходила до хоста (hisr_rx=0 весь
         свип даже при живом приёме в физике). Без IEN1 8051 не генерирует
         событие RX_REQUEST на хост. */
    {
        uint8_t fbr = 0, ioc = 0;
        h3_sdc_cmd52_read(0, 0x0100, &fbr);
        h3_sdc_cmd52_read(0, 0x0004, &ioc);
        WLOG("  rx: fbr=0x");
        WLOG_HEX((uint32_t)fbr);
        WLOG(" cccr=0x");
        WLOG_HEX((uint32_t)ioc);
        WLOG(" (fbr_inten=");
        WLOG_INT((int)(fbr & 1));
        WLOG(" cccr_ienm=");
        WLOG_INT((int)(ioc & 1));
        WLOG(" cccr_ien1=");
        WLOG_INT((int)((ioc >> 1) & 1));
        WLOG(")\r\n");
        h3_sdc_cmd52_write(0, 0x0100, (uint8_t)(fbr | 0x01));  /* INT_EN fn1 */
        h3_sdc_cmd52_write(0, 0x0004, (uint8_t)(ioc | 0x03));  /* IENM | IEN1 */
        h3_sdc_cmd52_read(0, 0x0100, &fbr);
        h3_sdc_cmd52_read(0, 0x0004, &ioc);
        WLOG("  rx: fbr_set=0x");
        WLOG_HEX((uint32_t)fbr);
        WLOG(" cccr_set=0x");
        WLOG_HEX((uint32_t)ioc);
        WLOG("\r\n");
    }

    /* 1) Канал 1 / 20 МГц: 0x401 (как после LCK), НЕ 0x8C01 (bit15!). */
    rf_write(0x18, 0x00000401);
    udelay(100);
    WLOG("  rx: rf18=0x");
    WLOG_HEX(rf_read(0x18));
    WLOG("\r\n");

    /* 1b) _BBTurnOnBlock (sdio_halinit.c:832-840): включить CCK и OFDM
         блоки BB — rFPGA0_RFMOD (0x800) |= bCCKEn | bOFDMEn.
         ВАЖНО: bCCKEn = 0x1000000 (BIT24), bOFDMEn = 0x2000000 (BIT25) —
         Hal8188FPhyReg.h:515-516! Раньше мы ставили (1<<16)|(1<<17) —
         НЕ ТЕ биты, CCK/OFDM так и не включались (лог rfmod=0x82075700,
         а должно 0x83075700). Без этих бит приёмник физически не
         декодирует эфир. */
    h3_sdc_read_wlan_reg32(0x0800, &d);
    d |= (1u << 24) | (1u << 25);   /* bCCKEn | bOFDMEn */
    h3_sdc_write_wlan_reg32(0x0800, d);
    h3_sdc_read_wlan_reg32(0x0800, &d);
    WLOG("  rx: rfmod=0x");
    WLOG_HEX(d);
    WLOG("\r\n");

    /* 1b) Записи из конца драйверной инициализации (sdio_halinit.c:1158-1159):
         REG_SECONDARY_CCA_CTRL_8188F (0x577) = 0x3 — CRC-CCA;
         0x976 = 0 — 2nd CCA. У нас их не было. */
    h3_sdc_write_wlan_reg8(0x0577, 0x03);
    h3_sdc_write_wlan_reg8(0x0976, 0x00);

    /* 2) FIFO-разметка (как драйвер sdio_halinit.c:1126-1130):
       - RQPN (0x200) = 0x80E7020C, RQPN_NPQ (0x214) = 0x02 —
         _InitQueueReservedPage: numPubQ = TX_TOTAL(0xF7) - HPQ(0x0C) -
         LPQ(0x02) - NPQ(0x02) = 0xE7. Живой чип Lakka подтверждает:
         0x200=0x00E7020C (rtl8189fs-lakka-live-dump.md). Раньше было
         0xE0 (считали с BCNQ1/WOWLAN 0x08/0x07). W.247. */
    h3_sdc_write_wlan_reg8(0x0214, 0x02);        /* RQPN_NPQ */
    h3_sdc_write_wlan_reg32(0x0200, 0x80E7020Cu);/* RQPN: HPQ|LPQ|PUBQ|LD */
    h3_sdc_read_wlan_reg32(0x0200, &d);
    WLOG("  rx: rqpn=0x");
    WLOG_HEX(d);
    WLOG("\r\n");

    /* 2a) TX-границы (_InitTxBufferBoundary, sdio_halinit.c:321-324):
         txpktbuf_bndy = TX_PAGE_BOUNDARY_8188F = 0xF8
         (0xFF - BCNQ 0x08 - BCNQ1 0x00 - WOWLAN 0x00 + 1; 0xF8 подтверждено
         ЖИВЫМ чипом Lakka: 0x114=0xF8, rtl8189fs-lakka-live-dump.md).
         Раньше стояло 0xF1 (считали с WOWLAN 0x07) — бОльшая граница
         отнимала страницы хвоста TX, и FW могла не находить свою область
         под передачу кадров. W.247: выровнено по живому. */
    h3_sdc_write_wlan_reg8(0x0424, 0xF8);   /* TXPKTBUF_BCNQ_BDNY */
    h3_sdc_write_wlan_reg8(0x0425, 0xF8);   /* TXPKTBUF_MGQ_BDNY */
    h3_sdc_write_wlan_reg8(0x045D, 0xF8);   /* TXPKTBUF_WMAC_LBK */
    h3_sdc_write_wlan_reg8(0x0114, 0xF8);   /* TRXFF_BNDY (tx start) */
    h3_sdc_write_wlan_reg8(0x0209, 0xF8);   /* REG_TDECTRL+1 (0x0208+1): 5-й регистр _InitTxBufferBoundary (sdio_halinit.c:325) */

    h3_sdc_read_wlan_reg32(0x0224, &d);

    h3_sdc_write_wlan_reg32(0x0224, d | (1u << 16));
    {
        unsigned int lt = 0;
        do {
            udelay(2000);
            h3_sdc_read_wlan_reg32(0x0224, &d);
        } while ((d & (1u << 16)) && (++lt < 500));   /* до 1 с */
        WLOG("  rx: llt=0x");
        WLOG_HEX(d);
        WLOG("\r\n");
    }
    /* rxff_bndy (0x0116): RX_DMA_BOUNDARY_8188F = 0x4000-0x80-1 = 0x3F7F
           (RX_DMA_SIZE 16K минус RESERVED 0x80 — т.к. CONFIG_FW_C2H_DEBUG
           выключен, резерв для tx report 0x80, а не 0x100; раньше ставили
           жёстко 0x3EFF — лишние 128 байт отдавались, драйвер берёт 0x3F7F). */
    h3_sdc_write_wlan_reg16(0x0116, 0x3F7F);   /* rxff_bndy (0x0116, 16-бит) */
    h3_sdc_read_wlan_reg16(0x0116, (uint16_t *)&d);
    d &= 0xFFFF;
    WLOG("  rx: rx_bndy=0x");
    WLOG_HEX(d);
    WLOG("\r\n");

    /* 2c) Драйверные шаги из sdio_halinit.c (_InitOperationMode :793,
         _InitEDCA :600-619, _InitAdaptiveCtrl :592-597):
         - REG_BWOPMODE (0x603) = BW_OPMODE_20MHZ (BIT2=0x04) — ширина
           канала приёмника;
         - SIFS: 0x428/0x63A/0x514/0x516 = 0x100a;
         - RL (0x42A) = 0x3030;
         - EDCA: 0x508/0x50C/0x504/0x500 + 0x638/0x55C. */
    h3_sdc_write_wlan_reg8(0x0603, 0x04);          /* BWOPMODE 20MHz */
    h3_sdc_write_wlan_reg16(0x0428, 0x100A);       /* SPEC_SIFS */
    h3_sdc_write_wlan_reg16(0x063A, 0x100A);       /* MAC_SPEC_SIFS */
    h3_sdc_write_wlan_reg16(0x0514, 0x100A);       /* SIFS_CTX (CCK) */
    h3_sdc_write_wlan_reg16(0x0516, 0x100A);       /* SIFS_TRX (OFDM) */
    h3_sdc_write_wlan_reg16(0x042A, 0x3030);       /* RL: LRL/SRL 0x30 */
    h3_sdc_write_wlan_reg32(0x0508, 0x005EA42Bu);
    h3_sdc_write_wlan_reg32(0x050C, 0x0000A44Fu);
    h3_sdc_write_wlan_reg32(0x0504, 0x005EA324u);
    h3_sdc_write_wlan_reg32(0x0500, 0x002FA226u);
    h3_sdc_write_wlan_reg8(0x0638, 0x28);          /* USTIME_EDCA */
    h3_sdc_write_wlan_reg8(0x055C, 0x28);          /* USTIME_TSF */

    /* Непокрытые _Init* из sdio_halinit.c:
       - _InitRetryFunction (sdio_halinit.c:656-663): FWHW_TXQ_CTRL (0x818)
         |= EN_AMPDU_RTY_NEW (BIT7), ACKTO (0x634) = 0x40;
       - _InitRDGSetting (sdio_halinit.c:810-815): RD_CTRL (0x524) = 0xFF,
         RD_NAV_NXT (0x544) = 0x200, RD_RESP_PKT_TH (0x463) = 0x05 —
         управление Response Data (ответы/приём). Исправление addr:
         0x524 = REG_RD_CTRL (0x524), 0x544 = REG_RD_NAV_NXT, 0x463 = RD_TH. */
    h3_sdc_read_wlan_reg8(0x0818, &num);
    h3_sdc_write_wlan_reg8(0x0818, (uint8_t)(num | (1u << 7)));  /* EN_AMPDU_RTY_NEW */
    h3_sdc_write_wlan_reg8(0x0634, 0x40);          /* ACKTO */
    h3_sdc_write_wlan_reg8(0x0524, 0xFF);          /* RD_CTRL */
    h3_sdc_write_wlan_reg16(0x0544, 0x0200);       /* RD_NAV_NXT */
    h3_sdc_write_wlan_reg8(0x0463, 0x05);          /* RD_RESP_PKT_TH */

    /* W.264-D5: rtl8188f_CheckAntenna_Selection (rtl8188f_hal_init.c:3375-3389) —
       АКТИВНАЯ ветка драйвера для 8188F: только REG_LEDCFG2 (0x4E) |= BIT7
       (DPDT_SEL_EN — «Let 8051 take control antenna setting», 0x4C[23]).
       Наш старый блок (W.246) копировал НЕАКТИВНУЮ ветку #if 0 функции
       rtl8188f_InitAntenna_Selection (0x66/0x40/0x4C[24:23]/0x944/0x930/0x38) —
       «only for DPDT and Fixed board type», и, по замечанию, не даёт 8051
       управлять DPDT-переключателем антенны. Ставим точно активную ветку. */
    {
        uint8_t led = 0;
        h3_sdc_read_wlan_reg8(0x004E, &led);
        h3_sdc_write_wlan_reg8(0x004E, (uint8_t)(led | 0x80)); /* DPDT_SEL_EN */
        h3_sdc_read_wlan_reg8(0x004E, &led);
        WLOG("  rx: antenna_sel set (ledcfg2=0x");
        WLOG_HEX((uint32_t)led);
        WLOG(")\r\n");
    }
    WLOG("  rx: rdg_set\r\n");

    /* readback BWOPMODE — подтвердить запись. */
    h3_sdc_read_wlan_reg8(0x0603, &num);
    WLOG("  rx: bwop=0x");
    WLOG_HEX((uint32_t)num);
    WLOG("\r\n");

    /* _InitNetworkType: REG_CR[17:16] = NT_LINK_AP (0x2). */
    h3_sdc_read_wlan_reg32(0x0100, &d);
    d = (d & ~0x30000u) | (0x2u << 16);
    h3_sdc_write_wlan_reg32(0x0100, d);
    WLOG("  rx: nettype_ap, cr=0x");
    WLOG_HEX(d);
    WLOG("\r\n");

    /* 3) RCR (0x0608): по _InitWMACSetting (sdio_halinit.c:548-556):
           RCR_APM|RCR_AM|RCR_AB | RCR_CBSSID_DATA|RCR_CBSSID_BCN|RCR_AMF |
           RCR_HTC_LOC_CTRL | RCR_APP_PHYST_RXFF|RCR_APP_ICV|RCR_APP_MIC
           = 0x700060CE (биты 28-30|14|13|7|6|3|2|1). Раньше мы ставили
           0x7000200F — без AM (bit2, multicast) и с снятым CBSSID_DATA (6).
           Для скана снимаем только CBSSID_BCN (bit7) → 0x7000604E. */
    h3_sdc_write_wlan_reg32(0x0608, 0x7000604Eu);   /* _InitWMACSetting, ~BCN */
/* RRSR (0x0440): базовые принимаемые скорости. Эталон на инициализации
        (_InitAdaptiveCtrl, sdio_halinit.c:583-586) пишет RATE_RRSR_CCK_ONLY_1M
        = 0xFFFF1 (RATE_BITMAP_ALL=0xFFFFF с очисткой, hal_com_reg.h:767,770).
        Раньше ставили 0x15F (CCK 1/2/5.5/11 + OFDM 6..24) — такого значения
        драйвер для 8188F не пишет; выравниваем по эталону. */
h3_sdc_write_wlan_reg32(0x0440, 0x000FFFF1u);
    /* RXFLTMAP на скане (как hw_var_set_mlme_sitesurvey): RXFLTMAP2=0 —
       не принимать data-кадры, RXFLTMAP1=ps-poll, RXFLTMAP0=mgmt.
       Живой чип (Lakka) в том же режиме читается как 0x6A0=0xffff,
       0x6A2=0x400 — совпадает с нашим. */
    h3_sdc_write_wlan_reg16(0x06A4, 0x0000);   /* RXFLTMAP2: 0 (sitesurvey) */
    h3_sdc_write_wlan_reg16(0x06A2, 0x0400);   /* RXFLTMAP1: ps-poll */
    h3_sdc_write_wlan_reg16(0x06A0, 0xFFFF);   /* RXFLTMAP0: mgmt */
    /* MAR (0x0620): broadcast + свой MAC (пишем FF, приём всех). */
    h3_sdc_write_wlan_reg32(0x0620, 0xFFFFFFFF);
    h3_sdc_write_wlan_reg32(0x0624, 0xFFFFFFFF);

    /* 3z) SDIO TX_CTRL (SDIO-local 0x00, 32-бит): драйвер (rtl8188fs_hal_init,
         sdio_halinit.c:1212-1220) читает и ЧИСТИТ биты [2:0] (& 0x0000FFF8),
         затем пишет обратно — прямо перед включением MACTXEN/MACRXEN.
         Живой чип Lakka: 0x00 = 0x00000010 ([2:0]=0). МЫ ЭТОТ ШАГ НИКОГДА
         НЕ ДЕЛАЛИ — W.248. Если [2:0] не 0, SDIO TX-путь (и выдача кадров
         из FIFO в MAC) может блокироваться, что объясняет: probe пишется в
         HIQ (hiq_free падает), но FW не передаёт кадр и не возвращает страницы. */
    {
        uint8_t tc[4] = { 0, 0, 0, 0 };
        uint32_t tv;
        h3_sdc_cmd53_read_bytes(0, 0x0000u, 4, tc);
        tv = (uint32_t)tc[0] | ((uint32_t)tc[1] << 8) |
             ((uint32_t)tc[2] << 16) | ((uint32_t)tc[3] << 24);
        tv &= 0x0000FFF8u;                      /* clear [2:0], keep [15:3] */
        tc[0] = (uint8_t)(tv & 0xFF);
        tc[1] = (uint8_t)((tv >> 8) & 0xFF);
        tc[2] = (uint8_t)((tv >> 16) & 0xFF);
        tc[3] = (uint8_t)((tv >> 24) & 0xFF);
        h3_sdc_cmd53_write_bytes(0, 0x0000u, 4, tc);
        WLOG("  rx: tx_ctrl=0x");
        WLOG_HEX(tv);
        WLOG("\r\n");
    }

    /* W.253: пропущенные шаги драйвера перед включением MAC:
       - REG_BAR_MODE_CTRL (0x04CC) = 0x0201FFFF (sdio_halinit.c:1194);
       - REG_HWSEQ_CTRL (0x0423) = 0xFF (sdio_halinit.c:1198, HW SEQ NUM);
       - REG_EARLY_MODE_CONTROL (0x04D0) = 0 (sdio_halinit.c:1052). */
    h3_sdc_write_wlan_reg32(0x04CC, 0x0201FFFFu);   /* BAR_MODE_CTRL */
    h3_sdc_write_wlan_reg8(0x0423, 0xFF);           /* HWSEQ_CTRL */
    h3_sdc_write_wlan_reg8(0x04D0, 0x00);           /* EARLY_MODE_CONTROL=0 */
    WLOG("  rx: bar/hwseq/early set\r\n");

    /* 4) REG_CR (0x0100): MACTXEN=BIT6, MACRXEN=BIT7 (hal_com_reg.h:1293-1294,
           sdio_halinit.c:1238-1241 «Enable MACTXEN/MACRXEN block»); BIT9 (ENSEC).
           ВАЖНО: BIT8 (ENSWBCN) НЕ ставим — драйвер его не выставляет (только
           MACTXEN|MACRXEN), а живой чип Lakka читается как 0x0206FF (bit8=0).
           32-бит RMW, чтобы НЕ затереть bit17 (NETTYPE_AP) из шага выше. */
    h3_sdc_read_wlan_reg32(0x0100, &d);
    d |= (1u << 6) | (1u << 7) | (1u << 9);
    d &= ~(1u << 8);                                /* ENSWBCN=0, как живой */
    h3_sdc_write_wlan_reg32(0x0100, d);
    h3_sdc_read_wlan_reg32(0x0100, &d);
    uart_print("  rx: cr=0x");
    uart_print_hex((uint32_t)d);
    uart_print(" (bit17=");
    uart_print_int((int)((d >> 17) & 1));
    uart_print(" macrx_en=");
    uart_print_int((int)((d >> 7) & 1));
    uart_print(" mactx_en=");
    uart_print_int((int)((d >> 6) & 1));
    uart_print(")\r\n");

    /* W.361: откат к W.307-виду. Блок tx_enable (W.352/W.353) с
       0x101|=1 / 0x02|=3 / 0x100|=C0 / прямая запись A0=1 убран:
       - запись 0x100|=C0 дублировала CR из блока выше (2153) и не
         учитывала ENSWBCN=0;
       - A0->1 писал в SDIO-local 0x00A0, а не WLAN 0x100A0 (SYS_CFG) —
         уходило в пустоту;
       - 0x101|=1/0x02|=3 не влияли на TXPKT_EMPTY.
       CR уже выставлен выше: MACTXEN|MACRXEN|ENSEC, ENSWBCN=0 (как живой). */
    uart_print("  rx: tx_enable done (CR MACTXEN|MACRXEN, ENSWBCN=0)\r\n");

    /* W.261-D4: IQK ПОСЛЕ включения MAC — как драйвер:
       sdio_halinit.c:1238 (MACTXEN|MACRXEN) → :1273 (LCK) → :1277-1288
       (H2C 0x6D + poll 0x1E7 + PHY_IQCalibrate). Раньше IQK в цепочке
       НЕ вызывался вообще (main.c только LCK → RX-тест). Калибровка
       I/Q-дисбаланса может требоваться чипу для включения RX-тракта. */
    h3_wifi_rf_iqk();

/* 4b) REG_RXPKT_NUM (0x284 device 8 = hal_com_reg.h:187): счётчик пакетов
       в RXPKTBUF + биты RW_RELEASE_EN(18)/RXDMA_IDLE(17)/RXPKT_RELEASE(16).
       Если бит 18 (RW_RELEASE_EN) взведён — DMA заморожена и пакеты не
       выкладываются в хост. Сбрасываем (release), как
       rtw_hal_release_rx_dma (hal_com.c:1566). */
    h3_sdc_read_wlan_reg32(0x0284, &d);
    h3_sdc_write_wlan_reg32(0x0284, d & ~(1u << 18));
    h3_sdc_read_wlan_reg32(0x0284, &d);
    uart_print("  rx: rxpk=0x");
    uart_print_hex(d);
    uart_print(" (rw_release=");
    uart_print_int((int)((d >> 18) & 1));
    uart_print(")\r\n");

    /* 4b2) System Interrupt + C2H-очистка — EnableInterrupt8188FSdio
           (sdio_ops.c:1787-1799): REG_HSIMR (0x58) |= SysIntrMask (0),
           и REG_C2HEVT_CLEAR (0x1AF) = C2H_EVT_HOST_CLOSE (0x00) —
           закрыть C2H-событие, иначе FW НЕ ШЛЁТ новые C2H (пока старый
           не взят). Пропуск этой записи = FW принимает пакеты (счётчик
           RXPKT_NUM растёт), но C2H/HISR молчат. */
    {
        uint32_t hsimr = 0;
        h3_sdc_read_wlan_reg32(0x0058, &hsimr);
        h3_sdc_write_wlan_reg32(0x0058, hsimr);   /* SysIntrMask=0 — читаем/пишем как драйвер */
        h3_sdc_write_wlan_reg8(0x01AF, 0x00);     /* C2H_EVT_HOST_CLOSE */
        udelay(100);
        WLOG("  rx: c2h_clr=0x");
        h3_sdc_read_wlan_reg8(0x01AF, &num);
        WLOG_HEX((uint32_t)num);
        WLOG("\r\n");
    }

    /* 4c) SDIO HIMR (0x14, device 0) — 32-битный, ВАЖНО: драйвер
           InitInterrupt8188FSdio (sdio_ops.c:1646-1677) маскирует ТОЛЬКО
           SDIO_HIMR_RX_REQUEST_MSK (BIT0); все остальные биты
           (AVAL/TXERR/RXERR/C2HCMD/...) закомментированы.
           CMD52-запись байтом 0x14 не применяется (readback даёт
           старые биты 0x07). Драйвер пишет HIMR 4-байтно через
           sdio_local_write (sdio_ops.c:1781, CMD53). Пишем так же. */
    {
        /* W.320b: HIMR (0x0014) — SDIO-local, доступ только через fn1
           (sdio_local_write, sdio_ops.c:1781 CMD53). Раньше писали fn0,
           HIMR не применялся (readback старые биты 0x07). */
        /* W.345: RX_REQUEST (BIT0) + C2HCMD (BIT17). Эталон (hal_com_reg.h:1617,
           1626): C2HCMD_MSK = BIT17 (НЕ bit18 — это CPWM1). Если C2H-прерывание
           не разрешено — FW не выставит C2H-событие (C2H-ABSENT). */
        uint8_t himr4[4] = { 0x01, 0x00, 0x02, 0x00 };  /* RX_REQUEST | C2HCMD */
        h3_sdc_cmd53_write_bytes(1, 0x0014, 4, himr4);
    }
    h3_sdc_cmd52_write(1, 0x0018, 0xFF);   /* HISR: clear (write-1-clear), fn1 */
    h3_sdc_cmd53_read_bytes(1, 0x0014, 4, (uint8_t *)&num);
    uart_print("  rx: himr_rb=0x");
    uart_print_hex((uint32_t)num);
    WLOG("\r\n");

    /* 4e) Диагностика (W.165): drvinfo, RXDMA_CTRL, C2H_EVT (firmware
           mailbox), HISR до свипа. agg_th читается после записи в 4d. */
    h3_sdc_read_wlan_reg8(0x060F, &num);
    WLOG("  rx: drvinfo=0x");
    WLOG_HEX((uint32_t)num);
    h3_sdc_read_wlan_reg8(0x0286, &num);
    WLOG(" rxdma=0x");
    WLOG_HEX((uint32_t)num);
    WLOG("\r\n");
    h3_sdc_read_wlan_reg32(0x01A0, &d);
    WLOG("  rx: c2h_evt=0x");
    WLOG_HEX(d);
    WLOG("\r\n");
h3_sdc_cmd52_read(1, 0x0018, &num);   /* HISR: fn1 (SDIO-local) */
    WLOG("  rx: hisr_before=0x");
    WLOG_HEX((uint32_t)num);
    WLOG("\r\n");

/* 4d) RX-DMA настройки из драйвера (sdio_halinit.c):
         - PBP (0x0104) = PBP_128|PBP_128 (0x11) — разбиение SRAM;
         - RX_DRVINFO_SZ (0x060F) = 4 — размер драйвер-инфо перед
           802.11-кадром;
         - REG_RXDMA_AGG_PG_TH (0x0280) = 0x010F — по HalRxAggr8188FSdio
           (sdio_halinit.c:649-658: dma_time_th=1, dma_len_th=15). Раньше
           у нас было 0x03 (из дефолта) — порог не тот;
         - TRXDMA_CTRL (0x010C) |= RXDMA_AGG_EN (BIT2);
         - RXDMA_MODE_CTRL (0x0290) = 0x0E — по sdio_AggSettingRxUpdate
           (sdio_halinit.c:661-679): AGG_MODE_EN(bit1)|aggBurstNum(3<<2)|
           aggBurstSize(0<<4) = 0x0C|0x02 = 0x0E. Раньше у нас было 0x06. */
    h3_sdc_write_wlan_reg8(0x0104, 0x11);          /* PBP 128+128 */
    h3_sdc_write_wlan_reg8(0x060F, 0x04);          /* RX_DRVINFO_SZ */
    /* AGG_PG_TH (0x0280) полным 32-бит словом, RXDMA_AGG=0x20 (старший байт) +
       младшие 0x0101 (time=1, len=1). W.317: len снижен с 15 до 1 — каждый
       RX-пакет агрегируется не более чем до 1 страницы (≤512 б), чтобы
       RX0_REQ_LEN не копил 13-Кбайт-агрегаты, которые host не дочитывает. */
    h3_sdc_write_wlan_reg32(0x0280, 0x20000101u);
    /* W.252: REG_TRXDMA_CTRL (0x010C, 16-бит) — карта TX-очередей + RXDMA_AGG_EN.
       Раньше писали БАЙТ read8|BIT2 — ставили только RXDMA_AGG_EN и затирали
       карту очередей, из-за чего TX HIQ/MIQ/LOQ НЕ были примаплены в DMA-движок
       (FW не знала, куда выводить кадр из очереди — вероятная причина мёртвого TX).
       Драйвер: _InitQueuePriority (sdio_halinit.c:1137) → _InitNormalChipRegPriority
       (=_InitNormalChipThreeOutEpPriority, OutEp=3, typical wifi_spec=0):
         voQ=3<<4| viQ=2<<6| beQ=1<<8| bkQ=1<<10| mgQ=3<<12| hiQ=3<<14| cmQ=3<<16
       = 0x3F5B0; низ 0x7 сохраняется RMW. Живой чип Lakka: 0x10c=0xF5B4
       (низкие 16 бит 0x3F5B0 + низ 0x7 + bit2 RXDMA_AGG_EN) — совпадает. */
    {
        uint16_t qlo = 0;
        h3_sdc_read_wlan_reg16(0x010C, &qlo);                 /* RMW, сохранить низ 0x7 */
        unsigned int q16 = qlo;
        q16 = (q16 & 0x0007u) | 0x3F5B0u | (1u << 2);           /* карта + RXDMA_AGG_EN */
        h3_sdc_write_wlan_reg16(0x010C, (uint16_t)q16);
        WLOG("  rx: trxdma=0x");
        WLOG_HEX((uint32_t)q16);
        WLOG("\r\n");
    }
    h3_sdc_write_wlan_reg8(0x0290, 0x0E);          /* RXDMA_MODE_CTRL: agg on, burst=4, size=1K */
    h3_sdc_read_wlan_reg8(0x0290, &num);
    WLOG("  rx: rxdma_mode=0x");
    WLOG_HEX((uint32_t)num);
    h3_sdc_read_wlan_reg16(0x0280, (uint16_t *)&d);
    d &= 0xFFFF;
    WLOG(" agg_th=0x");
    WLOG_HEX(d);
    WLOG("\r\n");

    /* readback ключевых регистров + FIFO. */
    h3_sdc_read_wlan_reg32(0x0608, &d);
    WLOG("  rx: rcr=0x"); WLOG_HEX(d);
    h3_sdc_read_wlan_reg32(0x0440, &d);
    WLOG(" rrsr=0x"); WLOG_HEX(d);
    h3_sdc_read_wlan_reg32(0x0620, &d);
    WLOG(" mar=0x"); WLOG_HEX(d);
    WLOG("\r\n");
    h3_sdc_read_wlan_reg16(0x06A0, (uint16_t *)&d);
    d &= 0xFFFF;
    WLOG("  rx: flt0=0x"); WLOG_HEX(d);
    h3_sdc_read_wlan_reg16(0x06A2, (uint16_t *)&d);
    d &= 0xFFFF;
    WLOG(" flt1=0x"); WLOG_HEX(d);
    h3_sdc_read_wlan_reg16(0x06A4, (uint16_t *)&d);
    d &= 0xFFFF;
    WLOG(" flt2=0x"); WLOG_HEX(d);
    h3_sdc_read_wlan_reg32(0x0200, &d);
    WLOG(" fifopg=0x"); WLOG_HEX(d);
    h3_sdc_read_wlan_reg32(0x011C, &d);
    WLOG(" rxffptr=0x"); WLOG_HEX(d);
    WLOG("\r\n");

    /* W.258: дамп фактических RF/MAC-состояний перед сканом — для
       побайтового сравнения с живым дампом Lakka (rtl8189fs-live-snapshot.md).
       RF 0x18/0x30/0x31/0x32/0x00 — после LCK; MSR (0x102)/TXPAUSE (0x522)/
       REG_RF_CTRL (0x1F) — состояние WLAN-стека. */
    {
        uint8_t m0;
        WLOG("  rx: rf_dump");
        for (uint8_t ra = 0; ra <= 0x3Fu; ra += 1) {
            if (ra % 8 == 0) { WLOG("\r\n  rx:   "); }
            WLOG_HEX(rf_read(ra));
            WLOG(" ");
        }
        WLOG("\r\n");
        h3_sdc_read_wlan_reg8(0x0102, &m0);
        WLOG("  rx: msr=0x"); WLOG_HEX((uint32_t)m0);
        h3_sdc_read_wlan_reg8(0x0522, &m0);
        WLOG(" txp=0x"); WLOG_HEX((uint32_t)m0);
        h3_sdc_read_wlan_reg8(0x001F, &m0);
        WLOG(" rfctl=0x"); WLOG_HEX((uint32_t)m0);
        WLOG("\r\n");
    }

    /* W.262: MAC_DUMP убран из основного пути — 128 строк лога удлиняют
       сессию SDIO на сотни мс и искажают тайминги приёма (замечание).
       Оставлен КОМПАКТНЫЙ контрольный дамп ключевых MAC-регистров. */
    WLOG("  rx: macchk");
    {
        /* 0x00F0 = REG_SYS_CFG: RTL_ID bit23 (1=TEST, 0=NORMAL, hal_com_reg.h:1248),
           CUT-версия bits 12-15 (CHIP_VER_RTL_MASK, :1252), vendor bits 18-19
           (EXT_VENDOR_ID, :1239, :1254). Остальные — ключевые MAC-регистры. */
        static const uint32_t macchk[13] = {
            0x00F0, 0x0100, 0x0102, 0x0104, 0x010C, 0x0116, 0x0200,
            0x0280, 0x0284, 0x0290, 0x0608, 0x060F, 0x0624
        };
        for (int mi = 0; mi < 13; mi++) {
            h3_sdc_read_wlan_reg32(macchk[mi], &d);
            WLOG(" ");
            WLOG_HEX(d);
        }
        WLOG("\r\n");
        h3_sdc_read_wlan_reg32(0x00F0, &d);
        WLOG("  rx: syscfg=0x");
        WLOG_HEX(d);
        WLOG(" (rtl_id=");
        WLOG_INT((int)((d >> 23) & 1));
        WLOG(" cut=");
        WLOG_HEX((d >> 12) & 0xF);
        WLOG(" vend=");
        WLOG_INT((int)((d >> 18) & 0x3));
        WLOG(")\r\n");
    }

    /* 4f) TX-probe request (W.220): после разметки TX FIFO (RQPN/LLT, шаг 2)
         по эталону FREE_TXPG (0x20) заполняется драйверной разметкой
         очередей (sdio_halinit.c:1234-1241), и только потом есть смысл
         ждать свободную страницу HIQ.
         Scan-loop (W.242): в Linux site_survey (rtw_mlme_ext.c:13606) шлёт
         probe НЕ один раз, а на КАЖДОМ канале перед слушанием, обычно до
         3 кадров (issue_probereq_ex, wait_ack=_FALSE) с интервалом. Раньше
         у нас был ОДИН probe на канале 1, а потом свип без новых передач —
         роутер отвечал только на probe, а слушали мы вхолостую. Теперь
         probe перенесён внутрь свип-цикла (см. ниже): на каждом канале
         шлём серию до 3 probe и сразу слушаем ответ. */

    /* 5) Опрос RX-доступности через SDIO_REG_RX0_REQ_LEN (0x1C,
          локальное SDIO-пространство, device 0) — на SDIO-чипах это
          единственный признак прихода пакета в RX FIFO (sdio_ops.c:2196),
          а не «pktnum» (USB-регистр). Свип каналов 1 → 6 → 11. */
    for (t = 0; t < 13; t++) {
        uint32_t ch;
        uint16_t rxlen = 0;
        int bcn_printed = 0;   /* W.411: был ли вывод BCN-строки в этой итерации (для
                                  отступа) — иначе двойной \r\n даёт пустую строку */
        /* Канал [7:0] + BW20 (0xC00). Эталон: PHY_RF6052SetBandwidth держит
           0xC00, phy_SwChnl8188F маской 0x3FF меняет только канал в [7:0].
           Свип ВСЕХ 13 каналов как 2.4G план драйвера (RTW_RD_2G_WORLD =
           {1..13}, rtw_mlme_ext.c:148), NOT 1/6/11 (W.280 ошибочно). */
        ch = 0xC00u | (uint32_t)(t + 1u);
        rf_write(0x18, ch);
        udelay(50000);   /* 50 мс на перестройку синтезатора */

        /* Scan-loop (W.242): серия probe на этом канале перед слушанием —
           как site_survey (rtw_mlme_ext.c:13606-13663) шлёт probe в начале
           каждого канала. До 3 кадров с интервалом (у роутера probe req
           идёт с небольшой паузой; основной — ровно по _issue_probereq,
           wait_ack=_FALSE). Роутер в 10 см отвечает на каждый probe. */
        {
            int p;
            for (p = 0; p < 3; p++) {
                int txrc = h3_wifi_tx_probe_req((uint8_t)(ch & 0xFF));
                if (txrc != 0) break;   /* FIFO/передача сломалась — хватит */
                udelay(50000);          /* ~50 мс между probe (как драйвер) */
            }
            WLOG("  rx: probe ch=");
            WLOG_INT((int)(ch & 0xFF));
            WLOG("\r\n");
        }

        /* Notch-фильтр паразита (phy_SpurCalibration_8188F, вызывается из
           phy_SwChnl8188F при каждом переключении канала, rtl8188f_phycfg.c:
           1118-1137): для каналов 5/6/7/8/13/14 включается CSI-notch с
           конкретной маской [28:24] и битом 9 в 0xC40 + 0xD40/0xD44/0xD48/
           0xD4C/0xD2C. У нас 0xC40=0x1F78403F из BB-таблицы (bit9=0, [28:24]=
           0x31 — мусор), т.е. на канале 6 (частота свипа!) паразит 2440 МГц
           не режется и давит RX. Ставим как драйвер для канала 6: */
        {
            uint32_t d40;
            if (t >= 4 && t < 8) {
                /* Каналы 5..8: включить CSI-notch как драйвер (маски каналов).
                   Значения — из rtl8188f_phycfg.c:1129-1154 (case 6/7/8). */
                uint32_t c40v, d40v, d4cv;
                switch (t) {
                case 4: /* канал 5: [28:24]=0xB, D40=0x06000000 */
                    c40v = 0x0B000000u; d40v = 0x06000000u; d4cv = 0x00000000u; break;
                case 5: /* канал 6: [28:24]=0x4, D40=0x00000600 */
                    c40v = 0x04000000u; d40v = 0x00000600u; d4cv = 0x00000000u; break;
                case 6: /* канал 7: [28:24]=0x3, D4C=0x06000000 */
                    c40v = 0x03000000u; d40v = 0x00000000u; d4cv = 0x06000000u; break;
                default: /* канал 8: [28:24]=0xA, D4C=0x00000380 */
                    c40v = 0x0A000000u; d40v = 0x00000000u; d4cv = 0x00000380u; break;
                }
                h3_sdc_read_wlan_reg32(0x0C40, &d40);
                d40 = (uint32_t)((d40 & ~0x1F000000u) | c40v) | (1u << 9); /* [28:24]+bit9=enable */
                h3_sdc_write_wlan_reg32(0x0C40, d40);
                h3_sdc_write_wlan_reg32(0x0D40, d40v);
                h3_sdc_write_wlan_reg32(0x0D44, 0x00000000u);
                h3_sdc_write_wlan_reg32(0x0D48, 0x00000000u);
                h3_sdc_write_wlan_reg32(0x0D4C, d4cv);
                h3_sdc_read_wlan_reg32(0x0D2C, &d40);
                h3_sdc_write_wlan_reg32(0x0D2C, d40 | (1u << 28));  /* CSI mask */
                udelay(1000);
                uart_print("  rx: notch prev set\r\n");
            } else {
                /* W.310: для всех остальных каналов сбросить notch — как
                   default эталона (rtl8188f_phycfg.c:1165-1167,1173-1174):
                   C40 bit9=0 (disable), D2C bit28=0 (disable CSI). */
                h3_sdc_read_wlan_reg32(0x0C40, &d40);
                h3_sdc_write_wlan_reg32(0x0C40, d40 & ~(1u << 9));
                h3_sdc_read_wlan_reg32(0x0D2C, &d40);
                h3_sdc_write_wlan_reg32(0x0D2C, d40 & ~(1u << 28));
            }
        }
        udelay(300000);  /* W.349: 300 мс слушаем (было 150) — больше dwell =
                             выше шанс поймать beacon'ы всех AP на канале */
        /* W.279: RX0_REQ_LEN (0x1C) читаем CMD53 4 байта через функцию 1
           (sdio_local_read_bytes) — как FREE_TXPG (0x20) в h3_wifi_tx_free_pages_hiq.
           Два отдельных CMD52 (как было) могут не увидеть SDIO-local целиком. */
        {
            uint8_t rxl4[4] = {0, 0, 0, 0};
            int rrc = h3_sdc_cmd53_read_bytes(1, 0x001C, 4, rxl4);
            if (rrc != 0) {
                uart_print("  rx: rxl4 FAIL rc=");
                uart_print_int(rrc);
                uart_print("\r\n");
            }
            rxlen = (uint16_t)(rxl4[0] | ((uint16_t)rxl4[1] << 8));
#if 0
            /* альтернатива для сравнения: полное 32-бит слово */
            uint32_t rxlen32 = rxl4[0] | ((uint32_t)rxl4[1] << 8)
                             | ((uint32_t)rxl4[2] << 16) | ((uint32_t)rxl4[3] << 24);
#endif
        }
        uart_print("  rx: t=");
        uart_print_int((int)t);
        uart_print(" ch=");
        uart_print_int((int)(ch & 0xFF));
        uart_print(" rxlen=0x");
        uart_print_hex((uint32_t)rxlen);
        /* W.280: НАСТОЯЩАЯ выдача пакета из RX FIFO (аналог RecvOnePkt,
           sdio_ops.c:2313-2348 + sd_recv_rxfifo:1977 sdio_read_port):
           если RX0_REQ_LEN > 0 — FW сформировала пакет, читаем его CMD53
           block-mode из device 7 (WLAN_RX0FF_DEVICE_ID), адрес (7<<13)|seq&3,
           длина RND4(rxlen), seq инкрементится как SdioRxFIFOCnt. После
           чтения FW освобождает FIFO и RX0_REQ_LEN обнуляется. */
        /* W.304: правильная выдача RX FIFO по эталону.
           sdio_read_port(pintfhdl, WLAN_RX0FF_DEVICE_ID=7, cnt, mem)
             → HalSdioGetCmdAddr8188FSdio(7, cnt, &addr) → addr=(7<<13)|(cnt&3)
             → _sd_read(func=1, addr, cnt) → sdio_memcpy_fromio(func=1, addr, cnt)
           ИТОГ: CMD53 function 1, адрес (7<<13)|(seq&3), читаем ВЕСЬ rxlen
           одним CMD53 (block mode через blks для >511, byte/block для <512).
           НЕ function 7+addr0 (даёт нули — W.280 баг). seq++ один раз за приём. */
        static uint32_t rx_seq = 0;
        static uint8_t rxbuf[0x4000 + 512];
        if (rxlen > 0) {
            uint32_t acc = 0;
            /* W.308: drain-цикл эталона (sdio_ops.c:2196-2223). Перечитываем
               0x1C (RX0_REQ_LEN) на каждой итерации и читаем РОВНО size одним
               CMD53, seq++ один раз за пакет (окно). Без cap на 8 блоков:
               эталон читает size целиком (sd_recv_rxfifo). Это выгребает весь
               накопленный буфер (rxlen2 после ch1 = 0x26DC), а не бросает
               остаток в чужое окно след. канала. */
            /* W.317: снимок total из rxlen (0x1C читается ОДИН раз, до цикла — как
               эталон sdio_ops.c:2196-2202, где RX0_REQ_LEN берётся до чтения
               порта). Читаем пакет по 1 блоку (host не тянет многоблок,
               30 блоков → rc=-1), seq ПЕРЕД чтением фиксирован, seq++ —
               только когда весь total вычитан и RX0_REQ_LEN подтвердил 0.
               Если после чтения RX0_REQ_LEN не 0 — пакет не дочитан, повторим
               с тем же seq (без seq++). */
            uint32_t total = (uint32_t)rxlen;
            if (total > 0x4000) total = 0x4000;
            uint32_t off_read = 0;
            int frc = 0;
            uint32_t seq_cur = rx_seq;      /* зафиксировать seq на пакет */
            for (uint32_t g = 0; g < 64 && off_read < total; g++) {
                /* W.319: host SDIO H3 умеет только 1 блок (512 б) за CMD53;
                   многоблок (даже 2) даёт rc=-1 (DATA_STAR_TO). Читаем строго
                   по 1 блоку, seq фиксирован на пакет, off_read накапливается. */
                uint32_t addr = (7u << 13) | (seq_cur & 3u);
                frc = h3_sdc_cmd53_read(1, addr, 1, rxbuf + off_read);
                WLOG(" blk rc=");
                WLOG_INT(frc);
                if (frc != 0) {
                    uart_print(" risr=0x");
                    uart_print_hex(sdc_diag_risr);
                    uart_print(" sta=0x");
                    uart_print_hex(sdc_diag_sta);
                    break;
                }
                off_read += 512u;
            }
            if (frc == 0 && off_read >= total) {
                /* Проверить, что FW подтвердила полное чтение пакета. */
                uint8_t r2[4] = {0,0,0,0};
                if (h3_sdc_cmd53_read_bytes(1, 0x001C, 4, r2) == 0 && (r2[0] | r2[1]) == 0)
                    rx_seq = (seq_cur + 1) & 0xFFFFFFFFu;  /* пакет ушёл → seq++ */
                /* иначе — seq не двигаем, повторим с тем же окном */
            }
            WLOG(" fifo_rd rc=");
            WLOG_INT(frc);
            WLOG(" read=0x");
            WLOG_HEX(off_read);
            acc = (total < off_read) ? total : off_read;
            /* W.315: парсер работает по наличию данных (acc>0), а не по
               frc==0. Лишняя попытка дочитать накопленный объём может
               вернуть rc=-1, но валидно прочитанный первый пакет уже лежит
               в rxbuf — его и разбираем. Ошибку только логируем. */
            if (acc > 0) {
                /* Парсер: off += 24 + drvinfo + plen, round 8.
                   SSID из beacon (fc=0x80 0x00), IE обход после 36. */
                uint32_t off = 0;
                int pkts = 0;
                while (off + 24 <= acc && pkts < 64) {
                    uint32_t plen = (uint32_t)(rxbuf[off] | ((rxbuf[off+1] & 0x3F) << 8));
                    /* W.316: drvinfo из GET_RX_STATUS_DESC_DRVINFO_SIZE_8188F =
                       LE_BITS_TO_4BYTE(pdesc,16,4) = rxbuf[off+2] & 0x0F, <<3.
                       По дампу rxbuf0: fc=0x80 0x00 на off+24, drvinfo_field=0
                       (данных drvinfo нет) → drvsz=0, fr=off+24. БЕЗ fallback:
                       fallback 4 сдвигает fr на 28 и теряет beacon. */
                    uint32_t drvsz = (uint32_t)(rxbuf[off+2] & 0x0Fu) << 3;
                    uint32_t pkt_total = 24u + drvsz + plen;
                    if (plen == 0 || plen > 0x2000 || off + pkt_total > acc) break;
                    uint32_t fr = off + 24u + drvsz;
                    if (fr + 40 <= acc && (rxbuf[fr] & 0xF0) == 0x80) {
                        uart_print(" BCN plen=0x");
                        uart_print_hex(plen);
                        uart_print(" ch=");
                        uart_print_int((int)(ch & 0xFF));
                        uart_print(" ssid=");
                        uint32_t ie = fr + 36u;
                        int got = 0;
                        /* BSSID = addr3 кадра (fr+16), для захвата целевой сети. */
                        uint8_t *p_bssid = &rxbuf[fr + 16];
                        /* Реальный канал AP берём из DS Parameter Set IE (id=3),
                           если он есть — свип ловит off-channel (один BSSID на ch1/5/9/10/11),
                           auth на неверной частоте тонет (rc=-4, W.368). */
                        uint8_t ap_ch = (uint8_t)(ch & 0xFF);
                        /* WPA2: RSN IE (id=0x30) этого BSSID (W.373) — сохраняем
                           локально, включим в AssocReq, если сеть станет целью.
                           Емкость 64 байта (хватит для стандартного RSN). */
                        uint8_t bcn_rsn[64];
                        uint8_t bcn_rsn_len = 0;
                        {
                            uint32_t ie2 = fr + 36u;
                            while (ie2 + 2 <= fr + 24u + plen && ie2 + 2 <= acc) {
                                uint8_t id2 = rxbuf[ie2];
                                uint8_t len2 = rxbuf[ie2+1];
                                if (id2 == 0x30 && len2 >= 2 && len2 <= 62 &&
                                    ie2 + 2 + len2 <= fr + 24u + plen) {
                                    bcn_rsn_len = (uint8_t)(len2 + 2); /* + header id,len */
                                    for (uint32_t j = 0; j < bcn_rsn_len && j < 64; j++)
                                        bcn_rsn[j] = rxbuf[ie2 + j];
                                    break;
                                }
                                ie2 += 2u + len2;
                            }
                        }
                        while (ie + 2 <= fr + 24u + plen && ie + 2 <= acc) {
                            uint8_t id = rxbuf[ie];
                            uint8_t len = rxbuf[ie+1];
                            if (id == 0x03 && len >= 1) { ap_ch = rxbuf[ie+2]; }
                            ie += 2u + len;
                        }
                        ie = fr + 36u;   /* перезапуск обхода IE */
                        while (ie + 2 <= fr + 24u + plen && ie + 2 <= acc) {
                            uint8_t id = rxbuf[ie];
                            uint8_t len = rxbuf[ie+1];
                            if (id == 0x00 && len >= 1 && len <= 32 && ie + 2 + len <= fr + 24u + plen) {
                                int ok = 1;
                                for (uint32_t j = 0; j < len; j++) {
                                    uint8_t cc = rxbuf[ie+2+j];
                                    /* W.365: разрешаем UTF-8 (байты >=0x80) —
                                       SSID может содержать русские буквы
                                       (A05s пользователя М). Блокируем только
                                       управляющие < 0x20. */
                                    if (cc < 0x20) { ok = 0; break; }
                                }
                                if (ok) {
                                    for (uint32_t j = 0; j < len; j++) uart_putc((char)rxbuf[ie+2+j]);
                                    got = 1;
                                    /* Добавить сеть в список для UI «Transfer»
                                       (SSID, канал, BSSID; rssi из дескриптора
                                       пока не берём). */
                                    {
                                        struct h3_wifi_ap ap;
                                        ap.ssid_len = (uint8_t)len;
                                        for (uint32_t j = 0; j < len; j++)
                                            ap.ssid[j] = (char)rxbuf[ie+2+j];
                                        ap.ssid[len] = 0;
                                        ap.channel = ap_ch;
                                        for (int k = 0; k < 6; k++) ap.bssid[k] = p_bssid[k];
                                        ap.rssi_lvl = 0;
                                        /* WPA2: RSN IE (id=0x30) этой сети — чтобы
                                           UI-connect мог передать его в AssocReq. */
                                        ap.rsn_len = bcn_rsn_len;
                                        for (uint32_t j = 0; j < bcn_rsn_len; j++)
                                            ap.rsn_ie[j] = bcn_rsn[j];
                                        h3_wifi_scan_ap_add(&ap);
                                    }
                                    /* Захват BSSID/channel целевой сети (для
                                       JOINBSS): по ASCII-префиксу join_target.ssid
                                       ("A05s"); при совпадении копируем ПОЛНЫЙ
                                       SSID (в т.ч. UTF-8-хвост) из beacon'а —
                                       чтобы AssocReq нёс точный SSID эфира. */
                                    if (join_target.ssid_len > 0 &&
                                        len >= join_target.ssid_len) {
                                        int same = 1;
                                        for (uint32_t j = 0; j < join_target.ssid_len; j++)
                                            if (rxbuf[ie+2+j] != (uint8_t)join_target.ssid[j]) { same = 0; break; }
                                        if (same && !join_target.bssid_captured) {
                                            for (uint32_t j = 0; j < len; j++)
                                                join_target.ssid[j] = (uint8_t)rxbuf[ie+2+j];
                                            join_target.ssid_len = (uint8_t)len;
                                            join_target.ssid[len] = 0;
                                            /* RSN IE (WPA2) этого BSSID — для AssocReq. */
                                            join_target.rsn_len = bcn_rsn_len;
                                            for (uint32_t j = 0; j < bcn_rsn_len; j++)
                                                join_target.rsn_ie[j] = bcn_rsn[j];
                                            /* WPA2: задать passphrase по SSID уже на захвате
                                               цели (не только в wifi_layer_connect) — чтобы
                                               автоjoin из kmain тоже вёл 4-way handshake. */
                                            if (bcn_rsn_len > 0 && len >= 4 &&
                                                rxbuf[ie+2]=='A' && rxbuf[ie+3]=='0' &&
                                                rxbuf[ie+4]=='5' && rxbuf[ie+5]=='s')
                                                h3_wifi_set_passphrase("8645449Qq", 9);
                                            h3_wifi_scan_capture_target(p_bssid, ap_ch);
                                        }
                                    }
                                }
                                break;
                            }
                            ie += 2u + len;
                        }
                        if (!got) uart_print("-");
                        uart_print("\r\n");
                        bcn_printed = 1;
                        pkts++;
                    }
                    off += pkt_total;
                    off = (off + 7u) & ~7u;
                }
            }
            /* После выдачи проверить, что RX0_REQ_LEN сбросился. */
            {
                uint8_t rxl2[4] = {0, 0, 0, 0};
                h3_sdc_cmd53_read_bytes(1, 0x001C, 4, rxl2);
                uint16_t rxlen2 = (uint16_t)(rxl2[0] | ((uint16_t)rxl2[1] << 8));
                WLOG(" rxlen2=0x");
                WLOG_HEX((uint32_t)rxlen2);
                (void)rxlen2;   /* при выкл. verbose не используется */
            }
        }
        /* W.277: HMETFR (0x1CC, 32-бит) в свипе: valid-биты боксов. Если
           какой-то бокс застрял (бит=1) — FW не отпустила команду. */
        {
            uint32_t hmfr = 0;
            h3_sdc_read_wlan_reg32(0x01CC, &hmfr);
            WLOG(" hmetfr=0x");
            WLOG_HEX(hmfr);
            WLOG("(b");
            for (int bx = 0; bx < 4; bx++)
                WLOG_INT((int)((hmfr >> bx) & 1));
            WLOG(")");
        }
        /* RXPKT_NUM (0x284 dev8, полное 32-битное слово): счётчик пакетов
           во внутреннем RXPKTBUF FW. Если !=0 — FW приняла кадры. Раньше
           читали только байт 0x287 (см. pkt ниже) — полное слово покажет
           точное число. */
        h3_sdc_read_wlan_reg32((8u << 13) | 0x0284, &d);
        WLOG(" rxpkt=0x");
        WLOG_HEX(d);
        /* RXDMA_CTRL (0x286 dev8, байт): bit3 = «RX DMA has req» — FW
           сформировала запрос на выдачу пакета хосту (hal_com.c:1634).
           Если bit3=1, а rxlen=0 — пакет есть, но не выложен. */
        h3_sdc_read_wlan_reg8((8u << 13) | 0x0286, &num);
        WLOG(" req=");
        WLOG_INT((int)((num >> 3) & 1));
        WLOG(" dr=0x");
        WLOG_HEX((uint32_t)num);
        /* RXPKT_NUM байт 0x287 (для совместимости с прошлыми логами). */
        h3_sdc_read_wlan_reg8((8u << 13) | 0x0287, &num);
        WLOG(" pkt=0x");
        WLOG_HEX((uint32_t)num);
        /* W.279: разбор 0x284 по битам: [15:0]=RXPKT_NUM, bit16=RXPKT_RELEASE,
           bit17=RXDMA_IDLE, bit18=RW_RELEASE_EN (hal_com_reg.h:187-188). */
        {
            uint32_t rxpk = d;
            WLOG(" cnt=");
            WLOG_HEX(rxpk & 0xFFFF);
            WLOG(" b16="); WLOG_INT((int)((rxpk >> 16) & 1));
            WLOG(" b17="); WLOG_INT((int)((rxpk >> 17) & 1));
            WLOG(" b18="); WLOG_INT((int)((rxpk >> 18) & 1));
            (void)rxpk;   /* при выкл. verbose не используется */
        }
        /* W.279: TRXFF_STATUS (0x011C, 32-бит) — RXFF_PTR/RXFF_WR_PTR живого
           движется (D7: 0x37003700); у нас читался байт по смещению — теперь
           полное слово, чтобы увидеть указатель записи/чтения RX FIFO. */
        {
            uint32_t trxst = 0;
            h3_sdc_read_wlan_reg32((8u << 13) | 0x011C, &trxst);
            WLOG(" trxst=0x");
            WLOG_HEX(trxst);
        }
        /* W.257: ревизия диагностики. 0xC4C (rOFDM0_ECCAThreshold) — статичный
           порог, НЕ индикатор приёма (драйвер его как статус RX не читает).
           Заменён на живой HISR (0x18 dev0): бит0 = RX_REQUEST — FW собрала
           пакет и ждёт выдачи хосту (sdio_ops.c:2187). cca=0xA08 / trssi=0xA50
           — report-регистры CCK (в драйвере не читаются; только контекст),
           rxrpt=0xA54 — живой отчёт CCK (на ch11 даёт 0x...8D09). */
        h3_sdc_read_wlan_reg32(0x0A08, &d);
        /* W.267: cca/trssi/rxrpt убраны из per-итерации — самый дорогой
           вывод цикла (4 MMIO-чтения + ~200 символов UART ~17 мс на 115200).
           Оставлены только критичные индикаторы: rxlen/rxpkt/req/pkt/hisr. */
        (void)d;
        /* W.280: HISR (0x18, SDIO-local) читаем 32-бит через CMD53 fn1 — как
           ReadInterrupt8188FSdio (sdio_ops.c:1627-1630: байты 0x18..0x1B).
           Через CMD52 fn0 HISR не виден (доказано W.276-W.279: rxlen тоже
           читался CMD52 fn0 = 0, стал 0x100 после CMD53). */
        {
            uint8_t hisr4[4] = {0, 0, 0, 0};
            h3_sdc_cmd53_read_bytes(1, 0x0018, 4, hisr4);
uint32_t hisr32 = (uint32_t)hisr4[0] | ((uint32_t)hisr4[1] << 8)
                            | ((uint32_t)hisr4[2] << 16) | ((uint32_t)hisr4[3] << 24);
            (void)hisr32;   /* используется только в WLOG (verbose) */
            WLOG(" hisr=0x");
            WLOG_HEX(hisr32);
            WLOG("(b0=");
            WLOG_INT((int)(hisr32 & 1));
            WLOG(" b17=");
            WLOG_INT((int)((hisr32 >> 17) & 1));
            WLOG(")");
        }
        if (!bcn_printed) uart_print("-\r\n");   /* канал без beacon: псевдо-строка */
        uart_flush();   /* W.278: слить лог итерации до перехода на след. канал */
    }

    /* HISR после свипа — поднялся ли RX_REQUEST (bit0) от FW. */
    {
        uint8_t hisr_a[4] = {0, 0, 0, 0};
        h3_sdc_cmd53_read_bytes(1, 0x0018, 4, hisr_a);
        uint32_t hisr32 = (uint32_t)hisr_a[0] | ((uint32_t)hisr_a[1] << 8)
                        | ((uint32_t)hisr_a[2] << 16) | ((uint32_t)hisr_a[3] << 24);
        uart_print("  rx: hisr_after=0x");
        uart_print_hex(hisr32);
        uart_print("(b0=");
        uart_print_int((int)(hisr32 & 1));
        uart_print(" b17=");
        uart_print_int((int)((hisr32 >> 17) & 1));
        uart_print(")\r\n");
    }

    /* ===== W.266: C2H-ping. Показать, что 8051 читает H2C и отвечает
       C2H по регистровому пути (0x1A0), НЕ через RX-FIFO. =====
       Команда: H2C_8188F_AP_REQ_TXRPT (0x43), len=2, macid1=1, macid2=0
       (rtl8188f_cmd.c:537-549). FW в ответ после probe шлёт C2H_CCX_TX_RPT
       (id=0x03) в REG_C2HEVT_CMD_ID (0x1A0), CLEAR ставит C2H_EVT_FW_CLOSE
       (0xFF). Детект по 3 путям: 0x1A0 (C2H-регистр), SDIO HISR bit17
       (0x18 fn0), RXPKT_NUM bit16. */
    {
        unsigned int tt = 0;
        uint8_t cv = 0xFF;
        uint32_t hisr = 0;
        uint32_t rxn = 0;
        uint32_t hmfr = 0;
        uint32_t c2hid = 0, c2h0 = 0, c2h1 = 0;

        /* ждать, пока FW заберёт предыдущую H2C (valid==0). */
        do {
            udelay(1000);
            h3_sdc_read_wlan_reg8(0x01CC, &cv);
        } while ((cv & 0x01u) && (++tt < 100));
        /* W.277: полный HMETFR после ожидания — какие боксы свободны. */
        h3_sdc_read_wlan_reg32(0x01CC, &hmfr);

        /* box0 = {id 0x43, macid1=1, macid2=0, 0} */
        h3_sdc_write_wlan_reg32(0x01D0, 0x43u | (0x01u << 8) | (0x00u << 16));

        /* Дать FW время обработать H2C и сгенерировать C2H. */
        udelay(30000);

        /* W.345: перед чтением C2H явно закрыть событие (0x1AF=0, host close)
           и подтвердить readback. Если старый C2H из прошлой сессии висит и его
           не взять — FW новый не пришлёт (C2H-ABSENT «залочен»). */
        {
            uint8_t clr = 0xFF;
            h3_sdc_write_wlan_reg8(0x01AF, 0x00);
            h3_sdc_read_wlan_reg8(0x01AF, &clr);
            if (clr != 0) uart_print("  rx: c2h_clr_trapped=0x");
            else uart_print("  rx: c2h_clr_ok=0x");
            uart_print_hex((uint32_t)clr);
            uart_print("\r\n");
        }

        h3_sdc_read_wlan_reg32(0x01A0, &c2hid);      /* C2HEVT_CMD_ID/LEN/SEQ */
        h3_sdc_read_wlan_reg32(0x01A4, &c2h0);       /* payload [0..3] */
        h3_sdc_read_wlan_reg32(0x01A8, &c2h1);       /* payload [4..7] */
        {
            /* W.280: HISR 32-бит через CMD53 fn1 (как выше). */
            uint8_t hisr4[4] = {0, 0, 0, 0};
            h3_sdc_cmd53_read_bytes(1, 0x0018, 4, hisr4);
            hisr = (uint8_t)((uint32_t)hisr4[0] | ((uint32_t)hisr4[1] << 8)
                   | ((uint32_t)hisr4[2] << 16) | ((uint32_t)hisr4[3] << 24));
        }
        h3_sdc_read_wlan_reg32(0x0284, &rxn);        /* RXPKT_NUM */

        uart_print("  rx: c2h_ping 0x1A0=0x");
        uart_print_hex(c2hid);
        uart_print(" payload=0x");
        uart_print_hex(c2h0);
        uart_print("0x");
        uart_print_hex(c2h1);
        uart_print(" hisr=0x");
        uart_print_hex((uint32_t)hisr);
        uart_print(" rxpk=0x");
        uart_print_hex(rxn);
        uart_print(" hmetfr=0x");
        uart_print_hex(hmfr);
        if (c2hid & 0xFu)
            uart_print(" C2H-PRESENT");
        else
            uart_print(" C2H-ABSENT");
        uart_print("\r\n");

        /* Захватилить C2H: CLEAR (0x1AF) = C2H_EVT_HOST_CLOSE (0x00). */
        h3_sdc_write_wlan_reg8(0x01AF, 0x00);
    }

    uart_print("  rx: done\r\n");
}

/* ---- Загрузка прошивки чипа RTL8189FTV (реализация rtl8188f_FirmwareDownload) ----
 *
 * Протокол (из rtl8188f_hal_init.c):
 *   1) _FWDownloadEnable(1): REG_SYS_FUNC_EN[1]|=0x04 (8051 enable),
 *      REG_MCUFWDL(0x80) |= 0x01, REG_MCUFWDL[2] &= 0xf7 (8051 reset).
 *   2) Записать прошивку блоками CMD53 (block write) начиная с FW_START=0x1000.
 *   3) _FWFreeToGo: REG_MCUFWDL[0] |= MCUFWDL_RDY(bit1), &= ~WINTINI_RDY(bit6),
 *      8051 reset, затем поллить FW ready (WINTINI_RDY|FWDL_ChkSum_rpt|MCUFWDL_RDY).
 *
 * Константы (hal8188freg.h): REG_MCUFWDL=0x80, MCUFWDL_RDY=BIT1, WINTINI_RDY=BIT6,
 *   RAM_DL_SEL=BIT7, FW_8188F_START_ADDRESS=0x1000, размер 0x8000, страница 4096.
 */

/* Макро-хелпер: запись одного байта в WLAN IOREG (MAC base) через CMD52. */
#define WREG8(reg, val)  pseq_write8(0, (reg), (val))
#define RREG8(reg, dst)  pseq_read8(0, (reg), (dst))

/* Полная загрузка прошивки. fw — массив, size — размер. Возвращает 0 при успехе.
 * Код повторяет rtl8188f_FirmwareDownload (rtl8188f_hal_init.c): порядок:
 *   1) _FWDownloadEnable(TRUE): 0x03|=0x04, 0x80|=0x01, поллинг, 0x82&=0xf7;
 *   2) взвести FWDL_ChkSum_rpt (0x80|=BIT2) — ДО записи FW (стр.1251);
 *   3) записать прошивку постранично byte-mode CMD53 (по 4 байта);
 *   4) _FWDownloadEnable(FALSE): 0x80 &= ~BIT0 (снять FWDL);
 *   5) _FWFreeToGo: 0x80 |= MCUFWDL_RDY(BIT1), &= ~WINTINI_RDY(BIT6),
 *      полный _8051Reset8188 (сначала 0x82&=0xf7, потом reset 8051),
 *      поллинг WINTINI_RDY|FWDL_ChkSum_rpt|MCUFWDL_RDY|RAM_DL_SEL (BIT7). */
/* Одна попытка записи образа прошивки: сброс chksum -> постраничная запись ->
   поллинг отчёта chksum. Возвращает 0, если контрольная сумма подтверждена.
   По драйверу (rtl8188f_hal_init.c):
     - сброс FWDL_ChkSum_rpt ПЕРЕД записью (:1251);
     - страницы по MAX_DLFW_PAGE_SIZE = 4096, выбор страницы REG_MCUFWDL+2[2:0] (:190-191);
     - хвост страницы (1..3 байта) пишется как остаток запроса, без добивки
       нулями (:156-174);
     - затем polling_fwdl_chksum, минимум 5 попыток / 50 мс (:303-332). */
static int fw_write_image(const uint8_t *b, uint32_t size)
{
    uint8_t t;
    int w;

    if (RREG8(REG_MCUFWDL_OFF, &t)) return -7;
    WREG8(REG_MCUFWDL_OFF, (uint8_t)(t | FWDL_ChkSum_rpt));

    uint32_t page = 0;
    uint32_t off = 0;
    while (off < size) {
        uint32_t page_end = off + 4096;   /* граница текущей страницы */
        if (page_end > size) page_end = size;

        /* Выбрать страницу: REG_MCUFWDL+2[2:0] = page. */
        if (RREG8(REG_MCUFWDL_OFF + 2, &t)) return -6;
        t = (uint8_t)((t & 0xF8) | (page & 0x07));
        WREG8(REG_MCUFWDL_OFF + 2, t);

        uart_print("  fwdl page=");
        uart_print_int((int)page);
        uart_print(" off=0x");
        uart_print_hex(off);
        uart_print(" len=0x");
        uart_print_hex(page_end - off);
        uart_print("\r\n");

        /* Запись страницы байтовыми CMD53 по 4 байта (как rtw_write32): базовый
           адрес FW_START, page-select перенаправляет в нужную RMAC/IROM-страницу,
           внутри страницы адрес автоинкрементируется по 4. */
        uint32_t ftaddr = (8u << 13) | (FW_8188F_START & 0x7FFFu);
        uint32_t n4 = (page_end - off) / 4;   /* полных слов */
        uint32_t r4 = (page_end - off) % 4;   /* остаток байт */

        for (uint32_t i = 0; i < n4; i++) {
            int rc = h3_sdc_cmd53_write_bytes(1, ftaddr + i * 4, 4,
                                              (const void *)(b + off + i * 4));
            if (rc != 0) {
                sdc_diag_fail_blk = (int)((off + i * 4) / 512);
                uart_print("  fwdl CMD53W FAIL rc=");
                uart_print_int(rc);
                uart_print("\r\n");
                return -40 - rc;
            }
        }
        if (r4) {
            /* Хвост: ровно r4 байт (1..3), как P3 у драйвера (rtl8188f_hal_init.c:166-173).
               Добивка нулями до слова смещает образ и ломает chksum. */
            int rc = h3_sdc_cmd53_write_bytes(1, ftaddr + n4 * 4, r4,
                                              (const void *)(b + off + n4 * 4));
            if (rc != 0) {
                sdc_diag_fail_blk = (int)((off + n4 * 4) / 512);
                uart_print("  fwdl CMD53W tail FAIL rc=");
                uart_print_int(rc);
                uart_print("\r\n");
                return -40 - rc;
            }
        }

        udelay(5000);
        off = page_end;
        page++;
    }

    /* Поллинг отчёта контрольной суммы. */
    for (w = 0; w < 500; w++) {
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -35;
        if (t & FWDL_ChkSum_rpt) return 0;
        udelay(100);
    }
    uart_print("  fwdl: chksum TIMEOUT\r\n");
    return -38;
}

int h3_wifi_download_fw(const void *fw, uint32_t size)
{
    const uint8_t *b = (const uint8_t *)fw;
    uint32_t i;
    int rc;

    if (size < 4) return -9;
    /* Проверка сигнатуры: 0xF1 0x88 (чип 8188F), + 0x10 0x00. */
    if (b[0] != 0xF1 || b[1] != 0x88) {
        uart_print("  fwdl: BAD SIG 0x");
        uart_print_hex((uint32_t)b[1]);
        uart_print_hex((uint32_t)b[0]);
        uart_print("\r\n");
        return -10;
    }
    uart_print("  fwdl: sig OK, size=0x");
    uart_print_hex(size);
    uart_print(" (raw=0x");
    uart_print_hex((uint32_t)(b[0] | ((uint16_t)b[1] << 8)));
    uart_print(")\r\n");

    /* RTL8189F NIC-прошивка содержит 32-байтный заголовок
       (RT_8188F_FIRMWARE_HDR). Драйвер его пропускает перед загрузкой
       (rtl8188f_hal_init.c: IS_FW_HEADER_EXIST_8188F -> pFirmwareBuf += 32):
       в IRAM 8051 грузится ТОЛЬКО код, заголовок не пишется. */
    if (size > 32 && ((b[0] | ((uint16_t)b[1] << 8)) & 0xFFF0) == 0x88F0) {
        b += 32;
        size -= 32;
    }

    /* 0) W.248FIX: возврат к проверенной сетке (HEAD/W.240: fwdl rc=0, hiq_free=12.)
          Для загрузки прошивки НЕ переходим на 4-bit: рабочая шина стенда —
          1-bit/50 МГц без pinctrl (W.235-W.237: и pinctrl, и 4-bit ломают
          data-фазу CMD53). Ставим 1-bit через h3_sdio_set_bus_width(0), блок
          size Fn1 512 (как sdio_set_block_size(func,512)); сама прошивка
          пишется в byte mode, как rtw_write32 -> sdio_writel. */
    h3_sdio_set_bus_width(0);
    udelay(1000);
    if ((rc = h3_sdc_set_fn1_blocksize(512)) != 0) {
        uart_print("  fwdl: fn1_blksz rc=");
        uart_print_int(rc);
        uart_print("\r\n");
        return -8;
    }
    uart_print("  fwdl: fn1 blksz=512 ok, bus 1-bit\r\n");

    /* 0) Если FW уже исполняется из RAM (RAM_DL_SEL), сбросить 8051 перед новой
       загрузкой — как драйвер (rtl8188f_hal_init.c:1241-1244). Без этого повторная
       загрузка (после reset/перезапуска) ломается. _8051Reset8188: 0x1D.bit0
       снять->взвести, 0x03.bit2 снять->взвести (:264-301). */
    {
        uint8_t t;
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -1;
        if (t & RAM_DL_SEL) {
            uart_print("  fwdl: RAM_DL_SEL set, reset 8051\r\n");
            WREG8(REG_MCUFWDL_OFF, (uint8_t)(t & ~RAM_DL_SEL));
            if (pseq_read8(0, 0x1D, &t)) return -2;
            WREG8(0x1D, (uint8_t)(t & 0xFE));
            if (pseq_read8(0, 0x03, &t)) return -3;
            WREG8(0x03, (uint8_t)(t & 0xFB));
            if (pseq_read8(0, 0x1D, &t)) return -4;
            WREG8(0x1D, (uint8_t)(t | 0x01));
            if (pseq_read8(0, 0x03, &t)) return -5;
            WREG8(0x03, (uint8_t)(t | 0x04));
            udelay(1000);
        }
    }

    /* 1) Включить MCU FW download: полный _FWDownloadEnable (rtl8188f_hal_init.c):
       REG_SYS_FUNC_EN+1 |= 0x04 (8051 enable), REG_MCUFWDL |= 0x01 (взвод),
       подтвердить что бит встал, затем REG_MCUFWDL+2 &= 0xf7 (8051 reset). */
    {
        uint8_t t;
        /* 8051 enable (REG_SYS_FUNC_EN = 0x02, +1 байт = адрес 0x03). */
        if (pseq_read8(0, 0x03, &t)) return -1;
        WREG8(0x03, t | 0x04);
        /* Взвести бит FWDL (REG_MCUFWDL = 0x80). */
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -2;
        WREG8(REG_MCUFWDL_OFF, t | 0x01);
        /* Подтвердить, что бит встал (поллинг до 100 попыток). */
        int tries;
        for (tries = 0; tries < 100; tries++) {
            if (RREG8(REG_MCUFWDL_OFF, &t)) return -3;
            if (t & 0x01) break;
            udelay(1000);
        }
        if (tries >= 100) return -4;
        /* 8051 reset: REG_MCUFWDL+2 &= 0xf7. */
        if (RREG8(REG_MCUFWDL_OFF + 2, &t)) return -5;
        WREG8(REG_MCUFWDL_OFF + 2, t & 0xf7);
        udelay(1000);
    }

    /* 2-4) Запись прошивки с повторами: драйвер повторяет цикл
       «сброс chksum -> запись -> поллинг chksum» до 3 раз (rtl8188f_hal_init.c:1248-1260). */
    {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            if (fw_write_image(b, size) == 0) break;
            uart_print("  fwdl: attempt ");
            uart_print_int(attempt + 1);
            uart_print(" failed, retry\r\n");
        }
        if (attempt >= 3) return -37;
        uart_print("  fwdl: chksum OK\r\n");
    }

    /* 4) _FWDownloadEnable(FALSE): снять FWDL (0x80 &= ~BIT0) до старта FW. */
    {
        uint8_t t;
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -40;
        WREG8(REG_MCUFWDL_OFF, (uint8_t)(t & 0xFE));
        uart_print("  fwdl: FWDL off, mcufwdl=0x");
        uart_print_hex((uint32_t)t);
        uart_print("\r\n");
    }

    /* 5) FreeToGo (rtl8188f_hal_init.c _FWFreeToGo):
       0x80 |= MCUFWDL_RDY(BIT1); 0x80 &= ~WINTINI_RDY(BIT6);
       полный _8051Reset8188: REG_RSV_CTRL+1 (0x1D) bit0 снять->взвести,
       REG_SYS_FUNC_EN+1 (0x03) bit2 снять->взвести;
       поллинг MCUFWDL_RDY|FWDL_ChkSum_rpt|WINTINI_RDY|RAM_DL_SEL (BIT7). */
    {
        uint8_t t;
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -30;
        t |= MCUFWDL_RDY;           /* bit1 */
        t &= (uint8_t)~WINTINI_RDY; /* bit6 сброшен */
        WREG8(REG_MCUFWDL_OFF, t);

        /* Полный reset 8051 — как драйвер _8051Reset8188: */
        /*   io_rst: REG_RSV_CTRL+1 &= ~BIT0, затем |= BIT0 (0x1D) */
        if (pseq_read8(0, 0x1D, &t)) return -31;
        WREG8(0x1D, (uint8_t)(t & 0xFE));
        /*   cpu_rst: REG_SYS_FUNC_EN+1 (0x03) &= ~BIT2 */
        if (pseq_read8(0, 0x03, &t)) return -32;
        WREG8(0x03, (uint8_t)(t & 0xFB));
        /*   io_rst again |= BIT0 */
        if (pseq_read8(0, 0x1D, &t)) return -33;
        WREG8(0x1D, (uint8_t)(t | 0x01));
        /*   cpu_rst |= BIT2 */
        if (pseq_read8(0, 0x03, &t)) return -34;
        WREG8(0x03, (uint8_t)(t | 0x04));
        udelay(1000);

        /* Поллить FW ready. want-маск — как драйвер (value_expected):
           MCUFWDL_RDY | FWDL_ChkSum_rpt | WINTINI_RDY | RAM_DL_SEL. */
        uint32_t want = MCUFWDL_RDY | FWDL_ChkSum_rpt | WINTINI_RDY | RAM_DL_SEL;
        for (i = 0; i < 100000; i++) {
            uint8_t v;
            if (RREG8(REG_MCUFWDL_OFF, &v)) return -35;
            if ((v & want) == want) break;
            udelay(100);
        }
        if (i >= 100000) {   /* FW не поднялся */
            uint8_t v = 0;
            RREG8(REG_MCUFWDL_OFF, &v);
            sdc_diag_fw_mcufwdl = v;
            uart_print("  fwdl: FreeToGo TIMEOUT mcufwdl=0x");
            uart_print_hex((uint32_t)v);
            uart_print("\r\n");
            return -33;
        }
        uart_print("  fwdl: FW READY, want-маска выставлена\r\n");

        /* W.237: 4-bit не включаем — рабочая шина 1-bit/50 МГц
           (стенд: и pinctrl, и 4-bit ломают data-фазу CMD53). Контроль. */
        {
            uint8_t bwv = 0;
            h3_sdc_cmd52_read(0, 0x07, &bwv);
            uart_print("  fwdl: bus 1-bit, cccr=0x");
            uart_print_hex((uint32_t)bwv);
            uart_print(" ctype=0x");
            uart_print_hex(SDC_REG(SDC_CTYPE));
            uart_print("\r\n");
        }
        return 0;
    }
}

/* SDIO CMD53 — блочный доступ к адресному пространству функции чипа.
 *
 * Аргумент (SDIO-спецификация):
 *   bit31     = write flag (1=write, 0=read)
 *   bits30:28 = function number (3 бита)
 *   bit27     = 1 — block mode (0 — byte mode)
 *   bit26     = 1 — OP code increment (адрес автоинкрементируется)
 *   bits25:9  = register/base address
 *   bits8:0   = число блоков (в block mode)
 *
 * blks — число блоков по 512 байт. Блоки читаются подряд из addr.
 * data — буфер (blks*512 байт). Возвращает 0 при успехе, не-0 — ошибка. */
int h3_sdc_cmd53_read(uint32_t func, uint32_t addr, uint32_t blks, void *data)
{
    uint32_t arg = (0u << 31)                       /* read */
                 | ((func & 7u) << 28)              /* функция */
                 | (1u << 27)                       /* block mode */
                 | (1u << 26)                       /* increment addr */
                 | ((addr & 0x1FFFF) << 9)          /* база */
                 | (blks & 0x1FF);                  /* число блоков */
    return h3_sdc_block_transfer(53, arg, 512, blks, 0, data);
}

/* SDIO CMD53 — блочная ЗАПИСЬ в адресное пространство функции чипа.
   Адресация как у CMD53-read: function + block mode + increment.
   Используется для загрузки прошивки чипа (WLAN IOREG, function 1).
   blks — число блоков по 512 байт. Возвращает 0 при успехе, не-0 — ошибка. */
int h3_sdc_cmd53_write(uint32_t func, uint32_t addr, uint32_t blks, const void *data)
{
    uint32_t arg = (1u << 31)                       /* write */
                 | ((func & 7u) << 28)              /* функция */
                 | (1u << 27)                       /* block mode */
                 | (1u << 26)                       /* increment addr */
                 | ((addr & 0x1FFFF) << 9)          /* база */
                 | (blks & 0x1FF);                  /* число блоков */
    return h3_sdc_block_transfer(53, arg, 512, blks, 1, (void *)data);
}

/* Полная инициализация SDIO-карты (RTL8189FTV) по спецификации SDIO.
 *
 * RTL8189FTV — чистая I/O-карта: CMD0 не нужен (сброс через CMD52 в CCCR).
 * Последовательность: CMD5 -> CMD3 -> CMD7 -> CMD52/CMD53.
 */
int h3_sdio_init_chip(void)
{
    uint32_t rca;
    int rc;
    int tries;

    /* 1) Подождать стабилизации питания чипа (после WIFI_EN). */
    udelay(100000);

    /* 2) CMD5 (IO_SEND_OP_COND): цикл, пока R4 не вернёт C=1 (готовность).
          Аргумент: bits[23:21] = 0b111 (2.7-3.6V), остальное 0. */
    for (tries = 0; tries < 100; tries++) {
        uint32_t ocr_arg = (0x7u << 21);   /* запрос 3.0-3.6V */
        rc = h3_sdc_cmd_no_data_flags(5, ocr_arg, 0, 1, 0);
        if (rc != 0) {
            uart_print("  CMD5 rc=");
            uart_print_int(rc);
            uart_print("\r\n");
            return rc;
        }
        uint32_t resp = SDC_REG(SDC_RESP0);
        uart_print("  CMD5 R4=0x");
        uart_print_hex(resp);
        uart_print(" C=");
        uart_print_int((int)((resp >> 31) & 1u));
        uart_print(" vol=");
        uart_print_hex((resp >> 21) & 0x7u);
        uart_print("\r\n");
        if (resp & (1u << 31))   /* C=1 - карта готова */
            break;
        udelay(10000);
    }
    if (tries >= 100) return -3;   /* таймаут CMD5 */

    /* 3) CMD3 (SEND_RELATIVE_ADDR): получить RCA. */
    rc = h3_sdc_cmd_no_data_flags(3, 0, 0, 1, 0);
    if (rc != 0) return -4;
    rca = (SDC_REG(SDC_RESP0) >> 16) & 0xFFFF;
    uart_print("  CMD3 RCA=0x");
    uart_print_hex(rca);
    uart_print("\r\n");

    /* 4) CMD7 (SELECT_CARD): выбрать карту. */
    rc = h3_sdc_cmd_no_data_flags(7, rca << 16, 0, 1, 0);
    uart_print("  CMD7 select rc=");
    uart_print_int(rc);
    uart_print("\r\n");
    if (rc != 0) return -6;

    /* 5) W.248FIX: убран новый блок «50 МГц + PLLPERI + фазы + CKCR DIV=0».
        Heго в рабочей сетке (W.240, fwdl rc=0, hiq_free=12) НЕ БЫЛО:
        h3_sdio_init_chip заканчивался на CMD7 и сразу возвращал 0.
        Такт (init ~375 кГц затем 50 МГц) и 1-bit ставятся в h3_sdc1_init
        (шаг 6) и h3_wifi_download_fw (h3_sdio_set_bus_width(0)). Добавленный
        здесь переход на PLLPERI/50МГц с DIV=0 ломает data-фазу CMD53 при
        загрузке FW (fwdl rc=-37, CMD53W FAIL rc=-1). Возврат к проверенному. */

    return 0;
}

int h3_sdc_block_transfer(uint32_t cmd, uint32_t arg, uint32_t blksz,
                          uint32_t blkcnt, int write, void *data)
{
    volatile uint32_t *fifo = (volatile uint32_t *)(SDC_BASE + SDC_FIFO);
    const uint32_t *in  = (const uint32_t *)data;
    uint32_t *out = (uint32_t *)data;
    uint32_t words = (blksz * blkcnt) / 4;
    uint32_t rem   = (blksz * blkcnt) % 4;
    uint32_t i, status;
    uint32_t cmdr_val;

    /* Возвращаемые коды: 0 успех, -1 ошибка ожидания FIFO, -2 ошибка ответа,
       -3 таймаут команды/данных, -4 ошибка регистра. */

    /* Диагностика. */
    sdc_diag_cmd  = cmd;
    sdc_diag_arg  = arg;
    sdc_diag_cmdr = 0;
    sdc_diag_risr = 0;
    sdc_diag_sta  = 0;

    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;

    SDC_REG(SDC_BLKSZ) = blksz;              /* размер блока */
    SDC_REG(SDC_BYCNT) = blksz * blkcnt;     /* всего байт */
    SDC_REG(SDC_TMOR)  = 0xFFFFFFFFu;        /* максимальный таймаут передачи */
    SDC_REG(SDC_ARG)   = arg;

/* FIFO-доступ в режиме AHB (как в референсе mmc_bsp: GCTL_FIFO_AC_MODE_AHB). */
    SDC_REG(SDC_GCTL) |= (1u << 31);
    __asm volatile("dmb sy" ::: "memory");

    /* Для SDIO-команд (53) CRC ответа не проверяется (как и для CMD52).
       Проверку CRC включаем только для «обычных» команд (2, 9 и т.п.). */
    int check_crc = (cmd != 53);
    if (write) {
        /* Запись: стартуем CMD перед наполнением FIFO (порядок как в mmc_bsp.c:
           CMD ставится заранее, затем данные пишутся в FIFO по мере опустошения).
           Важно: FIFO глубиной 16 слов, блок 512 байт = 128 слов, поэтому пишем
           в цикле, ожидая, когда контроллер освободит FIFO. */
        cmdr_val = SDC_CMDR_START
                 | (cmd & SDC_CMDR_IDX_MASK)
                 | SDC_CMDR_RESP_RCV
                 | (check_crc ? SDC_CMDR_CHK_CRC : 0)
                 | SDC_CMDR_DATA_TRANS
                 | SDC_CMDR_TRANS_DIR
                 | SDC_CMDR_WAIT_PRE;
        SDC_REG(SDC_CMDR) = cmdr_val;
        sdc_diag_cmdr = cmdr_val;
        __asm volatile("dmb sy" ::: "memory");
        for (i = 0; i < words; i++) {
            if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_STAR), SDC_STAR_FIFO_FULL, 0) != 0) {
                sdc_diag_risr = SDC_REG(SDC_RISR);
                sdc_diag_sta = SDC_REG(SDC_STAR);
                return -1;
            }
            fifo[0] = in[i];   /* FIFO-регистр (0x200), контроллер сам буферизует */
        }
        /* Хвост (total не кратен 4): собрать последнее слово из остатка байт
           (старшие байты нулевые) и записать — BYCNT требует все total байт. */
        if (rem) {
            const uint8_t *ib = (const uint8_t *)data;
            uint32_t w = 0;
            for (i = 0; i < rem; i++)
                w |= ((uint32_t)ib[words * 4 + i]) << (8 * i);
            if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_STAR), SDC_STAR_FIFO_FULL, 0) != 0) {
                sdc_diag_risr = SDC_REG(SDC_RISR);
                sdc_diag_sta = SDC_REG(SDC_STAR);
                return -1;
            }
            fifo[0] = w;
        }
        /* Дать контроллеру выгрести FIFO на шину. Без этого ожидание
           завершения (ниже) стартует, пока FIFO ещё полон, и RTL не получает
           данные; для записи данные реально уходят только когда FIFO пуст. */
        if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_STAR), SDC_STAR_FIFO_EMPTY, 1) != 0) {
            sdc_diag_risr = SDC_REG(SDC_RISR);
            sdc_diag_sta = SDC_REG(SDC_STAR);
            return -1;
        }
    } else {
        /* Чтение: старт CMD, затем читаем FIFO по мере наполнения. */
        cmdr_val = SDC_CMDR_START
                 | (cmd & SDC_CMDR_IDX_MASK)
                 | SDC_CMDR_RESP_RCV
                 | (check_crc ? SDC_CMDR_CHK_CRC : 0)
                 | SDC_CMDR_DATA_TRANS
                 | SDC_CMDR_WAIT_PRE;
        SDC_REG(SDC_CMDR) = cmdr_val;
        sdc_diag_cmdr = cmdr_val;
        __asm volatile("dmb sy" ::: "memory");
        for (i = 0; i < words; i++) {
            if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_STAR), SDC_STAR_FIFO_EMPTY, 0) != 0) {
                sdc_diag_risr = SDC_REG(SDC_RISR);
                sdc_diag_sta  = SDC_REG(SDC_STAR);
                return -1;
            }
            out[i] = fifo[0];
        }
        /* Хвост (total не кратен 4): данные приходят словом (4 байта) в FIFO —
           дожидаемся его и берём младшие rem байт. Для len<4 без ожидания
           FIFO ещё пуст в момент проверки, и хвост терялся (нули). */
        if (rem) {
            if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_STAR), SDC_STAR_FIFO_EMPTY, 0) != 0) {
                sdc_diag_risr = SDC_REG(SDC_RISR);
                sdc_diag_sta  = SDC_REG(SDC_STAR);
                return -1;
            }
            uint32_t w = fifo[0];
            uint8_t *ob = (uint8_t *)data;
            for (i = 0; i < rem; i++)
                ob[words * 4 + i] = (uint8_t)((w >> (8 * i)) & 0xFF);
        }
    }

    /* Ожидание завершения фазы данных. Для всех DAT-команд (CMD53 и пр.)
       завершение определяется по DATA_DONE (TRANS_COMPLETE), а НЕ по CMD_DONE:
       CMD_DONE взводится после цикла команда/ответа и не означает, что данные
       переданы на шину. Проверять "no-data" по биту 9 аргумента нельзя — это
       часть адресного поля bits[25:9] SDIO-команды; byte-mode CMD53 тоже
       участвует в фазе данных и завершается по DATA_DONE. */
    status = 0;
    for (i = 0; i < 0xffffff; i++) {
        status = SDC_REG(SDC_RISR);
        if (status & SDC_RISR_DATA_DONE)   /* фаза данных завершена HW */
            break;
        /* Только настоящие фатальные ошибки фазы данных:
           DATA_CRC, DATA_TIMEOUT, DATA_STAR_TO (HTO), DATA_ENDBIT, FIFO_UNDER.
           CMD_BUSY (bit12) после передачи безвреден, поэтому не входит. */
        if (status & (SDC_RISR_DATA_CRC | SDC_RISR_DATA_TO |
                      SDC_RISR_DATA_STAR_TO | SDC_RISR_DATA_END_BIT |
                      SDC_RISR_FIFO_UNDER))
            return -2;
    }
    if (i >= 0xffffff) {                    /* таймаут данных/команды */
        sdc_diag_risr = SDC_REG(SDC_RISR);
        sdc_diag_sta = SDC_REG(SDC_STAR);
        SDC_REG(SDC_RISR) = 0xFFFFFFFFu;
        return -3;
    }

    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;
    return 0;
}

/* Чтение CIS (Card Information Structure) чипа SDIO.
 *
 * 1) Читаем CCCR offset 0x09-0x0B (24-bit адрес Common CIS) по CMD52.
 *    Ожидаемый адрес для RTL8189FTV — 0x00001000.
 * 2) По этому адресу идём по CIS-туплам, читая каждый через CMD52 (байтовый
 *    доступ функции 0). Функция 0 (CCCR/CIS) НЕ поддерживает блоки CMD53 —
 *    поэтому тюплы читаются по одному байту. Ищем TPLID_MANFID (0x20) —
 *    за ним 16-бит Manufacturer Code (0x024C = Realtek).
 *
 * Формат CIS-тупла: [TPLDID][len][data...]. Конец — TPLID_END (0xFF).
 * Длина 0xFF означает, что реальная длина в следующих 2 байтах.
 *
 * Возвращает 0 при успехе и кладёт в *manf_id 16-бит код производителя;
 * не-0 — ошибка. В *cis_addr_out кладётся адрес CIS. */
/* Чтение ID чипа из CIS: Manufacturer ID + Card Info (PID).
 * Нужны для отладки: Realtek RTL8189FTV = manf 0x024C, card 0xF179.
 * Возвращает 0 при успехе (id0=manf, id1=card), -5 если MANFID-тупл не найден. */
int h3_sdio_read_chip_id(uint16_t *manf_id, uint16_t *card_id)
{
    int rc;
    uint8_t v;
    uint32_t addr;
    uint8_t b0, b1;

    /* Чтение байта из CIS по CMD52. */
    int rd8(uint32_t a, uint8_t *o) {
        return h3_sdc_cmd52_read(0, a & 0x1FFFF, o);
    }

    /* CCCR 0x09-0x0B: адрес Common CIS (LSB сперва). */
    if ((rc = rd8(0x09, &b0)) != 0) return rc;
    if ((rc = rd8(0x0A, &b1)) != 0) return rc;
    if ((rc = rd8(0x0B, &v))  != 0) return rc;
    uint32_t cis_addr = (uint32_t)v << 16 | (uint32_t)b1 << 8 | b0;
    if (cis_addr == 0) return -5;   /* CIS не задан */

    /* Идём по тюплам (лимит 64 тюпла / 512 байт для защиты). */
    uint16_t manf = 0, card = 0;
    int found = 0;
    addr = cis_addr;
    int guard;
    for (guard = 0; guard < 64 && !found; guard++) {
        uint8_t id, len;
        if ((rc = rd8(addr, &id)) != 0) return rc;
        addr++;
        if (id == 0xFF) break;          /* TPLID_END */
        if (id == 0x00) continue;       /* пустой тюпл (link says skip) */
        if ((rc = rd8(addr, &len)) != 0) return rc;
        addr++;
        if (len == 0xFF) {              /* 2-байтовая длина */
            uint8_t l0, l1;
            if ((rc = rd8(addr, &l0)) != 0) return rc;
            addr++;
            if ((rc = rd8(addr, &l1)) != 0) return rc;
            addr++;
            len = l1;
        }
        if (id == 0x20) {               /* TPLID_MANFID: 4 байта = manf(2) + card(2) */
            uint8_t m0, m1, c0, c1;
            if ((rc = rd8(addr,     &m0)) != 0) return rc;
            if ((rc = rd8(addr + 1, &m1)) != 0) return rc;
            if ((rc = rd8(addr + 2, &c0)) != 0) return rc;
            if ((rc = rd8(addr + 3, &c1)) != 0) return rc;
            manf = (uint16_t)(m1 << 8 | m0);
            card = (uint16_t)(c1 << 8 | c0);
            found = 1;
            break;
        }
        /* Пропустить данные тюпла. */
        addr += (uint32_t)len;
    }

    if (manf_id) *manf_id = manf;
    if (card_id) *card_id = card;
    return found ? 0 : -5;
}

/* Переключение шины SDIO: 1-bit (four_bit=0) или 4-bit (four_bit=1).
 *
 * 1) CCCR offset 0x07 (Card Bus Width [2:0]) по SDIO-спецификации:
 *    0b00 = 1-bit, 0b01 = 4-bit.
 * 2) Аппаратно: SDC_CTYPE (0x0C) = 1 — шина 4-bit.
 *
 * В рабочей цепочке вызывается из h3_wifi_download_fw перед загрузкой
 * прошивки: шина принудительно ставится в 1-bit (как проверено W.235-W.240 —
 * и pinctrl, и 4-bit ломают data-фазу CMD53 на 50 МГц). После загрузки FW
 * шина остаётся 1-bit на весь приём.
 * Возвращает 0 при успехе; не-0 — ошибка. */

int h3_sdio_set_bus_width(int four_bit)
{
    int rc;
    uint8_t v;

    if ((rc = h3_sdc_cmd52_read(0, 0x07, &v)) != 0) return rc;
    /* Сохранить прочие биты (высокоскоростной режим и пр.), обновить только
       поле ширины [2:0]: 0b00=1-bit, 0b01=4-bit. (W.248FIX: точная копия
       рабочей сетки HEAD/W.240 — без вызова sdc_prog_mmc_phase, который не
       участвовал в проверенной загрузке FW.) */
    uint8_t nv = (uint8_t)((v & ~0x07u) | (four_bit ? 0x01u : 0x00u));
    if ((rc = h3_sdc_cmd52_write(0, 0x07, nv)) != 0) return rc;

    /* Аппаратная ширина шины контроллера: 0=1-bit, 1=4-bit. */
    SDC_REG(SDC_CTYPE) = four_bit ? 0x1u : 0x0u;
    __asm volatile("dsb" ::: "memory");
    return 0;
}

/* ---- Передача probe request (механика 802.11) ----
 *
 *  - tx_desc 40 байт: rtl8188f_fill_default_txdesc (MGNT_FRAMETAG,
 *    rtl8188f_hal_init.c:4819-4903):
 *      PKT_SIZE [0:15], OFFSET=40 [16:23], BMC=1 [24] (broadcast),
 *      MACID=0 (pattrib->mac_id), QUEUE_SEL=QSLT_MGNT=0x12 (bits[8:12] dw1),
 *      USE_RATE=1 (dw3 bit8), TX_RATE=DESC_RATE1M=0x00 (dw4 [0:7]),
 *      RETRY_LIMIT_ENABLE=1 (dw4 bit17), DATA_RETRY_LIMIT=12 (dw4 bits[18:23]),
 *      MBSSID=0 (dw6 bits[12:15]), HWSEQ_EN=1 (dw9 бит, non-QoS mgmt).
 *  - кадр: 802.11 mgmt probe request (frame_ctl=0x0040, sub=0x04),
 *    addr1/addr3=FF:FF:FF:FF:FF:FF, addr2=наш MAC, seq=счётчик;
 *    SSID IE (пустой), Supported Rates IE, DS Set IE (канал).
*  - запись: sdio_write_port — CMD53 block write на
 *    addr = (WLAN_TX_HIQ_DEVICE_ID<<13)|((len/4)&0x1FFF), HIQ=4
 *    (WLAN_TX_HIQ_DEVICE_ID 4, см. hal_com_reg.h:1580).
 *
 *  Возвращает 0 при успехе. */
#define TXDESC_SIZE_W220    40u
#define WLAN_TX_HIQ_DEVICE_ID_W220 4u
#define WLAN_TX_LOQ_DEVICE_ID_W220 6u   /* LOW-очередь (BE/data) */

/* ===== JOINBSS: передача Auth/Assoc кадров =====
   Драйвер НЕ использует H2C 0x23 (это H2C_8188F_P2P_LPS_PARAM) — ассоциацию
   STA в 8189fs делает сам host: issue_auth (opensystem) -> issue_assocreq,
   ждёт Auth/AssocResp в RX (rtw_mlme_ext.c:8284 / 8669). Состояние целевой
   сети (join_target) и функции ниже. */

void h3_wifi_set_target(const char *ssid, uint8_t ssid_len)
{
    if (ssid_len > H3_WIFI_SSID_MAX) ssid_len = H3_WIFI_SSID_MAX;
    join_target.ssid_len = ssid_len;
    for (uint8_t i = 0; i < ssid_len; i++) join_target.ssid[i] = ssid[i];
    join_target.ssid[ssid_len] = 0;
    join_target.bssid_captured = 0;   /* сброс до следующего скана */
    join_target.have_msg1 = 0;        /* чистое 4-way состояние для нового подключения */
    join_target.secure = 0;           /* до прохождения handshake данные не шифровать */
}

int h3_wifi_target_is_set(void)
{
    return join_target.ssid_len > 0;
}

/* Захват BSSID/channel целевой сети из принятого beacon (вызывается из
   парсера RX-скана при совпадении SSID). */
void h3_wifi_scan_capture_target(const uint8_t *bssid, uint8_t channel)
{
    if (join_target.ssid_len == 0) return;
    for (int i = 0; i < 6; i++) join_target.bssid[i] = bssid[i];
    join_target.channel = channel;
    join_target.bssid_captured = 1;
}

int h3_wifi_target_captured(uint8_t bssid[6], uint8_t *channel)
{
    if (!join_target.bssid_captured) return -1;
    for (int i = 0; i < 6; i++) bssid[i] = join_target.bssid[i];
    if (channel) *channel = join_target.channel;
    return 0;
}

/* Установить RSN IE (WPA2) целевой сети — вызывается из UI-пути
   (wifi_layer_connect) после h3_wifi_scan_capture_target, т.к. там не
   проходит полного beacon-парсера, и RSN в join_target иначе останется 0.
   Копирует из структуры сети (из скана). W.374. */
void h3_wifi_set_target_rsn(const struct h3_wifi_ap *ap)
{
    join_target.rsn_len = ap->rsn_len;
    for (uint32_t i = 0; i < ap->rsn_len && i < 64; i++)
        join_target.rsn_ie[i] = ap->rsn_ie[i];
}

void h3_wifi_set_passphrase(const char *pass, uint8_t pass_len)
{
    if (pass_len > 63) pass_len = 63;
    join_target.pass_len = pass_len;
    for (uint8_t i = 0; i < pass_len; i++) join_target.passphrase[i] = pass[i];
    join_target.passphrase[pass_len] = 0;
    /* W.453: считать PMK ЗДЕСЬ (один раз, вне hot-path). PBKDF2(4096) ≈ 5.5с на
       этом SHA1-реализации; если считать на первый msg1 (кэш-мисс) — точка
       успевает уйти в retry. Здесь PMK вычисляется один раз при установке
       пароля/захвате цели (SSID уже в join_target.ssid). */
    if (join_target.pass_len && join_target.ssid_len) {
        wpa_pbkdf2_sha1((const uint8_t*)join_target.passphrase, join_target.pass_len,
                        (const uint8_t*)join_target.ssid, join_target.ssid_len,
                        4096, join_target.pmk, 32);
        join_target.pmk_valid = 1;
    } else {
        join_target.pmk_valid = 0;
    }
}

/* Внутренний общий отправитель управляющего кадра (tx_desc MGNT + кадр).
   Кадр собран в buf+40. Возвращает 0 при успехе. */
static int tx_mgnt_cmd(uint8_t *frame, uint32_t plen)
{
    uint8_t buf[40 + 200];
    uint32_t i;

    for (i = 0; i < 40; i++) buf[i] = 0;
    buf[2] = 40u;                                  /* OFFSET = 40 */
    buf[3] = (uint8_t)((1u << 0) | (1u << 2) | (1u << 3) | (1u << 7)); /* BMC|LAST|FIRST|OWN */
    buf[5] = 0x12u;                                /* QUEUE_SEL = QSLT_MGNT */
    buf[13] = (uint8_t)(1u << 0);                  /* USE_RATE */
    buf[16] = 0x00;                                /* TX_RATE 1M */
    buf[18] = (uint8_t)((1u << 1) | (12u << 2));   /* RETRY_LIMIT_EN + lim 12 */

    if (plen > 200) plen = 200;
    for (i = 0; i < plen; i++) buf[40 + i] = frame[i];

    buf[0] = (uint8_t)(plen & 0xFF);
    buf[1] = (uint8_t)((plen >> 8) & 0xFF);

    /* Дождаться свободной страницы HIQ. */
    int fp = -1;
    for (int t = 0; t < 100; t++) {
        fp = h3_wifi_tx_free_pages_hiq();
        if (fp >= 1) break;
        udelay(1000);
    }
    if (fp < 1) return -2;

    uint32_t total = (40 + plen + 3) & ~3u;
    uint32_t fifo_addr = (WLAN_TX_HIQ_DEVICE_ID_W220 << 13)
                       | ((total / 4) & 0x1FFFu);
    return h3_sdc_cmd53_write_bytes(1, fifo_addr, total, buf);
}

/* Прочитать наш MAC (REG_MACID 0x610-0x615), как в auth/assoc. Публичный,
   чтобы сетевой слой (wifi_net.c, DHCP) мог брать MAC для chaddr/frames. */
void h3_wifi_get_mac(uint8_t m[6])
{
    h3_sdc_read_wlan_reg8(0x610, &m[0]);
    h3_sdc_read_wlan_reg8(0x611, &m[1]);
    h3_sdc_read_wlan_reg8(0x612, &m[2]);
    h3_sdc_read_wlan_reg8(0x613, &m[3]);
    h3_sdc_read_wlan_reg8(0x614, &m[4]);
    h3_sdc_read_wlan_reg8(0x615, &m[5]);
}

/* Передача 802.11 data-фрейма (ToDS) для IP-трафика (DHCP/ARP/ICMP/EAPOL).
   payload = LLC/SNAP(8) + EtherType(2) + данные, length = размер payload без
   LLC-заглушки. frame построен: fc=0x0008 (type=data, ToDS), addr1=BSSID,
   addr2=наш MAC, addr3=BSSID. Отправка — в LOW-очередь (LOQ, device 6),
   которой пользуются data-кадры (mgmt идут в HIQ). Возвращает 0 при успехе.
   W.393. */
int h3_wifi_tx_data(uint16_t ethertype, const uint8_t *data, uint16_t len)
{
    int rc;

    if (!join_target.bssid_captured) return -3;

    uint8_t buf[40 + 24 + 8 + 2 + 1500];
    uint32_t i;
    for (i = 0; i < 40; i++) buf[i] = 0;

    /* tx_desc: как у драйвера для data (rtl8188f_fill_default_txdesc). */
    buf[2] = 40u;                                       /* OFFSET = 40 */
    /* W.492 [фикс по эталону rtw_xmit.c:1041-1051,1506-1512]: для STA (ToDS=1)
       addr1 (ra) = BSSID ВСЕГДА (unicast к AP, даже для broadcast-пакета),
       addr3 = L2 dst (для DHCP DISCOVER = ff:ff:ff:ff:ff:ff). Пакет к AP
       шифруется pairwise-ключом (cam4, MAC=BSSID), поэтому BMC=0. */
    int l2bcast = (ethertype == 0x0800);   /* DHCP DISCOVER: addr3 = broadcast */
    buf[3] = (uint8_t)((1u << 2) | (1u << 3) | (1u << 7));   /* LAST|FIRST|OWN */
    buf[5] = 0x00u;                                     /* QUEUE_SEL = QSLT_BE (data) */

    /* кадр с базы buf+40 */
    uint8_t *f = buf + 40;
    uint8_t m[6];
    h3_wifi_get_mac(m);

    f[0] = 0x08; f[1] = 0x01;      /* data, ToDS(bit1)=1 */
    /* W.462/W.465: после 4-way (secure=1) data-кадры ДОЛЖНЫ уходить зашифрованными.
       Два обязательных маркера, что чип реально применит TK/CCMP:
       1) Protected-бит (fc bit11=0x40),
       2) SET_TX_DESC_SEC_TYPE = AES (0x3) в TX-descriptor: биты[23:22]
          байта txdesc[4] (rtl8188f_hal_init.c:3258, SET_TX_DESC_SEC_TYPE_8188F).
       БЕЗ SEC_TYPE в txdesc чип отправляет кадр plaintext, даже если ключи
       в CAM и seccfg=0xCF. EAPOL msg4 НЕ защищается (уходит до установки
       ключей на AP), поэтому для ethertype 0x888E Protected и SEC_TYPE не
       ставятся. */
    if (join_target.secure && ethertype != 0x888E) {
        f[1] |= 0x40;              /* Protected (fc bit11) */
        /* SEC_TYPE = AES (0x3): SET_BITS_TO_LE_4BYTE(pDesc+4,22,2,val) —
           LE 32-бит слово на байтах txdesc[4..7]; бит22 = бит6 байта
           txdesc[6]; биты[23:22] = биты 7,6 байта txdesc[6]. 0x3 = 11. */
        buf[6] = (uint8_t)((buf[6] & 0x3Fu) | 0xC0u);   /* SEC_TYPE bits = 0b11 */
    }
    if (l2bcast) {
        for (i = 0; i < 6; i++) { f[4+i] = join_target.bssid[i]; f[10+i] = m[i]; f[16+i] = 0xFF; }
    } else {
        for (i = 0; i < 6; i++) { f[4+i] = join_target.bssid[i]; f[10+i] = m[i]; f[16+i] = join_target.bssid[i]; }
    }
    f[22] = 0; f[23] = 0;          /* seq */

    uint32_t p = 24;
    /* LLC/SNAP: AA AA 03 00 00 00 + ethertype (big-endian) */
    f[p++] = 0xAA; f[p++] = 0xAA; f[p++] = 0x03; f[p++] = 0x00; f[p++] = 0x00; f[p++] = 0x00;
    f[p++] = (uint8_t)(ethertype >> 8); f[p++] = (uint8_t)(ethertype & 0xFF);
    /* payload */
    for (i = 0; i < len; i++) f[p++] = data[i];

    /* длина кадра */
    buf[0] = (uint8_t)(p & 0xFF);
    buf[1] = (uint8_t)((p >> 8) & 0xFF);

    /* ждать свободной страницы LOW (data)-очереди */
    uint32_t total = (40u + p + 3u) & ~3u;
    int fp = -1;
    for (int t = 0; t < 100; t++) {
        fp = h3_wifi_tx_free_pages_loq();
        if (fp >= 1) break;
        udelay(1000);
    }
    if (fp < 1) return -2;

    uint32_t fifo_addr = (WLAN_TX_LOQ_DEVICE_ID_W220 << 13)
                       | ((total / 4) & 0x1FFFu);
    /* W.449 [W.450: спрятан] — tx dump печатался на каждую отправку (dhcp+msg2)
       и задерживал запись в HIQ. Оставлен в комментарии для справки:
       frame[24..31] подтверждал LLC/SNAP (AA AA 03 00 00 00) + ethertype. */
    rc = h3_sdc_cmd53_write_bytes(1, fifo_addr, total, buf);
    return rc;
}

/* Прочитать один накопленный пакет из RX FIFO (аналог drain-цикла скана):
   берёт RX0_REQ_LEN (0x1C), читает данные CMD53 по 1 блоку (512 б) с
   собственным seq, возвращает размер фактически прочитанного в *out_acc
   (0 = пакета не было). Переиспользуется для ожидания Auth/AssocResp
   в h3_wifi_join_ap. seq изолирован от скан-цикла (свой static). */
/* W.385: seq приёма RX FIFO для join (auth/assoc). Вынесен из static
   локального в file-static, чтобы можно было сбросить перед ожиданием
   AuthResp — после смены канала/скана host-seq должен быть согласован с
   чипом, иначе AuthResp читается из неверного окна (flaky auth rc=-4). */
static uint32_t rxp_seq = 0;

static int rx_read_packet(uint8_t *buf, uint32_t cap, uint32_t *out_acc)
{
    uint8_t rxl4[4] = {0, 0, 0, 0};
    uint32_t rxlen, total, off_read = 0;
    int frc = 0;

    *out_acc = 0;
    if (h3_sdc_cmd53_read_bytes(1, 0x001C, 4, rxl4) != 0)
        return -1;
    rxlen = (uint32_t)(rxl4[0] | ((uint16_t)rxl4[1] << 8));
    if (rxlen == 0)
        return 0;

    total = (rxlen > cap) ? cap : rxlen;
    if (total > 0x4000) total = 0x4000;
    uint32_t seq_cur = rxp_seq;
    /* Читаем по 1 блоку (512 б)/CMD53 — host H3 не тянет многоблок. */
    for (uint32_t g = 0; g < 64 && off_read < total; g++) {
        uint32_t addr = (7u << 13) | (seq_cur & 3u);
        frc = h3_sdc_cmd53_read(1, addr, 1, buf + off_read);
        if (frc != 0) break;
        off_read += 512u;
    }
    if (frc == 0 && off_read >= total) {
        uint8_t r2[4] = {0,0,0,0};
        if (h3_sdc_cmd53_read_bytes(1, 0x001C, 4, r2) == 0 && (r2[0] | r2[1]) == 0)
            rxp_seq = (seq_cur + 1) & 0xFFFFFFFFu;   /* пакет ушёл → seq++ */
    }
    *out_acc = (total < off_read) ? total : off_read;
    return (frc == 0) ? 0 : -2;
}

/* Authentication (opensystem) к целевому BSSID.
   Кадр: fc=0xB0, addr1/3=BSSID, addr2=наш MAC, alg=0, seq=1, status=0. */
int h3_wifi_tx_auth(void)
{
    if (!join_target.bssid_captured) return -3;

    uint8_t f[48];
    uint8_t m[6];
    /* MAC — из REG_MACID (как tx_probe). */
    h3_sdc_read_wlan_reg8(0x610, &m[0]);
    h3_sdc_read_wlan_reg8(0x611, &m[1]);
    h3_sdc_read_wlan_reg8(0x612, &m[2]);
    h3_sdc_read_wlan_reg8(0x613, &m[3]);
    h3_sdc_read_wlan_reg8(0x614, &m[4]);
    h3_sdc_read_wlan_reg8(0x615, &m[5]);

    /* W.380: обнуление буфера — поле duration [2],[3] иначе остаётся
       стековым мусором; строгий WPA2-AP молча дропает кадр с битым
       duration (A05s принимает, MTS_GPON молчит). */
    for (int i = 0; i < 48; i++) f[i] = 0;
    f[0] = 0xB0; f[1] = 0x00;              /* Auth */
    for (int i = 0; i < 6; i++) { f[4 + i] = join_target.bssid[i]; f[10 + i] = m[i]; f[16 + i] = join_target.bssid[i]; }
    f[22] = 0; f[23] = 0;                   /* seq (0 — не ведём) */
    f[24] = 0x00; f[25] = 0x00;             /* Auth algo: OPEN */
    f[26] = 0x01; f[27] = 0x00;             /* Auth seq = 1 */
    f[28] = 0x00; f[29] = 0x00;             /* Status = 0 */
    return tx_mgnt_cmd(f, 30);
}

/* W.413 [P1]: Deauthentication к BSSID (reason). Отправляется при смене сети и
   при WPA2-pending, чтобы AP и FW вышли из «associated/pending»-состояния
   (иначе следующий join к этому же BSSID молчит rc=-4). Uses bssid args. */
static int h3_wifi_tx_deauth(const uint8_t *bssid, uint16_t reason)
{
    uint8_t f[28];
    uint8_t m[6];
    for (int i = 0; i < 28; i++) f[i] = 0;
    h3_sdc_read_wlan_reg8(0x610, &m[0]);
    h3_sdc_read_wlan_reg8(0x611, &m[1]);
    h3_sdc_read_wlan_reg8(0x612, &m[2]);
    h3_sdc_read_wlan_reg8(0x613, &m[3]);
    h3_sdc_read_wlan_reg8(0x614, &m[4]);
    h3_sdc_read_wlan_reg8(0x615, &m[5]);
    f[0] = 0xC0; f[1] = 0x00;              /* Deauthentication (subtype 12) */
    for (int i = 0; i < 6; i++) { f[4+i] = bssid[i]; f[10+i] = m[i]; f[16+i] = bssid[i]; }
    f[22] = 0; f[23] = 0;                   /* seq */
    f[24] = (uint8_t)(reason & 0xFF);       /* reason, LE */
    f[25] = (uint8_t)((reason >> 8) & 0xFF);
    return tx_mgnt_cmd(f, 26);
}

/* W.453: построить и отправить EAPOL-Key msg2 (4-way, ответ на msg1).
   Вычисляет PMK (PBKDF2 pass+ssid), PTK (PRF-512), SNonce, MIC (HMAC-SHA1
   по KCK). Кадр уходит через h3_wifi_tx_data(0x888E) — data-путь ToDS.
   Возвращает 0 при успешной отправке. */
static int wpa_tx_msg2(void)
{
    if (join_target.pass_len == 0 || !join_target.have_msg1)
        return -1;

    /* 1) SNonce (детерминированный, seed = replay low + наш MAC). */
    wpa_fill_nonce(join_target.snonce, 0x12345678u ^
                   (join_target.replay[0] | (join_target.replay[1] << 8)));

    /* 2) PMK. Если предвычислен при установке passphrase (pmk_valid) — используем
       его БЕЗ повторного PBKDF2 (который на первый msg1 стоит 5.5с и точка уходит
       в retry). Иначе — один раз считаем здесь (не в hot-path повторно). */
    if (!join_target.pmk_valid && join_target.pass_len) {
        uint8_t pmk_local[32];
        wpa_pbkdf2_sha1((const uint8_t*)join_target.passphrase, join_target.pass_len,
                        (const uint8_t*)join_target.ssid, join_target.ssid_len,
                        4096, pmk_local, 32);
        for (int i = 0; i < 32; i++) join_target.pmk[i] = pmk_local[i];
        join_target.pmk_valid = 1;
    }

    /* W.430/W.435 [W.453: TV-блоки ПЕРЕМЕЩЕНЫ в h3_wifi_tx_msg2_error_... строго НЕ здесь].
       PMK/PTK TV выполняли два дополнительных PBKDF2(4096)+PRF-512 на КАЖДЫЙ msg1
       (≈150-300мс на Cortex-A7), из-за чего точка уходила в retry (replay 2→3→4),
       пока msg2 считался. Диагностика самопроверки больше не в hot-path. */

    /* 3) PTK = PRF-512(PMK, "Pairwise key expansion", min/nonce || max/mac_ap/sta)
       Данные PRF: min(AA,SPA)||max(AA,SPA)||min(ANonce,SNonce)||max(ANonce,SNonce).
       AA=BSSID, SPA=наш MAC. */
    {
        uint8_t aa[6], spa[6];
        h3_wifi_get_mac(spa);
        for (int i = 0; i < 6; i++) aa[i] = join_target.bssid[i];
        /* (AA,SPA) — сортировка лексикографически для min/max. */
        const uint8_t *lo_mac = spa, *hi_mac = aa;
        int cmp = 0;
        for (int i = 0; i < 6; i++) { if (aa[i] != spa[i]) { cmp = (aa[i] < spa[i]) ? -1 : 1; break; } }
        if (cmp < 0) { lo_mac = aa; hi_mac = spa; }
        /* (ANonce,SNonce) min/max — по 802.11i в PRF nonce идут min||max (лексикогр.).
       W.457 фикс: было if(cn<0) — инверсия (при anonce<snonce клал snonce в lo/
       больший в lo). Точное воспроизведение board-KCK и проверка hostapd-формой:
       для A05s anonce 0x5D.. > snonce 0x47.., правильный порядок = snonce||anonce.
       Плата слала anonce||snonce → AP при расчёте PTK делал min||max = snonce||anonce
       → KCK/MIC расходились → msg2 отвергался. */
        const uint8_t *lo_n = join_target.anonce, *hi_n = join_target.snonce;
        int cn = 0;
        for (int i = 0; i < 32; i++) {
            if (join_target.anonce[i] != join_target.snonce[i]) { cn = (join_target.anonce[i] < join_target.snonce[i]) ? -1 : 1; break; }
        }
        if (cn > 0) { lo_n = join_target.snonce; hi_n = join_target.anonce; }
        uint8_t prf_data[76];
        for (int i = 0; i < 6; i++) { prf_data[i] = lo_mac[i]; prf_data[6+i] = hi_mac[i]; }
        for (int i = 0; i < 32; i++) { prf_data[12+i] = lo_n[i]; prf_data[44+i] = hi_n[i]; }
        wpa_prf(join_target.pmk, 32, "Pairwise key expansion", prf_data, 76,
                join_target.ptk, 48*8);   /* KCK|KEK|TK = 48 байт */
    }

    /* 4) Собрать EAPOL-Key msg2: SNAP+ethertype + EAPOL header + EAPOL-Key body.
       body (RFC 4017/802.11i): descriptor_type=0x02(RSN),
       key_info=0x010A (ver2 0x0002 | KEY_TYPE 0x0008 | MIC 0x0100),
       key_len=0 (в msg2 длина ключа не указывается), replay counter(8) —
       копируется из msg1, nonce snonce(32), key_iv(16)=0, key_rsc(8)=0,
       key_id(8)=0, key_mic(16)=0 (посчитаем), key_data_len=0. */
    uint8_t body[95];
    for (int i = 0; i < 95; i++) body[i] = 0;
body[0]  = 0x02;                             /* RSN */
    body[1]  = 0x01; body[2] = 0x0A;             /* key_info msg2 = 0x010A:
                                                    version2(0x0002) | pairwise(0x0008) |
                                                    keymic(0x0100). БЕЗ ACK(0x0080) и
                                                    БЕЗ INSTALL(0x0040). */
    body[3]  = 0x00; body[4] = 0x10;             /* key_len = 16 (CCMP pairwise, как в msg1) */
    for (int i = 0; i < 8; i++) body[5+i] = join_target.replay[i];   /* replay из msg1 */
    for (int i = 0; i < 32; i++) body[13+i] = join_target.snonce[i]; /* nonce */

    /* key_data (после 95-байт тела): RSN IE (как в beacon/assoc), если есть —
       wpa_supplicant/hostapd включает RSN IE в key_data msg2, когда известен
       RSNE AP. Эталон (Harkonen): RSN IE в key_data msg2. */
    uint16_t kd_len = 0;
    uint8_t key_data[64];
    if (join_target.rsn_len > 0 && join_target.rsn_len <= 64) {
        uint32_t kl = 0;
        for (uint32_t i = 0; i < join_target.rsn_len && kl < 64; i++)
            key_data[kl++] = join_target.rsn_ie[i];
        kd_len = (uint16_t)kl;
    }
    body[93] = (uint8_t)(kd_len >> 8); body[94] = (uint8_t)(kd_len & 0xFF);

    /* EAPOL-Key кадр = 4 байта EAPOL header (ver2|type3|len) + тело(95) + key_data.
       длина тела в EAPOL hdr = 95 + kd_len. MIC покрывает ВЕСЬ EAPOL-кадр
       (header + тело + key_data) с key_mic обнулённым.
       Поле key_mic в теле на смещении 77..92 (95-байт тело). */
    uint8_t pkt[4 + 95 + 64];
    pkt[0] = 1;   /* W.446: EAPOL version FIX = 1 (эталонные msg2 Harkonen/praneethwifi = 1;
                   msg1 от A05s шлёт 2, но AP-валидатор ждёт 1 в msg2) */
    pkt[1] = 3; pkt[2] = 0; pkt[3] = (uint8_t)(95 + kd_len); /* EAPOL hdr: type, len */
    for (int i = 0; i < 95; i++) pkt[4+i] = body[i];   /* key_mic в теле = 0 (body[77..92]) */
    for (int i = 0; i < kd_len; i++) pkt[4+95+i] = key_data[i];
    uint32_t eapol_len = 4 + 95 + (uint32_t)kd_len;

    /* MIC = HMAC-SHA1(KCK=ptk[0..16)) по всему EAPOL кадру с key_mic=0.
       Результат (16 байт) кладём в key_mic тела на 77..92. */
    {
        uint8_t mic[20];
        wpa_hmac_sha1(join_target.ptk, 16, pkt, eapol_len, mic);
        for (int i = 0; i < 16; i++) pkt[4+77+i] = mic[i];
    }

    /* W.488: убран полный msg2 dump (ver/len/kinfo/keylen/replay/mic/kdlen) —
       в горячем пути msg1→msg2 он забивал UART-буфер и давал блокирующий flush. */
    /* W.452: убраны тяжёлые дампы (kck[16]/snonce[32]/ssid/pmk) — выполняли
       диагностическую миссию в hot-path msg1→msg2 и забивали UART-буфер (4096Б),
       вызывая блокирующий flush (≈22мс на 256Б@115200) ПЕРЕД записью msg2 в HIQ;
       из-за этого точка успевала уйти в retry (replay 2→3→4). Оставлен короткий
       полевой дамп (ver/kinfo/keylen/replay/mic/kdlen). */

    /* W.475: пауза 5 мс перед msg2, чтобы AP успела переключиться из приёма
       на обработку. W.488: убраны 100-мс пауза и диагностика loq/rpt после
       отправки (тормозили 4-way и срывали тайминги msg1-retry). */
    udelay(5000);
    return h3_wifi_tx_data(0x888E, pkt, eapol_len);
}

/* Регистры/биты CAM шифрования 8188F (hal_com_reg.h). */
#ifndef REG_CAMCMD
#define REG_CAMCMD     0x0670
#define REG_CAMWRITE   0x0674
#define REG_CAMREAD    0x0678
#define CAM_VALID      (1u << 15)
#define CAM_CONTENT_COUNT 8
#define CAM_AES        0x04
#define CAM_WRITE      (1u << 16)
#define CAM_READ       (1u << 17)
#define CAM_POLLINIG   (1u << 31)
#endif

/* W.442: записать ключ (PTK/TK или GTK) в CAM чипа (AES-CCMP, драйвер
   rtw_sec_write_cam). Протокол 8188F:
     REG_CAMCMD  = 0x0670, REG_CAMWRITE = 0x0674
     CAM_POLLINIG = BIT31, CAM_WRITE = BIT16, CAM_VALID = BIT15
     CAM_AES = 0x04, CAM_CONTENT_COUNT = 8
   addr — номер записи CAM (pairwise: номер по MAC, group: 0/1..3).
   mac[6] — адрес (pairwise — BSSID AP; для GTK-default — ВСЕ НУЛИ, см.
     драйвер rtw_mlme_ext.c:14186-14189: cam_id<=3 -> addr=null_addr).
   key[16] — 128-бит ключ (CCMP).
   is_group — 1 для GTK (default/group key): добавить BIT6 (флаг group key)
     в слово управления, как у драйвера ctrl = BIT15|BIT6|alg<<2|keyid
     (rtw_mlme_ext.c:14214). Без BIT6 и null-addr чип не находит групповой
     ключ для расшифровки broadcast-кадров (DHCP OFFER на ff:ff:ff:ff:ff:ff). */
static void wpa_cam_write_key(uint8_t addr, const uint8_t *mac,
                              const uint8_t *key, int is_group)
{
    uint32_t ulContent;
    for (int i = 0; i < (int)CAM_CONTENT_COUNT; i++) {
        ulContent = 0;
        if (i == 0) {
            ulContent = (addr & 0x03) | ((uint32_t)CAM_AES << 2) | CAM_VALID;
            if (is_group) ulContent |= (1u << 6);   /* BIT6 = group/default key */
            ulContent |= (uint32_t)mac[0] << 16;
            ulContent |= (uint32_t)mac[1] << 24;
        } else if (i == 1) {
            ulContent = (uint32_t)mac[2]
                      | ((uint32_t)mac[3] << 8)
                      | ((uint32_t)mac[4] << 16)
                      | ((uint32_t)mac[5] << 24);
        } else {
            /* слово 2..7: ключ (16 байт), по 4 байта LE на слово. */
            uint32_t koff = (uint32_t)(i - 2) * 4;
            if (koff < 16) {
                ulContent = (uint32_t)key[koff]
                          | ((uint32_t)key[koff+1] << 8)
                          | ((uint32_t)key[koff+2] << 16)
                          | ((uint32_t)key[koff+3] << 24);
            }
        }
        h3_sdc_write_wlan_reg32(0x0674, ulContent);        /* WCAMI */
        h3_sdc_write_wlan_reg32(0x0670, CAM_POLLINIG | CAM_WRITE |
                                ((uint32_t)CAM_CONTENT_COUNT * addr + (uint32_t)i));
        /* wait polling bit clears. W.484: 1000x10мс = до 10 с на слово при
           залипании POLLINIG, что рвало тайминги 4-way. 100x1мс достаточно. */
        for (int p = 0; p < 100; p++) {
            uint32_t rd = 0;
            h3_sdc_read_wlan_reg32(0x0670, &rd);
            if (!(rd & CAM_POLLINIG)) break;
            udelay(1);
        }
    }
}

/* Association Request к целевому BSSID (SSID + rates + capability). */
int h3_wifi_tx_assoc_req(void)
{
    if (!join_target.bssid_captured) return -3;

    uint8_t f[120];
    uint8_t m[6];
    /* W.380: обнуление — duration [2],[3] иначе стековый мусор, строгий
       WPA2-AP молча дропает такой кадр. (Аналог fix в h3_wifi_tx_auth.) */
    for (int i = 0; i < 120; i++) f[i] = 0;
    h3_sdc_read_wlan_reg8(0x610, &m[0]);
    h3_sdc_read_wlan_reg8(0x611, &m[1]);
    h3_sdc_read_wlan_reg8(0x612, &m[2]);
    h3_sdc_read_wlan_reg8(0x613, &m[3]);
    h3_sdc_read_wlan_reg8(0x614, &m[4]);
    h3_sdc_read_wlan_reg8(0x615, &m[5]);

    f[0] = 0x00; f[1] = 0x00;              /* AssocReq */
    for (int i = 0; i < 6; i++) { f[4 + i] = join_target.bssid[i]; f[10 + i] = m[i]; f[16 + i] = join_target.bssid[i]; }
    f[22] = 0; f[23] = 0;
    uint32_t p = 24;
    /* capability: ESS(0x0001) всегда + Privacy(0x0010) если сеть WPA2 (есть RSN IE).
       Без Privacy-bit WPA2-AP (MTS_GPON_9290e0) отвечает AssocResp status!=0 -> rc=-6. */
    uint16_t cap = 0x0001;
    if (join_target.rsn_len > 0) cap |= 0x0010;   /* Privacy */
    f[p++] = (uint8_t)(cap & 0xFF); f[p++] = (uint8_t)(cap >> 8);
    f[p++] = 0x03; f[p++] = 0x00;          /* listen interval = 3 */
    /* SSID IE */
    f[p++] = 0x00; f[p++] = join_target.ssid_len;
    for (uint32_t i = 0; i < join_target.ssid_len; i++) f[p++] = (uint8_t)join_target.ssid[i];
    /* Supported Rates (CCK+OFDM basic) */
    f[p++] = 0x01; f[p++] = 0x08;
    f[p++] = 0x82; f[p++] = 0x84; f[p++] = 0x8B; f[p++] = 0x96; /* 1/2/5.5/11 basic */
    f[p++] = 0x0C; f[p++] = 0x12; f[p++] = 0x18; f[p++] = 0x24; /* 6/9/12/18 */
    /* RSN IE (WPA2) — копируем побайтово из beacon'а цели (W.373). */
    for (uint32_t i = 0; i < join_target.rsn_len && p < 120; i++)
        f[p++] = join_target.rsn_ie[i];
    /* W.379: полный hex-дамп готового AssocReq-фрейма (без tx_desc), чтобы
       увидеть фактические байты, которые уходят в эфир — RSN-дамп не показал
       повреждения, но AP молчит именно с RSN IE (W.378). p = длина кадра. */
    /* W.467: тяжёлый hex-дамп assoc-frm (до 96 слов) УБРАН — печатался в hot-path
       перед Auth/Assoc и задерживал handshake (AP успевал ретраить), см. W.452. */
    return tx_mgnt_cmd(f, p);
}

/* Полная host-ассоциация: Auth opensystem, пауза, затем AssocReq.
   (Ожидание ответов и обработка AssocResp — следующий этап.) */
int h3_wifi_join_ap(void)
{
    int skip_auth = 0;   /* W.478: 1 = already-assoc (re-DHCP), не пере-authenticate */
    if (!join_target.bssid_captured) return -3;

    /* W.402 [P1]: повторный join. Если AP уже ассоциирован с тем же BSSID и
       MSR=STA — новый Auth к уже-associated BSSID от того же MAC игнорируется
       (A05s отвечал rc=-4 на sel=2 после успешного kmain join). Выходим сразу
       rc=0, не трогая связку. Иначе, если REG_BSSID (0x618) != target —
       старый BSSID/SSID/MSR сбрасываем в NOLINK, чтобы новый Auth ушёл как
       «новый клиент» (иначе AP считает нас уже associated и молчит). */
    {
        uint8_t cur[6] = {0,0,0,0,0,0}, msr = 0;
        int same_bssid = 1;
        for (int k = 0; k < 6; k++) {
            h3_sdc_read_wlan_reg8(0x0618 + k, &cur[k]);
            if (cur[k] != join_target.bssid[k]) same_bssid = 0;
        }
        h3_sdc_read_wlan_reg8(0x0102, &msr);
        /* W.402/478: уже ассоциирован к целевому BSSID и MSR=STA — не пере-authenticate
           (это рвёт связку). Вместо return 0 флажим skip_auth: пройдём БЕЗ Auth/
           Assoc сразу в DHCP-цикл ниже (повторный DISCOVER/ожидание OFFER).
           Если 4-way уже был (secure=1) — DISCOVER уйдёт зашифрованным и OFFER
           расшифруется; если ещё нет — EAPOL-Start внизу запустит msg1. */
        if (same_bssid && (msr & 0x01) && join_target.secure) {
            uart_print("  join: already assoc+secure to same BSSID, skip auth->reDHCP\r\n");
            skip_auth = 1;
        }
        /* W.472 [фикс]: deauth ПЕРЕД любым auth (даже тот же BSSID). Если
           предыдущий AssocResp был потерян (rc=-6), AP считает нас associated
           (msr=1 на AP), а наш msr=0 -> рассинхрон, auth молчит (rc=-4).
           Deauth не-associated стана AP игнорирует (no-op) — вреда нет,
           зато сбрасывает подвисшее состояние на AP. reason 3 = Leaving ESS. */
        /* W.485: при skip_auth (уже assoc к этому BSSID) НЕ рвать связку.
           Прежний код слал deauth + сбрасывал MSR/BSSID даже в этом случае,
           затем всё равно проходил auth/assoc (флаг влиял только на возврат
           -4/-6) → каждый повторный клик разрушал рабочий линк и начинал
           4-way с нуля, в итоге падая в WPA2 pending. */
        if (!skip_auth && (msr & 0x01)) {
            h3_wifi_tx_deauth(join_target.bssid, 3);   /* к ЦЕЛЕВОМУ BSSID */
            udelay(20000);
            uart_print("  join: deauth (pre-auth)\r\n");
        }
        /* Всегда сбрасываем link в NOLINK перед новым join, но только если
           действительно делаем auth (иначе теряем готовую связку). */
        if (!skip_auth) {
            h3_sdc_write_wlan_reg8(0x0102, 0x00);        /* MSR NOLINK */
            for (int k = 0; k < 6; k++) h3_sdc_write_wlan_reg8(0x0618 + k, 0x00);
            for (int k = 0; k < 32; k++) h3_sdc_write_wlan_reg8(0x06E0 + k, 0x00);
            udelay(5000);
            uart_print("  join: reset link (NOLINK, bssid=0)\r\n");
        }
    }

    /* W.391 (join_prepare): сбросить накопленное состояние между join.
       Без этого 2-й и последующие join наследуют от первого (data-rx остался
       в RCR/RXFLTMAP, rxp_seq накоплен). Drain RX FIFO выполняется по 1 блоку
       через rx_read_packet (см. шаг 4) — прежний flush через CMD53 blks>1/
       dummy[4096] загонял SDIO в bad state (W.390), поэтому только по 1 блоку. */
    {
        /* 1) restore scan-mode RCR/RXFLTMAP (на случай data-rx наследия). */
        h3_sdc_write_wlan_reg32(0x0608, 0x7000604Eu);
        h3_sdc_write_wlan_reg16(0x06A0, 0xFFFF);
        h3_sdc_write_wlan_reg16(0x06A2, 0x0400);
        h3_sdc_write_wlan_reg16(0x06A4, 0x0000);
        /* 2) разморозить RX DMA: если RW_RELEASE_EN (RXPKTBUF 0x284[18]) остался
           взведён после data-rx, FW не выкладывает новые пакеты в SDIO FIFO
           (auth response приходит по RF, но зависает в FW) — причина rc=-4 на
           повторных join. Аналог сброса в h3_sdc.c:2257 (init). */
        {
            uint32_t rxpk = 0;
            if (h3_sdc_read_wlan_reg32(0x0284, &rxpk) == 0) {
                int was18 = (rxpk >> 18) & 1;
                rxpk &= ~(1u << 18);                       /* RW_RELEASE_EN=0 */
                h3_sdc_write_wlan_reg32(0x0284, rxpk);
                /* 3) clear HISR (SDIO-local): гасим все pending RX-прерывания. */
                h3_sdc_cmd52_write(1, 0x0018, 0xFF);
                udelay(1000);
                uart_print("  join: prepare rxpk b18=");
                uart_print_int(was18);
                uart_print(" -> reset\r\n");
            }
        }
        /* 4) сбросить seq чтения RX FIFO для join. */
        rxp_seq = 0;
        /* W.407: безопасный drain RX FIFO (по 1 блоку через rx_read_packet —
           ВАЖНО не blks>1/dummy[4096], они загоняли SDIO в bad state, W.390).
           После неудачного join (напр. assoc timeout MTS_GPON rc=-6) в FIFO
           остаются накопленные beacon/Action-кадры; следующий join читает их
           как AuthResp/AssocResp из неверного окна → плавающий rc=-4/-6.
           Выгребаем до конца (rxlen=0), не печатая содержимое. */
        {
            uint8_t flush_buf[0x4000 + 512];
            int drained = 0;
            for (int it = 0; it < 32; it++) {
                uint32_t fa = 0;
                if (rx_read_packet(flush_buf, sizeof(flush_buf), &fa) != 0) break;
                if (fa == 0) break;
                drained++;
            }
            if (drained > 0) {
                uart_print("  join: prepare flush FIFO drained=");
                uart_print_int(drained);
                uart_print("\r\n");
            }
        }
        uart_print("  join: prepare (scan-mode on, rxp_seq=0, rx-dma unfrozen)\r\n");
    }

    /* Перед ассоциацией переключить RF на канал целевой сети. Свип оставил
       RF на канале 13 (последний), а Auth/Assoc идут к BSSID на его канале —
       иначе кадры уходят в пустоту (rc=-4 из-за этого). */
    rf_write(0x18, 0xC00u | (uint32_t)(join_target.channel & 0xFF));
    udelay(50000);   /* 50 мс на перестройку синтезатора (как в свипе) */
    uart_print("  join: rf18=0x");
    uart_print_hex(rf_read(0x18));
    uart_print(" (want 0x");
    uart_print_hex(0xC00u | (uint32_t)(join_target.channel & 0xFF));
    uart_print(")\r\n");

    /* Шаг 1: Auth (opensystem, seq=1). W.485: при skip_auth (уже assoc)
       кадр НЕ шлём — линк не трогаем, сразу ждём DHCP/EAPOL ниже. */
    int rc = 0;
    if (!skip_auth) {
        rc = h3_wifi_tx_auth();
        uart_print("  join: auth tx rc=");
        uart_print_int(rc);
        uart_print("\r\n");
        if (rc != 0) return rc;
    }

    /* Шаг 2: ждать AuthResp (fc 0xB0, seq=2, status=0) от целевого BSSID. */
    {
        uint8_t rxbuf[0x4000 + 512];
        uint32_t acc = 0;
        int tries;
        int got_auth = 0;
        /* W.385: сброс seq чтения RX FIFO — после смены канала/скана host-seq
           должен стартовать с начала, иначе AuthResp читается из неверного окна
           (flaky auth rc=-4, auth-запрос уходит но ответ не распознаётся). */
        /* W.386: таймаут AuthResp ~80 мс был слишком мал — AP отвечает за
           сотни мс, поэтому rc=-4 флейкался (auth tx rc=0, но ответ не успевал).
           Ждём до ~1000 мс, как ассо. */
        rxp_seq = 0;
        {
            int waited_auth = 0;
            for (tries = 0; !skip_auth && tries < 500 && waited_auth < 1000; tries++) {
                if (rx_read_packet(rxbuf, sizeof(rxbuf), &acc) != 0) { udelay(2000); waited_auth += 2; continue; }
                if (acc == 0) { udelay(2000); waited_auth += 2; continue; }
            uint32_t off = 0;
            while (off + 26 <= acc) {
                uint32_t plen = (uint32_t)(rxbuf[off] | ((rxbuf[off+1] & 0x3F) << 8));
                if (plen == 0 || plen > 0x2000) break;
                uint32_t drvsz = (uint32_t)(rxbuf[off+2] & 0x0Fu) << 3;
                uint32_t fr = off + 24u + drvsz;
                uint16_t fcfc2 = (uint16_t)(rxbuf[fr] | (rxbuf[fr+1] << 8));
                (void)fcfc2;
                /* Диагностика (WLOG): любой management кадр в буфере при ожидании
                   AuthResp — тип + отправитель + status. Спам beacon'ов (fc=0x80)
                   маскировал auth/assoc, поэтому скрыто за WIFI_VERBOSE_LOG (W.367);
                   раскомментируй #define WIFI_VERBOSE_LOG 1 в h3_sdc.h — вернётся
                   полный вывод. */
/* W.392: диагностика всего RX при ожидании AuthResp. Это дамп КАЖДОГО
                   mgmt-кадра (в т.ч. непрерывных beacon fc=0x80 от всех AP) —
                   заливает UART и мешает видеть authresp. Скрыто за
                   WIFI_VERBOSE_LOG (W.402): при отладке auth-таймаута вернётся
                   раскомментированием #define WIFI_VERBOSE_LOG 1 в h3_sdc.h. */
                WLOG("  join: rx mgmt fc=0x");
                WLOG_HEX((uint32_t)fcfc2);
                WLOG(" sub=");
                WLOG_HEX((uint32_t)((fcfc2 >> 4) & 0xF));
                if (fr + 16 <= acc) {
                    WLOG(" dst=");
                    for (int k = 0; k < 6; k++) { WLOG_HEX((uint32_t)rxbuf[fr+4+k]); if (k<5) WLOG(":"); }
                }
                WLOG(" src=");
                for (int k = 0; k < 6; k++) { WLOG_HEX((uint32_t)rxbuf[fr+10+k]); if (k<5) WLOG(":"); }
                WLOG("\r\n");
/* AuthResp: subtype=0xB, лежит в старшем полубайте fc[0] (биты 4-7),
                   т.е. fc[0]=0xB0. Младший полубайт (Protocol Version) всегда 0 —
                   маскировать по &0x0F НЕЛЬЗЯ (не совпадёт ни один mgmt-кадр).
                   Высокий байт fc[1] (Retry/Pwr/Protected) НЕ проверяем: AP может
                   прислать кадр повторно с Retry-bit=1 (fc[1]=0x08), тогда строгая
                   проверка ==0x00 отбрасывала валидный AuthResp -> ложный rc=-4. */
                if (fr + 30 <= acc && (rxbuf[fr] & 0xF0) == 0xB0) {
                    int same = 1;
                    for (int k = 0; k < 6; k++)
                        if (rxbuf[fr+10+k] != join_target.bssid[k]) { same = 0; break; }
                    if (same) {
                        uint16_t status = (uint16_t)(rxbuf[fr+28] | (rxbuf[fr+29] << 8));
                        uart_print("  join: authresp status=0x");
                        uart_print_hex((uint32_t)status);
                        uart_print("\r\n");
                        if (status == 0) { got_auth = 1; break; }
                    }
                }
                off += 24u + drvsz + plen;
                off = (off + 7u) & ~7u;
            }
            if (got_auth) break;
            udelay(2000);
                waited_auth += 2;
            }
        }
        if (!skip_auth && !got_auth) return -4;   /* AuthResp не получена/отказ */
        if (skip_auth)
            uart_print("  join: skip auth (re-DHCP)\r\n");
        else
            uart_print("  join: auth resp rcvd (rx pkt fc=0xB0, BSSID match)\r\n");
    }

    /* Шаг 3: на тот же целевой BSSID — AssocReq. W.485: при skip_auth не шлём. */
    if (!skip_auth) {
        rc = h3_wifi_tx_assoc_req();
        uart_print("  join: assocreq tx rc=");
        uart_print_int(rc);
        uart_print(" rsn_len=");
        uart_print_int((int)join_target.rsn_len);
        uart_print(" cap=");
        /* Фактический cap, согласованный с h3_wifi_tx_assoc_req (W.376): не хардкод! */
        uart_print_hex((uint32_t)((join_target.rsn_len > 0) ? 0x0011 : 0x0001));
        uart_print("\r\n");
        if (rc != 0) return rc;
    }

    /* Шаг 4: ждать AssocResp (fc 0x10, status на +26 после 3addr + cap2) от BSSID. */
    {
        uint8_t rxbuf[0x4000 + 512];
        uint32_t acc = 0;
        int tries;
        int got_assoc = 0;
        int waited = 0;
        /* W.377: ассос к WPA2-AP отвечает до сотен мс. Ждём до ~1200 мс. */
        for (tries = 0; !skip_auth && tries < 600 && waited < 1200; tries++) {
            if (rx_read_packet(rxbuf, sizeof(rxbuf), &acc) != 0) { udelay(2000); waited += 2; continue; }
            if (acc == 0) { udelay(2000); waited += 2; continue; }
            uint32_t off = 0;
            while (off + 26 <= acc) {
                uint32_t plen = (uint32_t)(rxbuf[off] | ((rxbuf[off+1] & 0x3F) << 8));
                if (plen == 0 || plen > 0x2000) break;
                uint32_t drvsz = (uint32_t)(rxbuf[off+2] & 0x0Fu) << 3;
                uint32_t fr = off + 24u + drvsz;
                /* W.381: диагностика Action-фреймов (fc=0xD0) от целевого BSSID
                   при ожидании AssocResp убрана за WIFI_VERBOSE_LOG — спам fрrecеймов
                   заваливал UART (Action-фреймы идут от многих AP). Видимым остался
                   только assocresp status ниже. */
#ifdef WIFI_VERBOSE_LOG
                if (fr + 2 <= acc && (rxbuf[fr+10] == join_target.bssid[0]) &&
                    rxbuf[fr+11] == join_target.bssid[1] &&
                    rxbuf[fr+12] == join_target.bssid[2] &&
                    rxbuf[fr+13] == join_target.bssid[3] &&
                    rxbuf[fr+14] == join_target.bssid[4] &&
                    rxbuf[fr+15] == join_target.bssid[5]) {
                    uint16_t mfc = (uint16_t)(rxbuf[fr] | (rxbuf[fr+1] << 8));
                    uart_print("  join: waitassoc mgmt fc=0x");
                    uart_print_hex((uint32_t)mfc);
                    uart_print(" sub=");
                    uart_print_hex((uint32_t)((mfc >> 4) & 0xF));
                    uart_print("\r\n");
                }
#endif
                /* AssocResp: subtype=0x1, старший полубайт fc[0] (fc[0]=0x10), addr2=AP,
                   status на +26. Высокий байт fc игнорируем (Retry/Pwr/Protected),
                   см. AuthResp выше. */
                if (fr + 30 <= acc && (rxbuf[fr] & 0xF0) == 0x10) {
                    int same = 1;
                    for (int k = 0; k < 6; k++)
                        if (rxbuf[fr+10+k] != join_target.bssid[k]) { same = 0; break; }
                    if (same) {
                        uint16_t status = (uint16_t)(rxbuf[fr+26] | (rxbuf[fr+27] << 8));
                        uart_print("  join: assocresp status=0x");
                        uart_print_hex((uint32_t)status);
                        uart_print("\r\n");
                        if (status == 0) { got_assoc = 1; break; }
                    }
                }
                off += 24u + drvsz + plen;
                off = (off + 7u) & ~7u;
            }
            if (got_assoc) break;
            udelay(2000);
            waited += 2;
        }
        if (!skip_auth && !got_assoc) return -6;   /* AssocResp не получена/отказ */
        if (!skip_auth) uart_print("  join: assoc resp OK (associated)\r\n");
    }

    /* Шаг 5: перевести в STA-режим и записать BSSID AP (ассоциация состоялась). */
    {
        /* MSR (0x0102) = 0x01 (STA mode) — до этого был NETTYPE_AP (0x02). */
        h3_sdc_write_wlan_reg8(0x0102, 0x01);
        /* REG_BSSID (0x618..0x61D) = BSSID выбранного AP (для связанного кадра). */
        for (int k = 0; k < 6; k++)
            h3_sdc_write_wlan_reg8(0x0618 + k, join_target.bssid[k]);
        /* Readback-подтверждение. */
        uint8_t rb[6], msr;
        for (int k = 0; k < 6; k++) h3_sdc_read_wlan_reg8(0x0618 + k, &rb[k]);
        h3_sdc_read_wlan_reg8(0x0102, &msr);
        uart_print("  join: msr=0x");
        uart_print_hex((uint32_t)msr);
        uart_print(" bssid_re=");
        int ok = (msr == 0x01);
        for (int k = 0; k < 6; k++) {
            uart_print_hex((uint32_t)rb[k]);
            if (k < 5) uart_print(":");
            if (rb[k] != join_target.bssid[k]) ok = 0;
        }
        uart_print(ok ? " OK\r\n" : " MISMATCH\r\n");
        if (!ok) return -7;
    }

    /* W.383: ассоциация состоялась — перевести приём из site-survey в
       connected: принимать data-кадры (RXFLTMAP2=0xFFFF, эталон rtl8188f
       rtl8188f_hal_init.c:5580). Без этого data (ARP/ICMP/DHCP) до нас не дойдут. */
    {
        h3_sdc_write_wlan_reg16(0x06A4, 0xFFFF);   /* RXFLTMAP2: data on */
        /* RCR: вернуть CBSSID_DATA (bit6) чтобы фильтроваться по BSSID и
           при BSSID-filter на пакеты данных от нашего AP. */
        uint32_t rcr = 0;
        h3_sdc_read_wlan_reg32(0x0608, &rcr);
        h3_sdc_write_wlan_reg32(0x0608, rcr | (1u << 6));   /* RCR_CBSSID_DATA */
        uart_print("  join: data-rx ON (rxfltmap2=ffff, rcr cbssid_data)\r\n");
    }

    /* W.395: data-приём после ассоциации — отправляем DHCP DISCOVER и ждём
       OFFER (получаем сеть -> IP x.x.x.199). Парсинг OFFER в net_handle_ipv4. */
    {
        struct net_state nst;
        for (int k = 0; k < 4; k++) { nst.ip[k]=0; nst.net[k]=0; nst.mask[k]=0; nst.gw[k]=0; }
        nst.have = 0;
        uint8_t rxbuf[0x4000 + 512];
        uint32_t acc = 0;
        int waited = 0;

        /* W.496: снять паузу TX. REG_TXPAUSE(0x0522)=0xFF означает «все очереди
           на паузе» — драйвер ставит его при init/стопе и снимает (0x00) в
           рабочем режиме. Мы этот регистр не трогали, поэтому data-очередь LOQ
           могла стоять: тот же путь TX, что у mgmt (который ходит иначе), из-за
           чего auth/assoc проходили, а data (DISCOVER/msg2) — нет. */
        {
            uint8_t txp = 0xFF;
            h3_sdc_read_wlan_reg8(0x0522, &txp);
            h3_sdc_write_wlan_reg8(0x0522, 0x00);
            uart_print("  join: txpause was=0x");
            uart_print_hex((uint32_t)txp);
            uart_print(" -> 0x00\r\n");
        }

        /* W.472: для WPA2-сети первый DISCOVER НЕ шлём в plaintext (AP после
           assoc ждёт только CCMP-encrypted; plaintext кадр дропается и висит
           в LOQ, задерживая msg4). Отправляем DISCOVER только ПОСЛЕ 4-way
           (wpa2_done) — уже зашифрованным. Для открытой сети (pass_len=0)
           DISCOVER отправляем сразу. */
        if (join_target.pass_len == 0) {
            uart_print("  join: dhcp discover send\r\n");
            net_dhcp_discover(&nst);
        } else {
            /* W.477: для WPA2-сети deferred DHCP — но часть AP (MTS_GPON_d23898)
               НЕ инициирует 4-way сама, если клиент молчит после AssocResp.
               Шлём EAPOL-Start (ver=1,type=1, len=0) как plaintext trigger,
               чтобы AP отправила EAPOL-Key msg1. */
            uint8_t eapol_start[4] = {0x01, 0x01, 0x00, 0x00};
            h3_wifi_tx_data(0x888E, eapol_start, 4);
            uart_print("  join: wpa2 net — dhcp deferred, eapol-start sent\r\n");
        }
        int eapol_count = 0;
        int wpa2_done = 0;      /* W.459: handshake завершён (PTK/GTK в CAM) */
        int dhcp_resent = 0;    /* W.459: DHCP повторно отправлен после WPA2 */
        int rx_dbg_done = 0;    /* W.494: RX-счётчики напечатаны один раз */

        while (waited < 6000) {   /* W.469: увеличено 1500→6000 — DHCP OFFER на WPA2 после
                                4-way идёт broadcast и может прийти с задержкой; окно 3с
                                было мало (MTS eapol=2, но OFFER не успевал) */
            /* W.459: первый DISCOVER ушёл plaintext ДО установки PTK — AP его дропнул
               (после WPA2 ждёт только CCMP-encrypted data). Пересылаем DISCOVER сразу
               после полного 4-way (wpa2_done=1) — теперь чип зашифрует его TK. */
            /* W.495: DISCOVER шлём 3 раза с интервалом ~1 с (как стандартный
               DHCP-клиент ретраит). Один потерянный кадр не должен убивать всю
               попытку. Перед первой отправкой — 100 мс на применение ключей. */
            if (wpa2_done && dhcp_resent < 3 && waited >= dhcp_resent * 1000) {
                if (dhcp_resent == 0) udelay(100000);
                uart_print("  join: dhcp discover (enc) #");
                uart_print_int(dhcp_resent + 1);
                uart_print("\r\n");
                net_dhcp_discover(&nst);
                dhcp_resent++;
                /* W.495 [TX-диагностика]: TXPKT_EMPTY (0x041A) бит0=1 — очередь
                   TX пуста (кадр ушёл в эфир). 0 — застрял в очереди.
                   LOQ free pages — сколько страниц осталось. */
                {
                    uint8_t txe = 0xFF;
                    int loq = h3_wifi_tx_free_pages_loq();
                    h3_sdc_read_wlan_reg8(0x041A, &txe);
                    uart_print("  join: tx-dbg txempty=0x");
                    uart_print_hex((uint32_t)txe);
                    uart_print(" loq=");
                    uart_print_int(loq);
                    uart_print("\r\n");
                }
            }
            /* W.494 [диагностика RX]: через ~500 мс после DISCOVER прочитать
               счётчики приёма FW. RXPKT_NUM (0x0284): cnt[15:0]=пакетов в
               буфере FW; b18=RW_RELEASE_EN. RXERR_RPT (0x0664): счётчик по
               выбранному типу. Если cnt>0 — FW приняла кадры от AP (значит
               проблема в дешифровке/выдаче), если 0 — AP не ответила. */
            {
                if (dhcp_resent && !rx_dbg_done && waited >= 500) {
                    uint32_t rxn = 0, rxerr = 0;
                    h3_sdc_read_wlan_reg32(0x0284, &rxn);
                    h3_sdc_read_wlan_reg32(0x0664, &rxerr);
                    uart_print("  join: RX-dbg rxpk=0x");
                    uart_print_hex(rxn);
                    uart_print(" cnt=");
                    uart_print_int((int)(rxn & 0xFFFF));
                    uart_print(" b18=");
                    uart_print_int((int)((rxn >> 18) & 1));
                    uart_print(" rxerr=0x");
                    uart_print_hex(rxerr);
                    uart_print("\r\n");
                    rx_dbg_done = 1;
                }
            }
            if (rx_read_packet(rxbuf, sizeof(rxbuf), &acc) != 0) { udelay(2000); waited += 2; continue; }
            if (acc == 0) { udelay(2000); waited += 2; continue; }
            uint32_t off = 0;
            while (off + 26 <= acc) {
                uint32_t plen = (uint32_t)(rxbuf[off] | ((rxbuf[off+1] & 0x3F) << 8));
                if (plen == 0 || plen > 0x2000) break;
                uint32_t drvsz = (uint32_t)(rxbuf[off+2] & 0x0Fu) << 3;
                uint32_t fr = off + 24u + drvsz;
                if (fr + 30 <= acc) {
                    uint16_t fcfc = (uint16_t)(rxbuf[fr] | (rxbuf[fr+1] << 8));
                    uint32_t ftype = (fcfc >> 2) & 0x3;      /* 0=mgmt, 1=ctrl, 2=data */
                    if (ftype == 2) {
                        uint32_t dp = fr + 24 + ((fcfc & 0x0080) ? 2 : 0)
                                    + (((fcfc & 0x000C) == 0x000C) ? 6 : 0);
                        uint16_t etype = (uint16_t)((rxbuf[dp+6] << 8) | rxbuf[dp+7]);
                        /* W.406: ethertype в проводе big-endian (IPv4=08 00).
                           Раньше читали LE (rxbuf[dp+6] | rxbuf[dp+7]<<8 → 0x0008),
                           из-за чего сравнение с 0x0800/0x888E никогда не срабатывало
                           и DHCP OFFER прилетал, но net_handle_ipv4 не вызывался. */
                        /* W.482: print rx DATA etype не более чем для первых кадров (флуд
                           зашифрованных 0x3208/0xABC3 тормозил лог на десятках
                           строк/цикле). Сумма — см. eapol_count/NET-данные ниже. */
                        {
                            static int dbg_cnt = 0;
                            if (dbg_cnt < 8 || etype == 0x0800 || etype == 0x888E) {
                                uart_print("  join: rx DATA etype=0x");
                                uart_print_hex((uint32_t)etype);
                                uart_print(" plen=");
                                uart_print_int((int)plen);
                                uart_print("\r\n");
                            }
                            dbg_cnt++;
                        }
                        if (etype == 0x0800)      /* IPv4: парсим DHCP OFFER */
                            net_handle_ipv4(&rxbuf[dp+8], (uint16_t)plen, &nst);
                        if (etype == 0x888E) {    /* WPA2 EAPOL-Key (4-way) */
                            eapol_count++;
                            /* W.412 [диагностика]: EAPOL-Key начинается сразу после
                               LLC/SNAP (AA AA 03 00 00 00) + ethertype 0x888E.
                               dp+8 = EAPOL header: version, type, length(be).
                               dp+12 = EAPOL-Key body. Печатаем msg1-поля. */
                            uint8_t ea = rxbuf[dp+8+1];       /* EAPOL type */
                            if (ea == 0x03) {                 /* EAPOL-Key */
                                /* W.413 [P2]: правильные смещения тела EAPOL-Key (RFC 4137):
                                   +0 = Descriptor Type (0x02 RSN)
                                   +1..2 = Key Information
                                   +3..4 = Key Length
                                   +5..12 = Key Replay Counter
                                   +13..44 = Key Nonce (ANonce) */
                                uint16_t kinfo = (uint16_t)((rxbuf[dp+12+1]<<8)|rxbuf[dp+12+2]);
                                /* ANonce (32 байта на +13) */
                                uint8_t  anonce[32];
                                for (int aa = 0; aa < 32; aa++) anonce[aa] = rxbuf[dp+12+13+aa];
                                /* W.488: убран подробный дамп полей msg1
                                   (kinfo/rsn_type/keylen/descr/pair/ack/mic/anonce) —
                                   печатается одна строка на msg1 ниже. */
                                /* W.447: сохранить msg1 для 4-way — ANonce + replay counter.
                                   Детект msg1: Key ACK (bit7=0x0080) установлен И
                                   Encrypted Key Data (bit12=0x1000) сброшен. msg3 (GTK)
                                   имеет Encrypted Key Data=1 и обрабатывается отдельно.
                                   Kinfo msg1 = 0x008A (version 2 | pairwise | Key ACK). */
                                if ((kinfo & 0x0080) && !(kinfo & 0x1000)) {
                                    /* W.447 [диагностика]: показать полный msg1 replay (8Б) и весь ANonce (32Б)
                                       рядом с msg2 — сверить, эхо ли это (если msg1 replay растёт 2,3,4,
                                       значит AP сам шлёт новые msg1, не ретраит; тогда мой эхо корректен). */
                                    /* W.488: одна короткая строка на msg1. */
                                    uart_print("    msg1 replay=");
                                    uart_print_hex((uint32_t)rxbuf[dp+12+5+6]);
                                    uart_print_hex((uint32_t)rxbuf[dp+12+5+7]);
                                    uart_print("\r\n");
                                    for (int aa = 0; aa < 32; aa++) join_target.anonce[aa] = anonce[aa];
                                    for (int aa = 0; aa < 8; aa++) join_target.replay[aa] = rxbuf[dp+12+5+aa];
                                    join_target.have_msg1 = 1;
                                    /* W.449: ответить msg2 (PMK/PTK/SNonce/MIC),
                                       затем ждать msg3 (GTK). */
                                    if (join_target.pass_len > 0) {
                                        int m2 = wpa_tx_msg2();
                                        uart_print("    msg2 tx rc=");
                                        uart_print_int(m2);
                                        uart_print("\r\n");
                                    }
                                }
/* W.450/W.469: msg3 (GTK) — AP приняла наш msg2. Key ACK + Encrypted Key Data
                                   установлены; GTK в key_data (wrapped, AES-Key-Wrap
                                   по KEK=ptk[16..32)). Распаковываем GTK.
                                   W.469: при have_msg1==2 (msg3 уже обработан) НЕ игнорируем
                                   повторный msg3 — AP ретраит его, пока не примет msg4.
                                   Дублирующий GTK unwrap безопасен (idempotent); главное —
                                   ответить msg4 на КАЖДЫЙ msg3, иначе AP висит (A05s eapol=5). */
                                if ((kinfo & 0x0080) && (kinfo & 0x1000) && join_target.have_msg1 >= 1) {
                                    uint16_t kd_len = (uint16_t)((rxbuf[dp+12+93]<<8)|rxbuf[dp+12+94]);
                                    uart_print("    EAPOL msg3 (GTK) kd_len=");
                                    uart_print_int((int)kd_len);
                                    if (join_target.pass_len > 0 && kd_len >= 24 && kd_len <= 128) {
                                        uint8_t gtk[64];
                                        uint32_t glen = 0;
                                        /* key_data [95..95+kd_len) */
                                        int uw = wpa_aes_kw_unwrap(join_target.ptk+16,
                                                    rxbuf+dp+12+95, kd_len, gtk, &glen);
                                        if (uw == 0) {
                                            uart_print(" gtk_ok len=");
                                            uart_print_int((int)glen);
                                            join_target.have_msg1 = 2;   /* msg3 OK */
                                            /* W.459: msg4 обязан нести replay-счётчик из msg3
                                               (не из msg1). Захватываем здесь. */
                                            for (int rr = 0; rr < 8; rr++)
                                                join_target.replay[rr] = rxbuf[dp+12+5+rr];
                                            /* msg4: подтверждение (KeyMIC, без GTK).
                                               key_info msg4 = 0x030A (version2|keymic|secure,
                                               no install, no ack); replay из msg3. */
                                            uint8_t b4[95];
                                            for (int k = 0; k < 95; k++) b4[k] = 0;
                                            b4[0] = 0x02;
                                            /* W.459 [фикс]: key_info msg4 = 0x030A — это ГЛАВНЫЙ
                                               баг W.458. Было 0x0042 (Install|ver2, БЕЗ Secure), из-за
                                               чего AP не принимал msg4 и повторял msg3. По 802.11i
                                               msg4 обязан нести SECURE-бит: ver2(0x0002) | pairwise(0x0008)
                                               | MIC(0x0100) | SECURE(0x0200) = 0x030A. */
                                            b4[1] = 0x03; b4[2] = 0x0A;
                                            b4[3] = 0x00; b4[4] = 0x00;   /* key_len 0 (msg4 без ключей) */
                                            for (int k = 0; k < 8; k++) b4[5+k] = join_target.replay[k]; /* replay из msg3 */
                                            for (int k = 0; k < 32; k++) b4[13+k] = 0;   /* Key Nonce = 0 (в msg4) */
                                            uint8_t p4[8+95];
                                            p4[0] = 1; p4[1] = 3; p4[2] = 0; p4[3] = 95;   /* W.459: EAPOL ver=1, как msg2 */
                                            for (int k = 0; k < 95; k++) p4[4+k] = b4[k];
                                            uint8_t m4[20];
                                            wpa_hmac_sha1(join_target.ptk, 16, p4, 4+95, m4);
                                            for (int k = 0; k < 16; k++) p4[4+77+k] = m4[k];
                                            /* W.474/475: пауза 15 мс ПЕРЕД msg4. После убирания
                                               plaintext DHCP (W.472/473) msg4 стал уходить
                                               мгновенно после msg3 — MTS_GPON регрессировал
                                               (1x msg3 -> 3x msg3): AP не успевает «встать» в
                                               состояние ожидания msg4 и отбрасывает его.
                                               hostapd/wpa_supplicant держат EAPOL-таймер;
                                               bare-metal ответ был слишком быстрым. */
                                            udelay(15000);
                                            /* W.464: ДЛИНА кадра msg4 = 4(EAPOL hdr) + 95(тело) = 99,
                                               а не 8+95=103 (был мусор-хвост 4 байта — строгий AP
                                               игнорирует/отбрасывает). У msg2 длина уже точная. */
                                            int m4rc = h3_wifi_tx_data(0x888E, p4, 4+95);
                                            uart_print(" msg4 tx rc=");
                                            uart_print_int(m4rc);
                                            uart_print(" kinfo=0x");
                                            uart_print_hex((uint32_t)((b4[1]<<8)|b4[2]));
                                            uart_print("\r\n");
                                            /* Установить ключи в CAM чипа:
                                               pairwise TK = ptk[32..48) (AES-CCMP),
                                               group GTK = gtk[0..glen). */
                                            /* W.442/468: pairwise TK = ptk[32..48) (AES-CCMP) — unicast, ищется
                                               по BSSID AP. */
                                            {
                                                uint8_t macz[6] = {0,0,0,0,0,0};
                                                wpa_cam_write_key(4, join_target.bssid,
                                                                  join_target.ptk + 32, 0);
                                                /* W.468/471: GTK = group/default key (cam 0), null-addr
                                                   + BIT6 (group) — как драйвер rtw_mlme_ext.c:14214.
                                                   W.471 [фикс]: распакованный key_data приходит в виде
                                                   KDE (Key Data Encapsulation), сам GTK НЕ с байта 0:
                                                     [0]=0xDD [1]=len(48) [2..5]=00 0F AC 01
                                                     [6]=keyid [7]=reserved [8..23]=GTK(16 байт)
                                                   Раньше писали gtk[0..16) = "DD 30 00 0F.." (заголовок)
                                                   — чип получал мусор и не расшифровывал broadcast.
                                                   Проверяем OUI и берём GTK со смещения 8. */
                                                const uint8_t *gk = gtk;
                                                /* W.480: plaintext key_data в msg3 может НЕ начинаться
                                                   с GTK-KDE (напр. сначала RSN IE: 30 14 01 00 00 0F AC 04..).
                                                   Сканируем весь plaintext на GTK-KDE
                                                   [0xDD][len][00 0F AC][0x01][keyid][reserved][16 GTK],
                                                   GTK на +8. Fallback — gtk. */
                                                {
                                                    uint32_t gk_off = 0;
                                                    for (uint32_t off = 0; off + 8 <= glen; off++) {
                                                        if (gtk[off] == 0xDD &&
                                                            off + 2 + gtk[off+1] <= glen &&
                                                            gtk[off+2]==0x00 && gtk[off+3]==0x0F &&
                                                            gtk[off+4]==0xAC && gtk[off+5]==0x01) {
                                                            gk = gtk + off + 8;
                                                            gk_off = off;
                                                            break;
                                                        }
                                                    }
                                                    uart_print(" gtk_from=");
                                                    uart_print_int((int)gk_off);
                                                }
                                                 wpa_cam_write_key(0, macz, gk, 1);
                                                 /* W.484: readback сокращён до word0 двух слотов.
                                                    Прежний полный дамп 16 слов с поллингом до 10 с/слово
                                                    давал десятки секунд паузы в логе и срывал тайминги.
                                                    word0 достаточно, чтобы подтвердить раскладку:
                                                    slot0 (GTK) = VALID|AES|GROUP, MAC=00:00;
                                                    slot4 (pairwise) = VALID|AES, MAC=BSSID. */
                                                 for (int cam_slot = 0; cam_slot < 8; cam_slot += 4) {
                                                     uint32_t addr_w = (uint32_t)(cam_slot * 8);
                                                     h3_sdc_write_wlan_reg32(0x0670,
                                                         CAM_POLLINIG | CAM_READ | addr_w);
                                                     for (int p = 0; p < 100; p++) {
                                                         uint32_t rd = 0;
                                                         h3_sdc_read_wlan_reg32(0x0670, &rd);
                                                         if (!(rd & CAM_POLLINIG)) break;
                                                         udelay(1);
                                                     }
                                                     uint32_t cv = 0;
                                                     h3_sdc_read_wlan_reg32(0x0678, &cv);
                                                     uart_print(" cam");
                                                     uart_print_int(cam_slot);
                                                     uart_print("=");
                                                     uart_print_hex(cv);
                                                 }
                                            }
                                            /* W.462: включить CCMP TxEnc/RxDec в чипе — БЕЗ этого
                                               даже с ключами в CAM чип физически НЕ шифрует/дешифрует.
                                               REG_SECCFG (0x680) = 0xCF (как драйвер 8188F в STA при
                                               opensystem): TxUseDK|RxUseDK|TxEncEnable(bit2)|RxDecEnable(bit3)
                                               |TXBCUSEDK|RXBCUSEDK. unicast шифруется pairwise-ключом из
                                               CAM; broadcast/multicast — default key (GTK). */
                                             /* W.487 [фикс]: seccfg=0xCC, НЕ 0xCF.
                                                hal_com.c:5809/5833 (эталон 8189FS) для 802.1X
                                                (WPA2) ставит только TxEncEnable|RxDecEnable
                                                (+TXBC/RXBCUSEDK), а SCR_TxUseDK(b0)/SCR_RxUseDK(b1)
                                                добавляет ЛИШЬ для не-802.1X. С b0=1 чип форсил
                                                Default Key (GTK) на ВСЕ data-кадры, включая
                                                unicast DISCOVER → AP ждал pairwise и не мог
                                                расшифровать. 0xCC = TxEnc|RxDec|TXBCUSEDK|RXBCUSEDK. */
                                             h3_sdc_write_wlan_reg8(0x0680, 0xCC);
                                            /* W.459 [диагностика]: дамп CAM после загрузки ключей.
                                               seccfg 0x0680 = 0xC0 (CCMP enc/dec on) если ключи
                                               реально активны; camcmd 0x0670 покажет занятость. */
                                            {
                                                uint8_t sec_cfg = 0, cam_cmd = 0;
                                                h3_sdc_read_wlan_reg8(0x0680, &sec_cfg);
                                                h3_sdc_read_wlan_reg8(0x0670, &cam_cmd);
                                                uart_print(" cam: seccfg=0x");
                                                uart_print_hex((uint32_t)sec_cfg);
                                                uart_print(" camcmd=0x");
                                                uart_print_hex((uint32_t)cam_cmd);
                                                wpa2_done = 1;
                                                join_target.secure = 1;
                                            }
                                            uart_print(" cam keys loaded\r\n");
                                        } else {
                                            uart_print(" gtk_unwrap_FAIL rc=");
                                            uart_print_int(uw);
                                        }
                                    }
                                    uart_print("\r\n");
                                }
                            } else {
                                uart_print("    eapol type=0x");
                                uart_print_hex((uint32_t)ea);
                                uart_print("\r\n");
                            }
                        }
                    }
                }
                off += 24u + drvsz + plen;
                off = (off + 7u) & ~7u;   /* выравнивание пакета до 8 байт */
            }
            if (nst.have) break;
            udelay(2000);
            waited += 2;
        }
        /* W.402 [P2/P3]: честный отчёт. DHCP OFFER/ACK с IP — успех; иначе
           WPA2-pending (EAPOL есть) или DHCP FAIL — не выдавать «dhcp done». */
        if (nst.have) {
            uart_print("  join: DHCP OK ip=");
            for (int k = 0; k < 4; k++) { uart_print_int((int)nst.ip[k]); if (k < 3) uart_print("."); }
            uart_print("\r\n");
        } else if (eapol_count > 0) {
            uart_print("  join: WPA2 pending (eapol=");
            uart_print_int(eapol_count);
            uart_print("), no DHCP/IP\r\n");
        } else {
            uart_print("  join: DHCP FAIL (no OFFER in 5s)\r\n");
        }
    }

    /* W.388: вернуть приём в scan-режим после data-rx poll. Без этого
       следующий join наследует data-режим (RCR CBSSID_DATA, RXFLTMAP2=ffff),
       и mgmt/auth ответы фильтруются → недетерминизм (success→2-5 fails→success).
       Scan-mode = исходные значения из site-survey (RCR 0x7000604E,
       RXFLTMAP0=ffff mgmt, RXFLTMAP1=0400 ps-poll, RXFLTMAP2=0000 no-data). */
    {
        h3_sdc_write_wlan_reg32(0x0608, 0x7000604Eu);
        h3_sdc_write_wlan_reg16(0x06A0, 0xFFFF);
        h3_sdc_write_wlan_reg16(0x06A2, 0x0400);
        h3_sdc_write_wlan_reg16(0x06A4, 0x0000);
        rxp_seq = 0;   /* сброс seq чтения RX — следующий join стартует ровно */
        udelay(1000);
        uart_print("  join: scan-mode restored (rcr=7000604E, rxflt data off, rxp_seq=0)\r\n");
    }

    return 0;   /* ассоциация подтверждена */
}

int h3_wifi_tx_probe_req(uint8_t channel)
{
    uint8_t buf[40 + 64];
    uint32_t i;

    /* 1) tx_desc 40 байт, занулён. */
    for (i = 0; i < 40; i++) buf[i] = 0;

    /* dw0: PKT_SIZE [15:0] (заполняется после сборки кадра), OFFSET=40 [23:16],
       BMC=1 [24], LAST_SEG=1 [26], FIRST_SEG=1 [27] — как у драйвера для
       одиночного буфера (rtl8188f_hal_init.c:3213-3214). */
    buf[2] = (uint8_t)TXDESC_SIZE_W220;                     /* OFFSET [23:16] = 40 */
    buf[3] = (uint8_t)((1u << 0) | (1u << 2) | (1u << 3) | (1u << 7)); /* BMC|LAST|FIRST|OWN(bit31) */

    /* dw1: MACID=0, QUEUE_SEL=QSLT_MGNT=0x12 (bits 8-12). */
    buf[5] = (uint8_t)(0x12u << 0); /* QUEUE_SEL = 0x12 */

    /* dw3: USE_RATE=1 (bit8). */
    buf[13] = (uint8_t)(1u << 0);   /* USE_RATE bit8 dw3 */

    /* dw4: TX_RATE=0x00 (DESC_RATE1M), RETRY_LIMIT_ENABLE=1 (bit17),
       DATA_RETRY_LIMIT=12 (bits 18-23). */
    buf[16] = 0x00;                                  /* TX_RATE */
    /* dw4: RETRY_LIMIT_ENABLE = bit17, DATA_RETRY_LIMIT = bits[23:18] (6 бит),
       поэтому значение 12 сдвигается на 2 (rtl8188f_xmit.h:167-168). */
    buf[18] = (uint8_t)((1u << 1) | (12u << 2));

    /* 2) кадр probe request с базы buf+40. */
    uint8_t *f = buf + 40;
    /* frame_control: type=mgmt(0), subtype=probe request(4) => 0x0040. */
    f[0] = 0x40; f[1] = 0x00;
    /* duration = 0 */
    /* addr1 = broadcast */
    for (i = 0; i < 6; i++) f[4 + i] = 0xFF;
    /* addr2 = наш MAC (02:00:00:09:7A:5C, но после LCK генер. — читать из 0x610) */
    {
        uint8_t m[6];
        h3_sdc_read_wlan_reg8(0x610, &m[0]);
        h3_sdc_read_wlan_reg8(0x611, &m[1]);
        h3_sdc_read_wlan_reg8(0x612, &m[2]);
        h3_sdc_read_wlan_reg8(0x613, &m[3]);
        h3_sdc_read_wlan_reg8(0x614, &m[4]);
        h3_sdc_read_wlan_reg8(0x615, &m[5]);
        for (i = 0; i < 6; i++) f[10 + i] = m[i];
    }
    /* addr3 = broadcast */
    for (i = 0; i < 6; i++) f[16 + i] = 0xFF;
    /* seq = 0 (у нас mgnt_seq не ведём) */
    f[22] = 0; f[23] = 0;
    uint32_t plen = 24;   /* 802.11 hdr 3addr */

    /* SSID IE: id=0, len=0 */
    f[plen++] = 0x00;
    f[plen++] = 0x00;

    /* Supported Rates IE: id=1, len=8, 1M..11M (CCK) + 6M (OFDM) */
    f[plen++] = 0x01;
    f[plen++] = 0x08;
    f[plen++] = 0x02;   /* 1M */
    f[plen++] = 0x04;   /* 2M */
    f[plen++] = 0x0B;   /* 5.5M */
    f[plen++] = 0x16;   /* 11M */
    f[plen++] = 0x0C;   /* 6M */
    f[plen++] = 0x12;   /* 9M */
    f[plen++] = 0x18;   /* 12M */
    f[plen++] = 0x24;   /* 18M */

    /* DS Set IE: id=3, len=1, channel */
    f[plen++] = 0x03;
    f[plen++] = 0x01;
    f[plen++] = channel;

    /* 3) PKT_SIZE = plen (только кадр, без desc). В tx_desc dw0 [0:15]. */
    buf[0] = (uint8_t)(plen & 0xFF);
    buf[1] = (uint8_t)((plen >> 8) & 0xFF);

    /* W.361: TRXFF_BNDY (0x114) — 3 замера (pre/post/16-bit), чтобы понять,
       почему txpre читает trxffb=0, хотя init пишет 0xF8. Если post=0xF8 —
       write работает и txpre читает не тот адрес; если post=0 — запись
       блокирована или регистр требует 16-бит. */
    {
        uint8_t t0 = 0, t1 = 0;
        uint16_t t16 = 0;
        h3_sdc_read_wlan_reg8(0x0114, &t0);
        WLOG("  tx: trxffb_pre=0x");
        WLOG_HEX((uint32_t)t0);
        h3_sdc_write_wlan_reg8(0x0114, 0xF8);
        udelay(1000);
        h3_sdc_read_wlan_reg8(0x0114, &t1);
        WLOG(" post=0x");
        WLOG_HEX((uint32_t)t1);
        h3_sdc_read_wlan_reg16(0x0114, &t16);
        WLOG(" 16=0x");
        WLOG_HEX((uint32_t)t16);
        WLOG("\r\n");
    }

    /* 4) Дождаться свободной страницы в очереди HIQ: без этого кадр уходит в
       переполненный FIFO и теряется (эталон: rtl8189fs_xmit.c:95-121 —
       query_free_page + HalQueryTxBufferStatus). Пакет <= 128 байт = 1 страница. */
    {
        int fp = -1;
        for (int t = 0; t < 100; t++) {
            fp = h3_wifi_tx_free_pages_hiq();
            if (fp >= 1) break;
            udelay(1000);
        }
        WLOG("  tx: hiq_free=");
        WLOG_INT(fp);
        WLOG("\r\n");
        if (fp < 1) return -2;
    }

    /* 5) запись: len = 40 + plen, округлить до 4. */
    uint32_t total = (40 + plen + 3) & ~3u;
    uint32_t fifo_addr = (WLAN_TX_HIQ_DEVICE_ID_W220 << 13)
                       | ((total / 4) & 0x1FFFu);

    WLOG("  tx: probe ch=");
    WLOG_INT((int)channel);
    WLOG(" len=");
    WLOG_HEX(total);
    WLOG(" fifo=0x");
    WLOG_HEX(fifo_addr);
    WLOG("\r\n");

    int wrc = h3_sdc_cmd53_write_bytes(1, fifo_addr, total, buf);

    /* W.350: полный дамп TX-пути ДО и ПОСЛЕ записи кадра в HIQ — по списку
       регистров: TXPKT_EMPTY(0x041A, hal_com_reg.h:251), TXFF_PTR(0x0420),
       TXPAUSE(0x0522), CR-MACTXEN(0x0100 bit6), PBP(0x0104),
       RXDMA_MODE(0x0290), TRXDMA_CTRL(0x010C), RQPN(0x0200), TXDMA(0x0400),
       + free pages HIQ (SDIO-local 0x20). Сверяем, уходит ли FALSE в FX. */
    uint8_t mempty = 0, mpause = 0, mtrffb = 0, mpbplo = 0, mpbphi = 0;
    uint32_t mff, mcr, mrx, mtrx, mrqpn, mtx;
    {
        h3_sdc_read_wlan_reg8(0x041A, &mempty);   /* TXPKT_EMPTY */
        h3_sdc_read_wlan_reg32(0x011C, &mff);     /* REG_RXFF_PTR: FIFO-указатели RX/TX (двигаются ли) */
        h3_sdc_read_wlan_reg8(0x0522, &mpause);   /* TXPAUSE */
        h3_sdc_read_wlan_reg32(0x0100, &mcr);     /* CR (MACTXEN bit6) */
        h3_sdc_read_wlan_reg8(0x0114, &mtrffb);   /* TRXFF_BNDY (8-bit) */
        h3_sdc_read_wlan_reg8(0x0104, &mpbplo);   /* PBP lo (RX) */
        h3_sdc_read_wlan_reg8(0x0105, &mpbphi);   /* PBP hi (TX) */
        h3_sdc_read_wlan_reg32(0x0290, &mrx);     /* RXDMA_MODE */
        h3_sdc_read_wlan_reg32(0x010C, &mtrx);    /* TRXDMA_CTRL */
        h3_sdc_read_wlan_reg32(0x0200, &mrqpn);   /* RQPN */
        h3_sdc_read_wlan_reg32(0x0400, &mtx);     /* TXDMA_CTRL */
        WLOG("  txpre empty=0x");
        WLOG_HEX((uint32_t)mempty);
        WLOG(" ffptr=0x");
        WLOG_HEX(mff);
        WLOG(" pause=0x");
        WLOG_HEX((uint32_t)mpause);
        WLOG(" cr=0x");
        WLOG_HEX(mcr);
        WLOG(" trxffb=0x");
        WLOG_HEX((uint32_t)mtrffb);
        WLOG(" pbp_lo=0x");
        WLOG_HEX((uint32_t)mpbplo);
        WLOG(" pbp_hi=0x");
        WLOG_HEX((uint32_t)mpbphi);
        WLOG(" rxdma=0x");
        WLOG_HEX(mrx);
        WLOG(" trxdma=0x");
        WLOG_HEX(mtrx);
        WLOG(" rqpn=0x");
        WLOG_HEX(mrqpn);
        WLOG(" dbg=0x");
        WLOG_HEX(mtx);
        WLOG("\r\n");
    }
    udelay(50000);   /* W.361: дать FW 50 мс на обработку кадра */
    {
        uint8_t a = 0;
        uint32_t d = 0, d2 = 0, d3 = 0;
        int fp2 = h3_wifi_tx_free_pages_hiq();
        (void)fp2;   /* используется только в WLOG (verbose) */
        h3_sdc_read_wlan_reg8(0x041A, &a);   /* REG_TXPKT_EMPTY (0x041A!) */
        h3_sdc_read_wlan_reg32(0x011C, &d3); /* REG_RXFF_PTR: после записи (двинулся ли) */
        WLOG("  tx: wrc=");
        WLOG_INT(wrc);
        WLOG(" post hiq_free=");
        WLOG_INT(fp2);
        WLOG(" txpkt_empty=0x");
        WLOG_HEX((uint32_t)a);
        WLOG(" trxff=0x");
        WLOG_HEX(d3);
        h3_sdc_read_wlan_reg32(0x0405, &d);  /* REG_TXDMA_OFFSET */
        WLOG(" txdma_off=0x");
        WLOG_HEX(d);
        h3_sdc_read_wlan_reg32(0x0400, &d2); /* REG_TXDMA_CTRL/status */
        WLOG(" txdma=0x");
        WLOG_HEX(d2);
        uart_print("\r\n");
    }
    return wrc;
}