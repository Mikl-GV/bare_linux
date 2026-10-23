#include "h3_sdc0.h"
#include "h3_sdc.h"
#include "h3_ccu.h"
#include "h3.h"
#include "ports.h"
#include "uart.h"
#include "sys.h"

/* Слот SD-карты OPi Lite на контроллере SDC0 (база 0x01C0F000).
 * Двухфазный init (см. h3_sdc0.h):
 *   - Фаза A (первичная): наследуем карту, уже инициализированную U-Boot
 *     (загрузка с SD+U-Boot). Делаем минимум: gate только для полноты,
 *     без сброса контроллера и без CMD0/ACMD41 — проверка CMD17.
 *   - Фаза B (если A не удалась, например после FEL+SPL, где U-Boot карту
 *     не трогал): полный подъём с нуля —
 *   1) CCU: gate + reset MMC0 (GATING0/RST0 bit8), SDMMC0_CLK = OSC24M, init-такт.
 *   2) Пины PC0..PC5 -> function 2 (SDC0): CLK=PC0, CMD=PC1, D0=PC2, D1=PC3,
 *      D2=PC4, D3=PC5 (PC6 = CD в OPi Lite не задействован). U-Boot на этой
 *      плате: PORTC cfg0 = 0x77222222 (PC0..PC5 = 2, PC6/7 = 7) — совпадает.
 *   3) Сброс контроллера (GCTL_RESET), HWRST, CKC-такты, FUNS=0xceAA.
 *   4) Инициализация карты: CMD0 (80 тактов) -> CMD8 -> CMD55/ACMD41 (HCS,
 *      таймаут по busy) -> CMD2 (CID) -> CMD3 (RCA) -> CMD7 (select) ->
 *      CMD9 (CSD, объём карты) -> ACMD6 (4-bit) -> CMD16 (blocklen 512) ->
 *      CMD58 (SDSC/SDHC, CCS bit30).
 *
 * Чтение/запись: 1 блок (512 Б) за команду через FIFO (PIO, поллинг
 * FIFO_EMPTY/FIFO_LEVEL) — как в эталонном sd.c pico-retro. Host не тянет
 * многоблочный CMD18 (то же ограничение, что на SDC1/WiFi). */

#define SDC0_BASE   H3_SDC0_BASE
#define SDC0_REG(o) (*(volatile uint32_t *)(SDC0_BASE + (o)))
#define SDC0_FIFO   (*(volatile uint32_t *)(SDC0_BASE + SDC_FIFO))

/* Пины PC0..PC5 в режим SDC0 (function 2). 4 бита на пин. */
#define PORT_CFG0   (*(volatile uint32_t *)(PORTC + 0x00))
#define SDC0_PINS_MASK   0x00FFFFFFu   /* PC0..PC5 */
#define SDC0_PINS_FUNC2  0x00222222u

static uint8_t     g_sdhc = 1;   /* 1 = SDHC/SDXC (секторная адресация) */
static uint8_t     g_wide4 = 0;
static uint32_t    g_rca = 0;    /* 16-бит RCA карты (CMD3) */

static int sdc0_wait_rint(uint32_t want, uint32_t timeout)
{
    for (uint32_t t = 0; t < timeout; t++) {
        uint32_t r = SDC0_REG(SDC_RISR);
        if (r & SDC_RISR_RAW_ISTA) {          /* ошибка ответа/данных */
            SDC0_REG(SDC_RISR) = r;
            return -1;
        }
        if (r & want) {
            SDC0_REG(SDC_RISR) = r;
            return 0;
        }
    }
    return -1;
}

/* Отправить команду без данных. flags = SDC_CMDR_* (RESP_RCV, LONG_RESP,
 * CHK_CRC, SEND_INIT...). Ответа нет на CMD0 и CMD15; для остальных
 * проверяем CMD_DONE. При ошибке (таймаут/CRC) возвращаем -1, контроллер
 * НЕ сбрасываем (как эталон sd.c): после неудачного CMD8 (SDSC-карты его
 * не поддерживают) шина должна остаться рабочей для ACMD41. */
static int sdc0_cmd(uint32_t idx, uint32_t arg, uint32_t flags)
{
    SDC0_REG(SDC_RISR) = 0xFFFFFFFFu;
    SDC0_REG(SDC_ARG) = arg;
    __asm volatile("dmb sy" ::: "memory");
    SDC0_REG(SDC_CMDR) = (idx & SDC_CMDR_IDX_MASK) | SDC_CMDR_START | flags;
    __asm volatile("dmb sy" ::: "memory");
    return sdc0_wait_rint(SDC_RISR_CMD_DONE, 0xFFFFFFu);
}

