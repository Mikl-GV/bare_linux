#include "h3_sdc.h"
#include "h3.h"
#include "h3_ccu.h"
#include "ports.h"
#include "sys.h"

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
    udelay(100000);
}

void h3_sdc1_init(void)
{
    /* 1) Включить WiFi-чип. */
    wifi_power_on();

    /* 2) Такт шины: gate включить, полный цикл reset через CCU (assert -> deassert),
       чтобы контроллер гарантированно вышел из сброса (иначе star залипает). */
    H3_CCU_GATING0 |= H3_CCU_GATE_MMC1;
    __asm volatile("dsb" ::: "memory");
    H3_CCU_RST0 &= ~H3_CCU_RST_MMC1;   /* assert reset */
    __asm volatile("dsb" ::: "memory");
    udelay(10);
    H3_CCU_RST0 |= H3_CCU_RST_MMC1;    /* deassert reset */
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    /* 3) Модульный такт: OSC24M (новый режим), делитель для ~400 кГц.
      SCLK = source / N / M / 2 (new mode). Ставим N=1, M=30: 24M/2/30 = 400 кГц.
      Бит M_MAX=15, но здесь M это FIELD = m+1; используем M=30 -> поле 29 нельзя.
      Вместо этого: ставим источник OSC24M без деления (source/1) и делитель
      в CKCR = 30 -> CCLK = 24M/60 = 400 кГц. */
    H3_CCU_SDMMC1_CLK = H3_SDMMC1_SCLK_GATE | H3_SDMMC1_MODE_NEW
                      | H3_SDMMC1_CLK_SRC_OSC24M
                      | H3_SDMMC1_CLK_DIV_N(0)    /* N=1 */
                      | H3_SDMMC1_CLK_DIV_M(0);   /* M=1 -> потом CKCR добьёт */
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    /* 4) Пины PG0..PG5 в режим SDC1 (function 2).
          PG0..PG3 -> CFG0 (0x00), PG4..PG7 -> CFG1 (0x04), по 4 бита на пин. */
    uint32_t c0 = *(volatile uint32_t *)(PORTG + 0x00);
    c0 &= ~(0xFFFFu);                 /* очистить PG0..PG3 (16 бит) */
    c0 |=  (0x2222u);                 /* все function 2 (SDC1) */
    *(volatile uint32_t *)(PORTG + 0x00) = c0;

    uint32_t c1 = *(volatile uint32_t *)(PORTG + 0x04);
    c1 &= ~(0xFFu);                   /* очистить PG4..PG5 (8 бит) */
    c1 |=  (0x22u);                   /* PG4, PG5 function 2 */
    *(volatile uint32_t *)(PORTG + 0x04) = c1;

    __asm volatile("dsb" ::: "memory");

    /* 5) Сброс контроллера и FIFO. */
    SDC_REG(SDC_GCTL) = (1u << 0) | (1u << 1);   /* SOFT_RST + FIFO_RST */
    sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_GCTL), 1u << 0, 0);
    sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_GCTL), 1u << 1, 0);

    /* 6) Включить такт карты: CCLK_ENB + делитель.
          SCLK=24MHz (OSC), CCLK = SCLK/(2*30) = 400 kHz для инициализации. */
    SDC_REG(SDC_CKCR) = SDC_CKCR_CCLK_ENB | SDC_CKCR_CCLK_DIV(30);   /* /60 */
    __asm volatile("dsb" ::: "memory");

    /* Включить глобальное прерывание хоста (нужно для работы команд). */
    SDC_REG(SDC_GCTL) = (1u << 4);   /* INT_ENB */

    /* Снять маски прерываний (пока polling по raw status). */
    SDC_REG(SDC_IMKR) = 0x00000000u;
    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;

    /* Таймаут: большие значения. */
    SDC_REG(SDC_TMOR) = 0xFFFFFF40u;

    /* Ширина шины: 1-bit пока (SDC_CTYPE=0). Для RTL8189 включим 4-bit позже. */
    SDC_REG(SDC_CTYPE) = 0x0u;

    /* Блок по умолчанию 512 байт. */
    SDC_REG(SDC_BLKSZ) = 512u;

    __asm volatile("dsb" ::: "memory");
}

int h3_sdc_cmd_no_data_flags(uint32_t cmd, uint32_t arg, uint32_t resp_reg,
                             int has_resp, int check_crc)
{
    (void)resp_reg;
    /* Очистить статус перед командой. */
    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;

    SDC_REG(SDC_ARG) = arg;
    uint32_t cmdr = SDC_CMDR_START                  /* START - запуск команды */
                  | (cmd & SDC_CMDR_IDX_MASK)
                  | (has_resp ? SDC_CMDR_RESP_RCV : 0)
                  | (check_crc ? SDC_CMDR_CHK_CRC : 0);
    SDC_REG(SDC_CMDR) = cmdr;

    if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_RISR), SDC_RISR_CMD_DONE, 1) != 0)
        return -1;
    if (SDC_REG(SDC_RISR) & SDC_RISR_RESP_ERR)
        return -2;
    SDC_REG(SDC_RISR) = SDC_RISR_CMD_DONE | SDC_RISR_RESP_ERR;
    return 0;
}

