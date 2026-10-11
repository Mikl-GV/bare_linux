#include "h3_sdc.h"
#include "h3.h"
#include "h3_ccu.h"
#include "ports.h"
#include "sys.h"

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
/* Диагностика IDMA. */
uint32_t sdc_diag_idst = 0;   /* IDMA статус при ошибке */
uint32_t sdc_diag_dmac = 0;   /* DMAC при ошибке */
uint32_t sdc_diag_idma_used = 0;   /* счётчик: сколько передач через IDMA */
uint32_t sdc_diag_pio_used  = 0;   /* счётчик: сколько через PIO */
uint32_t sdc_diag_fw_mcufwdl = 0;  /* REG_MCUFWDL при падении FreeToGo */

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
          SCLK = OSC24M / N / M = 24M/2/16 ~= 750 кГц (для init).
          Затем делитель в CKC даст нужный CCLK. */
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
    __asm volatile("dsb" ::: "memory");

    /* 5) Сброс контроллера: GCTL_RESET (SOFT+FIFO+DMA), ждать автсброса. */
    SDC_REG(SDC_GCTL) = SDC_GCTL_RESET;
    sdc_wait((volatile uint32_t *)(SDC_BASE + SDC_GCTL), SDC_GCTL_RESET, 0);

    /* 6) HWRST: вывод контроллера из reset (по референсу boot0). */
    SDC_REG(SDC_HWRST) = 1u;   /* active */
    SDC_REG(SDC_HWRST) = 0u;   /* reset */
    udelay(1);
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

    /* 9) Прочее: INT_ENB, таймаут, 1-bit шина, блок 512. */
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
    if (rc != 0) return rc;
    *out = (uint8_t)(SDC_REG(SDC_RESP0) & 0xFF);
    return 0;
}

int h3_sdc_cmd52_write(uint32_t func, uint32_t reg, uint8_t val)
{
    uint32_t arg = (1u << 31) | ((func & 7u) << 28) | ((reg & 0x1FFFF) << 9)
                 | (val & 0x7F);
    return h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
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
    return h3_sdc_block_transfer(53, arg, len, 1, 1, (void *)data);
}

/* Чтение 32-бит регистра WLAN-функции (function 1) чипа через CMD52.
   Realtek отображает ММIO-регистры чипа в «WLAN IOREG»: SDIO-адрес =
   (device_id 8 << 13) | (reg & 0x7FFF). Повторяем sdio_read32() драйвера:
   3 байта через CMD52 (младший первым). */
int h3_sdc_read_wlan_reg32(uint32_t reg, uint32_t *val)
{
    uint32_t ftaddr = (8u << 13) | (reg & 0x7FFFu);
    uint8_t b0, b1, b2;
    int rc;
    if ((rc = h3_sdc_cmd52_read(1, ftaddr, &b0)) != 0) return rc;
    if ((rc = h3_sdc_cmd52_read(1, ftaddr + 1, &b1)) != 0) return rc;
    if ((rc = h3_sdc_cmd52_read(1, ftaddr + 2, &b2)) != 0) return rc;
    *val = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
    return 0;
}