/* Прочитать данные из FIFO (PIO): ждём FIFO не пуст, выгребаем слова. */
static int sdc0_read_data(uint8_t *buf, uint32_t bytes)
{
    uint32_t *dst = (uint32_t *)buf;
    uint32_t nwords = bytes >> 2;
    uint32_t i = 0;
    while (i < nwords) {
        uint32_t st;
        uint32_t t = 0;
        do {
            st = SDC0_REG(SDC_STAR);
            if (t++ > 0xFFFFFFu) return -1;
        } while (st & SDC_STAR_FIFO_EMPTY);
        uint32_t n = (st >> 17) & 0x3FFF;      /* FIFO_LEVEL */
        if (n == 0) n = 1;
        if (n > nwords - i) n = nwords - i;
        for (uint32_t j = 0; j < n; j++) dst[i++] = SDC0_FIFO;
    }
    return sdc0_wait_rint(SDC_RISR_DATA_DONE, 0xFFFFFFu);
}

/* Отправить команду с чтением одного блока (CMD17).
 * Порядок как в эталоне sd.c (pico-retro): после запуска CMD17 ждём
 * завершения команды и сразу выгребаем FIFO (ждать DATA_DONE ДО чтения
 * нельзя — данные из FIFO могут потеряться); DATA_DONE ждём в конце. */
static int sdc0_read_block(uint32_t addr, uint8_t *buf)
{
    SDC0_REG(SDC_BLKSZ) = 512;
    SDC0_REG(SDC_BYCNT) = 512;
    SDC0_REG(SDC_RISR) = 0xFFFFFFFFu;
    SDC0_REG(SDC_ARG) = addr;
    __asm volatile("dmb sy" ::: "memory");
    SDC0_REG(SDC_CMDR) = (17u & SDC_CMDR_IDX_MASK) | SDC_CMDR_START
                       | SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC
                       | SDC_CMDR_DATA_TRANS | SDC_CMDR_WAIT_PRE;
    __asm volatile("dmb sy" ::: "memory");
    if (sdc0_wait_rint(SDC_RISR_CMD_DONE, 0xFFFFFFu) != 0) return -1;
    return sdc0_read_data(buf, 512);
}

/* Записать данные в FIFO (PIO): ждём FIFO не полон, пишем слова. */
static int sdc0_write_data(const uint8_t *buf, uint32_t bytes)
{
    const uint32_t *src = (const uint32_t *)buf;
    uint32_t nwords = bytes >> 2;
    uint32_t i = 0;
    while (i < nwords) {
        uint32_t st;
        uint32_t t = 0;
        do {
            st = SDC0_REG(SDC_STAR);
            if (t++ > 0xFFFFFFu) return -1;
        } while (st & SDC_STAR_FIFO_FULL);
        uint32_t n = 32 - ((st >> 17) & 0x3FFF);   /* свободно в FIFO */
        if (n == 0) n = 1;
        if (n > nwords - i) n = nwords - i;
        for (uint32_t j = 0; j < n; j++) SDC0_FIFO = src[i++];
    }
    return sdc0_wait_rint(SDC_RISR_DATA_DONE, 0xFFFFFFu);
}

/* Записать один блок (CMD24). */
static int sdc0_write_block(uint32_t addr, const uint8_t *buf)
{
    SDC0_REG(SDC_BLKSZ) = 512;
    SDC0_REG(SDC_BYCNT) = 512;
    SDC0_REG(SDC_RISR) = 0xFFFFFFFFu;
    SDC0_REG(SDC_ARG) = addr;
    __asm volatile("dmb sy" ::: "memory");
    SDC0_REG(SDC_CMDR) = (24u & SDC_CMDR_IDX_MASK) | SDC_CMDR_START
                       | SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC
                       | SDC_CMDR_DATA_TRANS | SDC_CMDR_TRANS_DIR
                       | SDC_CMDR_WAIT_PRE;
    __asm volatile("dmb sy" ::: "memory");
    if (sdc0_wait_rint(SDC_RISR_CMD_DONE, 0xFFFFFFu) != 0) return -1;
    if (sdc0_write_data(buf, 512) != 0) return -1;
    /* Ждать, пока карта снимет busy (bit9 STATUS). */
    for (uint32_t t = 0; t < 0xFFFFFFu; t++)
        if (!(SDC0_REG(SDC_STAR) & (1u << 9))) break;
    return 0;
}