int h3_sdc_cmd_no_data(uint32_t cmd, uint32_t arg, uint32_t resp_reg)
{
    return h3_sdc_cmd_no_data_flags(cmd, arg, resp_reg, 1, 1);
}

/* SDIO CMD52 — байтовый доступ к регистру функции чипа.
 * Формат аргумента (SDIO spec):
 *   bit31    = write flag (1=write, 0=read)
 *   bits30:28= 0
 *   bit27    = 0 (data 0)
 *   bit26    = 0 (RAW)
 *   bits25:9 = register address
 *   bits8:7  = function number
 *   bits6:0  = write data (для чтения не используется)
 * Ответ (R5): bits[7:0] = read data.
 */
int h3_sdc_cmd52_read(uint32_t func, uint32_t reg, uint8_t *out)
{
    uint32_t arg = (0u << 31) | ((reg & 0x1FFFF) << 9) | ((func & 3) << 7);
    int rc = h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
    if (rc != 0) return rc;
    *out = (uint8_t)(SDC_REG(SDC_RESP0) & 0xFF);
    return 0;
}

int h3_sdc_cmd52_write(uint32_t func, uint32_t reg, uint8_t val)
{
    uint32_t arg = (1u << 31) | ((reg & 0x1FFFF) << 9) | ((func & 3) << 7)
                 | (val & 0x7F);
    return h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
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
        if (rc != 0) return rc;
        uint32_t resp = SDC_REG(SDC_RESP0);
        if (resp & (1u << 31))   /* C=1 - карта готова */
            break;
        udelay(10000);
    }
    if (tries >= 100) return -3;   /* таймаут CMD5 */

    /* 3) CMD3 (SEND_RELATIVE_ADDR): получить RCA. */
    rc = h3_sdc_cmd_no_data_flags(3, 0, 0, 1, 0);
    if (rc != 0) return -4;
    rca = (SDC_REG(SDC_RESP0) >> 16) & 0xFFFF;

    /* 4) CMD7 (SELECT_CARD): выбрать карту. */
    rc = h3_sdc_cmd_no_data_flags(7, rca << 16, 0, 1, 0);
    if (rc != 0) return -6;

    return 0;
}

int h3_sdc_block_transfer(uint32_t cmd, uint32_t arg, uint32_t blksz,
                          uint32_t blkcnt, int write, void *data)
{
    volatile uint32_t *fifo = (volatile uint32_t *)(SDC_BASE + SDC_FIFO);
    const uint32_t *in  = (const uint32_t *)data;
    uint32_t *out = (uint32_t *)data;
    uint32_t words = (blksz * blkcnt) / 4;
    uint32_t i;

    SDC_REG(SDC_RISR) = 0xFFFFFFFFu;

    SDC_REG(SDC_BLKSZ) = blksz;
    SDC_REG(SDC_BYCNT) = blksz * blkcnt;
    SDC_REG(SDC_ARG)   = arg;

    /* Карта FIFO: база + 0x80, 32-бит слова (burst). Доступ по возрастающему
       адресу (0,4,8,...), FIFO сам управляет глубиной 16 слов. */

    uint32_t cmdr = (cmd & SDC_CMDR_IDX_MASK)
                  | SDC_CMDR_RESP_RCV
                  | SDC_CMDR_CHK_CRC
                  | SDC_CMDR_DATA_TRANS
                  | (write ? SDC_CMDR_TRANS_DIR : 0);
    SDC_REG(SDC_CMDR) = cmdr;

    if (write) {
        /* Запись: ждём TX_REQ и пишем в FIFO последовательно с 0. */
        for (i = 0; i < words; i++) {
            if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_RISR), SDC_RISR_TX_REQ, 1) != 0)
                return -1;
            SDC_REG(SDC_RISR) = SDC_RISR_TX_REQ;
            fifo[i & 15] = in[i];   /* FIFO-отображение на 16 слов (0x80..0xBC) */
        }
    } else {
        /* Чтение: ждём RX_REQ и читаем FIFO с 0. */
        for (i = 0; i < words; i++) {
            if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_RISR), SDC_RISR_RX_REQ, 1) != 0)
                return -1;
            SDC_REG(SDC_RISR) = SDC_RISR_RX_REQ;
            out[i] = fifo[i & 15];
        }
    }

    /* Ждать завершения передачи данных. */
    if (sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_RISR), SDC_RISR_DATA_DONE, 1) != 0)
        return -1;

    if (SDC_REG(SDC_RISR) & SDC_RISR_RESP_ERR)
        return -2;

    SDC_REG(SDC_RISR) = SDC_RISR_DATA_DONE | SDC_RISR_RESP_ERR;
    return 0;
}