/* Чтение 16-бит регистра WLAN-функции (function 1) через CMD52. */
int h3_sdc_read_wlan_reg16(uint32_t reg, uint16_t *val)
{
    uint32_t ftaddr = (8u << 13) | (reg & 0x7FFFu);
    uint8_t b0, b1;
    int rc;
    if ((rc = h3_sdc_cmd52_read(1, ftaddr, &b0)) != 0) return rc;
    if ((rc = h3_sdc_cmd52_read(1, ftaddr + 1, &b1)) != 0) return rc;
    *val = (uint16_t)(b0 | ((uint16_t)b1 << 8));
    return 0;
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

/* Полный power-on чипа: тянет карту из CARDEMU в ACTIVE. */
int h3_wifi_power_on_chip(void)
{
    /* rtl8188F_card_enable_flow = CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT + END.
       Вручную раскрываем две последовательности из Hal8188FPwrSeq.h. */
    /* CARDEMU_TO_ACT (из макроса RTL8188F_TRANS_CARDEMU_TO_ACT).
   Поллинги заменены на простые WRITE: на стенде регистры 0x06/0x05 не дают
   ожидаемых битов готовности, поллинг зависал; write-шаги проходят. */
    static const uint8_t seq_ce2ac[] = {
        /* 0x0005 MAC WRITE BIT2 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x04, 0x00,
        /* 0x0006 MAC WRITE BIT1 BIT1 (ранее POLLING) */
        0x06,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x02, 0x02,
        /* 0x0005 MAC WRITE BIT7 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x80, 0x00,
        /* 0x0005 MAC WRITE BIT3 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x08, 0x00,
        /* 0x0005 MAC WRITE BIT0 BIT0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x01, 0x01,
        /* 0x0005 MAC WRITE BIT0 0 (ранее POLLING) */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x01, 0x00,
        /* 0x0027 MAC WRITE 0xff 0x35 */
        0x27,0x00, 0xFF, (0<<4)|0x00, 0x01, 0xFF, 0x35,
        /* END */
        0xFF,0xFF, 0xFF, (0<<4)|0x00, 0x04, 0x00, 0x00,
    };

    /* Чип уже вне suspend (sdio86=0x00), поэтому CARDDIS_TO_CARDEMU с его
       поллингом «ждать suspend» не сходится. Запускаем сразу CARDEMU_TO_ACT. */
    return pseq_exec(seq_ce2ac, sizeof(seq_ce2ac)/7);
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
int h3_wifi_download_fw(const void *fw, uint32_t size)
{
    const uint8_t *b = (const uint8_t *)fw;
    uint32_t i;
    int rc;

    if (size < 4) return -9;
    /* Проверка сигнатуры: 0xF1 0x88 (чип 8188F), + 0x10 0x00. */
    if (b[0] != 0xF1 || b[1] != 0x88) return -10;

    /* RTL8189F NIC-прошивка содержит 32-байтный заголовок
       (RT_8188F_FIRMWARE_HDR). Драйвер его пропускает перед загрузкой
       (rtl8188f_hal_init.c: IS_FW_HEADER_EXIST_8188F -> pFirmwareBuf += 32):
       в IRAM 8051 грузится ТОЛЬКО код, заголовок не пишется. */
    if (size > 32 && ((b[0] | ((uint16_t)b[1] << 8)) & 0xFFF0) == 0x88F0) {
        b += 32;
        size -= 32;
    }

    /* 0) Для загрузки прошивки переключаемся на 1-bit шину (данные по D0).
        Блок size Fn1 ставим 512 (как в драйвере sdio_intf.c: sdio_set_block_size
        (func, 512)); он нужен для блок-режима данных, но сама прошивка
        пишется в byte mode, как rtw_write32 -> sdio_writel. */
    h3_sdio_set_bus_width(0);
    udelay(1000);
    if ((rc = h3_sdc_set_fn1_blocksize(512)) != 0) return -8;

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

    /* 2) Взвести FWDL_ChkSum_rpt (0x80 |= BIT2) ДО записи FW — как драйвер
       (rtl8188f_hal_init.c:1251). Нужен для чексуммы при загрузке. */
    {
        uint8_t t;
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -7;
        WREG8(REG_MCUFWDL_OFF, (uint8_t)(t | FWDL_ChkSum_rpt));
    }

    /* 3) Записать прошивку постранично в byte mode (как драйвер: rtw_writeN /
       sdio_writel — одно 4-байтное CMD53 на слово). Каждая страница = 4096
       байт (8*512), переключается через REG_MCUFWDL+2[2:0]. Byte-режим
       гарантирует приём RTL boot ROM, который ждёт именно byte-mode CMD53. */
    {
        uint32_t page = 0;
        uint32_t off = 0;
        /* Драйвер пишет 4-байтными словами, 64-битное выравнивание гигиенично. */
        while (off < size) {
            uint32_t page_end = off + 4096;   /* граница текущей страницы */
            if (page_end > size) page_end = size;

            /* Выбрать страницу: REG_MCUFWDL+2[2:0] = page. */
            uint8_t t;
            if (RREG8(REG_MCUFWDL_OFF + 2, &t)) return -6;
            t = (uint8_t)((t & 0xF8) | (page & 0x07));
            WREG8(REG_MCUFWDL_OFF + 2, t);

            /* Записать страницу байтовыми CMD53 (по 4 байта), как rtw_write32:
               каждая страница пишется по одному базовому адресу FW_START
               (page-select в REG_MCUFWDL+2 перенаправляет в нужную RMAC/IROM
               страницу); внутри страницы адрес автоинкрементируется по 4. */
            uint32_t ftaddr = (8u << 13) | (FW_8188F_START & 0x7FFFu);
            uint32_t n4 = (page_end - off) / 4;   /* сколько полных слов */
            uint32_t r4 = (page_end - off) % 4;   /* остаток байт */
            for (uint32_t w = 0; w < n4; w++) {
                rc = h3_sdc_cmd53_write_bytes(1, ftaddr + w * 4, 4,
                                              (const void *)(b + off + w * 4));
                if (rc != 0) {
                    sdc_diag_fail_blk = (int)((off + w * 4) / 512);
                    return -40 - rc;
                }
            }
            if (r4) {   /* хвост: дополняем нулями до слова */
                uint8_t tail[4] = {0, 0, 0, 0};
                for (uint32_t k = 0; k < r4; k++) tail[k] = b[off + n4 * 4 + k];
                rc = h3_sdc_cmd53_write_bytes(1, ftaddr + n4 * 4, 4, tail);
                if (rc != 0) {
                    sdc_diag_fail_blk = (int)((off + n4 * 4) / 512);
                    return -40 - rc;
                }
            }

            udelay(5000);
            off = page_end;
            page++;
        }
    }

    /* 4) _FWDownloadEnable(FALSE): снять FWDL (0x80 &= ~BIT0) до старта FW. */
    {
        uint8_t t;
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -40;
        WREG8(REG_MCUFWDL_OFF, (uint8_t)(t & 0xFE));
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
            return -33;
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
    }

    /* Ожидание завершения фазы данных. Ключевое правило: для всех DAT-команд
       (CMD53 и пр.) завершение определяется по DATA_DONE (TRANS_COMPLETE), а
       НЕ по CMD_DONE. CMD_DONE взводится после цикла команда/ответа и совсем
       не означает, что данные переданы на шину.
       ВАЖНО (прежний баг): нельзя определять "no-data" по биту 9 аргумента —
         if (status & CMD_DONE && !(arg & (1<<9))) break;
       Bit 9 — часть адресного поля bits[25:9] SDIO-команды, для выровненного
       адреса (0x1000) он всегда 0 -> условие всегда истинно, ожидание выходило
       по CMD_DONE до конца фазы данных, FIFO оставался полон, данные в чип не
       уходили; итог rc=-38. byte-mode CMD53 тоже участвует в фазе данных и
       завершается по DATA_DONE. */
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
int h3_sdio_read_cis(uint16_t *manf_id, uint32_t *cis_addr_out)
{
    int rc;
    uint8_t v;
    uint32_t addr;
    uint8_t b0, b1;

    /* Чтение байта из CIS по CMD52. */
    int rd8(uint32_t a, uint8_t *o) {
        int e = h3_sdc_cmd52_read(0, a & 0x1FFFF, o);
        return e;
    }

    /* CCCR 0x09-0x0B: адрес Common CIS (LSB сперва). */
    if ((rc = rd8(0x09, &b0)) != 0) return rc;
    if ((rc = rd8(0x0A, &b1)) != 0) return rc;
    if ((rc = rd8(0x0B, &v))  != 0) return rc;
    uint32_t cis_addr = (uint32_t)v << 16 | (uint32_t)b1 << 8 | b0;
    if (cis_addr_out) *cis_addr_out = cis_addr;

    if (cis_addr == 0) return -5;   /* CIS не задан */

    /* Идём по тюплам (лимит 64 тюпла / 512 байт для защиты). */
    uint16_t manf = 0;
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
        if (id == 0x20) {               /* TPLID_MANFID */
            uint8_t m0, m1;
            if ((rc = rd8(addr, &m0)) != 0) return rc;
            if ((rc = rd8(addr + 1, &m1)) != 0) return rc;
            manf = (uint16_t)(m1 << 8 | m0);
            found = 1;
            break;
        }
        /* Пропустить данные тюпла. */
        addr += (uint32_t)len;
    }

    if (manf_id) *manf_id = manf;
    return found ? 0 : -5;
}

/* Переключение шины SDIO в 4-bit режим.
 *
 * 1) CCCR offset 0x07 (Bus Interface Control) → биты [6:0] = 0b100 (4-bit).
 * 2) Аппаратно: SDC_CTYPE (0x0C) = 1 — шина 4-bit.
 *
 * Примечание: данные-пины PG4/PG5 уже настроены на function 2 в
 * h3_sdc1_init(); должен работать и 4-bit вариант.
 * Возвращает 0 при успехе; не-0 — ошибка. */
int h3_sdio_set_bus_width(int four_bit)
{
    int rc;
    uint8_t v;

    if ((rc = h3_sdc_cmd52_read(0, 0x07, &v)) != 0) return rc;
    /* Сохранить прочие биты (высокоскоростной режим и пр.), обновить только
       поле ширины [2:0]: 0=1-bit, 2=4-bit. */
    uint8_t nv = (uint8_t)((v & ~0x07u) | (four_bit ? 0x02u : 0x00u));
    if ((rc = h3_sdc_cmd52_write(0, 0x07, nv)) != 0) return rc;

    /* Аппаратная ширина шины контроллера: 0=1-bit, 1=4-bit. */
    SDC_REG(SDC_CTYPE) = four_bit ? 0x1u : 0x0u;
    __asm volatile("dsb" ::: "memory");
    return 0;
}