/* Индикатор «идёт опрос SD»: PA15 (LED_EN_SD) — АКТИВНЫЙ УРОВЕНЬ 0.
   LED светится при логическом 0 на PA15: 0 = карта используется программой,
   1 = не используется (горит постоянно вне операций, если не гасить).
   PA15 = output (config1, function 1) — настраиваем при каждом включении
   (дёшево: одна запись MMIO), чтобы не зависеть от порядка вызовов. */
#define SD0_LED_ON()  do { \
        set_pin_mode(PORTA, 15, 1); \
        set_pin_data(PORTA, 15, 0); \
        __asm volatile("dmb sy" ::: "memory"); \
    } while (0)
#define SD0_LED_OFF() do { \
        set_pin_data(PORTA, 15, 1); \
        __asm volatile("dmb sy" ::: "memory"); \
    } while (0)

/* Обёртки с учётом SDSC/SDHC-адресации. */
int h3_sdc0_read_sector(uint32_t lba, void *buf)
{
    int rc;
    SD0_LED_ON();
    uint32_t addr = g_sdhc ? lba : (lba << 9);
    rc = sdc0_read_block(addr, (uint8_t *)buf);
    SD0_LED_OFF();
    return rc;
}

int h3_sdc0_write_sector(uint32_t lba, const void *buf)
{
    int rc;
    SD0_LED_ON();
    uint32_t addr = g_sdhc ? lba : (lba << 9);
    rc = sdc0_write_block(addr, (const uint8_t *)buf);
    SD0_LED_OFF();
    return rc;
}

/* Чтение CSD: размер карты (в 512-байтных секторах) из C_SIZE/C_SIZE_MULT
 * старого CSD v1 или C_SIZE из CSD v2; затем CMD58 уточняет SDSC/SDHC. */
static uint32_t sdc0_parse_csd(uint32_t *csd)
{
    /* CSD регистры: resp0..resp3, весовой порядок big-endian (32-бит слова,
       старший байт первого). Собираем 16 байт CSD в правильном порядке. */
    uint8_t raw[16];
    for (int i = 0; i < 4; i++) {
        raw[i*4+0] = (uint8_t)(csd[3-i] >> 24);
        raw[i*4+1] = (uint8_t)(csd[3-i] >> 16);
        raw[i*4+2] = (uint8_t)(csd[3-i] >> 8);
        raw[i*4+3] = (uint8_t)(csd[3-i]);
    }
    /* CSD version: raw[0] bits [7:6]. */
    uint32_t ver = (raw[0] >> 6) & 0x3;
    if (ver == 0) {
        /* CSD v1.0: C_SIZE = raw[6]*256 + raw[7]>>6; C_SIZE_MULT = raw[9]&3;
           READ_BL_LEN = raw[5]&0xF. Объём блока = 2^READ_BL_LEN (обычно 512). */
        uint32_t csize   = ((uint32_t)raw[6] << 8) | (raw[7] >> 6);
        uint32_t csize_m = (raw[9] >> 0) & 0x3;
        uint32_t bl_len  = raw[5] & 0xF;
        uint32_t blocknr = (csize + 1) << (csize_m + 2);
        return blocknr << (bl_len - 9);   /* пересчёт в 512-байтные сектора */
    } else {
        /* CSD v2.0 (SDHC/SDXC): C_SIZE = raw[7]&0x3F, raw[8], raw[9]>>6. */
        uint32_t csize = ((uint32_t)(raw[7] & 0x3F) << 16)
                       | ((uint32_t)raw[8] << 8)
                       | (raw[9] >> 6);
        return (csize + 1) << 10;          /* секторов по 512 Б */
    }
}

static void h3_sdc0_init_pins(void)
{
    uint32_t c0 = PORT_CFG0;
    c0 &= ~SDC0_PINS_MASK;
    c0 |=  SDC0_PINS_FUNC2;
    PORT_CFG0 = c0;
    __asm volatile("dsb" ::: "memory");
}

int h3_sdc0_init(struct sdc0_info *info)
{
    SD0_LED_ON();   /* индикатор «идёт опрос SD» */
    /* ==== Фаза A: наследование U-Boot (загрузка с SD) ====
       U-Boot уже инициализировал MMC0, GPIO, клок и карту (лог: 
       "17.5 MiB/s" из boot.scr). НЕ делаем ни сброса контроллера, ни
       CMD0/ACMD41 — только включаем gate (может быть снят) и проверяем
       карту лёгким CMD17 (как эталон sd.c pico-retro: «U-Boot поднял»).
       Если карта отвечает — сразу работаем. */
    H3_CCU_GATING0 |= H3_CCU_GATE_MMC0;
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    {
        uint8_t probe[512];
        SDC0_REG(SDC_BLKSZ) = 512;
        SDC0_REG(SDC_BYCNT) = 512;
        SDC0_REG(SDC_RISR) = 0xFFFFFFFFu;
        SDC0_REG(SDC_ARG) = 0;
        __asm volatile("dmb sy" ::: "memory");
        SDC0_REG(SDC_CMDR) = (17u & SDC_CMDR_IDX_MASK) | SDC_CMDR_START
                           | SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC
                           | SDC_CMDR_DATA_TRANS | SDC_CMDR_WAIT_PRE;
        __asm volatile("dmb sy" ::: "memory");
        if (sdc0_wait_rint(SDC_RISR_CMD_DONE, 0xFFFFFFu) == 0
            && sdc0_read_data(probe, 512) == 0) {
            /* карта готова в конфигурации U-Boot (часто 4-bit/50МГц).
               Определяем SDSC/SDHC честно — CMD58 (READ_OCR, R3): CCS bit30. */
            g_sdhc = 1;
            if (sdc0_cmd(58, 0, SDC_CMDR_RESP_RCV) == 0)
                g_sdhc = (uint8_t)((SDC0_REG(SDC_RESP0) >> 30) & 1);
            if (info) {
                info->sdhc = g_sdhc;
                info->wide4 = 1;   /* у U-Boot обычно 4-bit; читаем как есть */
                info->total_sectors = 0;
            }
            uart_print("sd0: legacy (U-Boot) ok\r\n");
            SD0_LED_OFF();
            return 0;
        }
        uart_print("sd0: legacy probe fail, full init\r\n");
    }

    /* ==== Фаза B: полный подъём с нуля (FEL+SPL: U-Boot карту не трогал) ====
       1) Такт шины: gate ММС0. МОДУЛЬНЫЙ СБРОС В CCU (RST0 bit8) НЕ делаем:
          при загрузке с SD карту уже инициализировал U-Boot, а assert/deassert
          RST0 = глобальный для модуля сброс — он сбрасывает контроллер вместе
          с состоянием карты и конфликтует с загрузчиком.
          Вместо этого «убиваем зомби» после U-Boot локально: перезаписываем
          регистры контроллера (GCTL_RESET + HWRST ниже) — это безопасно и
          не трогает шину/карту глобально. */
    H3_CCU_SDMMC0_CLK = H3_SDMMC0_SCLK_GATE
                      | H3_SDMMC0_CLK_SRC_OSC24M
                      | (2u << 16)   /* N=2 */
                      | (15u << 0);  /* M=16 */
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    /* 3) Пины PC0..PC5 -> function 2 (SDC0). */
    h3_sdc0_init_pins();

    /* 4) Сброс контроллера + HWRST + такты карты + включение функций. */
    SDC0_REG(SDC_GCTL) = SDC_GCTL_RESET;
    for (uint32_t t = 0; t < 0xFFFFFFu; t++)
        if ((SDC0_REG(SDC_GCTL) & SDC_GCTL_RESET) == 0) break;

    SDC0_REG(SDC_HWRST) = 1u;
    SDC0_REG(SDC_HWRST) = 0u;
    udelay(1000);
    SDC0_REG(SDC_HWRST) = 1u;

    SDC0_REG(SDC_CKCR) = 0;
    SDC0_REG(SDC_CKCR) = SDC_CKCR_CCLK_ENB | SDC_CKCR_CCLK_CTRL
                       | (1u & SDC_CKCR_CCLK_MASK);   /* DIV=1 -> ~375 кГц */
    SDC0_REG(SDC_FUNS) = 0xceAAu;
    SDC0_REG(SDC_GCTL) = SDC_GCTL_INT_ENB | SDC_GCTL_CD_DBC;
    SDC0_REG(SDC_IMKR) = 0;
    SDC0_REG(SDC_RISR) = 0xFFFFFFFFu;
    SDC0_REG(SDC_TMOR) = 0xFFFFFF40u;
    SDC0_REG(SDC_CTYPE) = 0x0u;     /* 1-bit на время исходной инициализации */
    SDC0_REG(SDC_BLKSZ) = 512;
    __asm volatile("dsb" ::: "memory");

    /* 5) Инициализация карты. */
    if (sdc0_cmd(0, 0, SDC_CMDR_SEND_INIT) != 0) {        /* CMD0: 80 тактов, GO_IDLE */
        uart_print("sd0: CMD0 fail\r\n");
        SD0_LED_OFF();
        return -1;
    }
    if (sdc0_cmd(8, 0x000001AA, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) != 0) {
        /* SDSC-карты (v1) на CMD8 не отвечают — пропускаем (не фатально). */
    }
    /* ACMD41: CMD55 + CMD41 с HCS и поллингом busy (до 1 c). */
    uint32_t busy_wait = 0;
    while (busy_wait < 1000000u) {
        if (sdc0_cmd(55, g_rca << 16, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) != 0)
            { uart_print("sd0: CMD55 fail\r\n"); SD0_LED_OFF(); return -1; }
        uint32_t ocr_arg = 0x40FF8000u;   /* HCS + 3.2-3.4V */
        if (sdc0_cmd(41, ocr_arg, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) != 0)
            { uart_print("sd0: CMD41 fail\r\n"); SD0_LED_OFF(); return -1; }
        uint32_t ocr = SDC0_REG(SDC_RESP0);
        if (ocr & (1u << 31)) break;      /* busy bit set — карта готова */
        udelay(1000);
        busy_wait += 1000;
    }
    if (busy_wait >= 1000000u) { uart_print("sd0: ACMD41 timeout\r\n"); SD0_LED_OFF(); return -1; }

    /* CMD2 (CID), CMD3 (RCA) — ответы, RCA сохраняем. */
    if (sdc0_cmd(2, 0, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC | SDC_CMDR_LONG_RESP) != 0)
        { uart_print("sd0: CMD2 fail\r\n"); SD0_LED_OFF(); return -1; }
    if (sdc0_cmd(3, 0, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) != 0)
        { uart_print("sd0: CMD3 fail\r\n"); SD0_LED_OFF(); return -1; }
    g_rca = (SDC0_REG(SDC_RESP0) >> 16) & 0xFFFF;

    /* CMD7 (select card). */
    if (sdc0_cmd(7, g_rca << 16, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) != 0)
        { uart_print("sd0: CMD7 fail\r\n"); SD0_LED_OFF(); return -1; }

    /* CSD (CMD9) — размер карты. */
    uint32_t csd[4];
    if (sdc0_cmd(9, g_rca << 16, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC | SDC_CMDR_LONG_RESP) == 0) {
        for (int i = 0; i < 4; i++) csd[i] = SDC0_REG(SDC_RESP0 + i * 4);
        if (info) info->total_sectors = sdc0_parse_csd(csd);
    }

    /* ACMD6 (4-bit) + CMD16 (blocklen). */
    if (sdc0_cmd(55, g_rca << 16, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) == 0) {
        if (sdc0_cmd(6, 0x2, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) == 0) {
            SDC0_REG(SDC_CTYPE) = 0x1;    /* 4-bit */
            g_wide4 = 1;
        }
    }
    if (sdc0_cmd(16, 512, SDC_CMDR_RESP_RCV | SDC_CMDR_CHK_CRC) != 0)
        { uart_print("sd0: CMD16 fail\r\n"); SD0_LED_OFF(); return -1; }

    /* CMD58 (READ_OCR): CCS bit30 — SDSC (0) / SDHC (1). */
    if (sdc0_cmd(58, 0, SDC_CMDR_RESP_RCV) == 0) {
        g_sdhc = (uint8_t)((SDC0_REG(SDC_RESP0) >> 30) & 1);
    } else {
        uart_print("sd0: CMD58 fail (assume SDHC)\r\n");
        g_sdhc = 1;
    }

    uart_print("sd0: init ok (");
    uart_print(g_sdhc ? "SDHC" : "SDSC");
    uart_print(g_wide4 ? ", 4-bit, size=" : ", 1-bit, size=");
    if (info && info->total_sectors)
        uart_print_int((int)info->total_sectors);
    else
        uart_print("?");
    uart_print(" sectors)\r\n");
    if (info) {
        info->sdhc = g_sdhc;
        info->wide4 = g_wide4;
    }
    SD0_LED_OFF();
    return 0;
}