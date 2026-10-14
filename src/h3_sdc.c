#include "h3_sdc.h"
#include "h3.h"
#include "h3_ccu.h"
#include "ports.h"
#include "sys.h"
#include "uart.h"
#include "bb_phy_reg.h"
#include "rf_radioa.h"

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
                 | (val & 0xFF);
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
    return h3_sdc_block_transfer(53, arg, len, 1, 0, data);
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
    return h3_sdc_block_transfer(53, arg, len, 1, 1, (void *)data);
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
    return h3_sdc_block_transfer(53, arg, len, 1, 0, data);
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

        uart_print("  pseq#");
        uart_print_int(i);
        uart_print(" b=");
        uart_print_int(base);
        uart_print(" o=0x");
        uart_print_hex((uint32_t)off);
        uart_print(" c=");
        uart_print_int(cmd);
        uart_print(" m=0x");
        uart_print_hex((uint32_t)msk);
        uart_print(" v=0x");
        uart_print_hex((uint32_t)val);
        uart_print("\r\n");

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
    uart_print("  pwon: CARDEMU->ACTIVE start\r\n");
    /* rtl8188F_card_enable_flow = CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT + END.
       Вручную раскрываем две последовательности из Hal8188FPwrSeq.h. */
    /* CARDEMU_TO_ACT (из макроса RTL8188F_TRANS_CARDEMU_TO_ACT).
   Поллинги заменены на простые WRITE: на стенде регистры 0x06/0x05 не дают
   ожидаемых битов готовности, поллинг зависал; write-шаги проходят. */
    static const uint8_t seq_ce2ac[] = {
        /* 0x0005 MAC WRITE BIT2 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x04, 0x00,
        /* 0x0006 MAC WRITE BIT1 BIT1 */
        0x06,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x02, 0x02,
        /* 0x0005 MAC WRITE BIT7 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x80, 0x00,
        /* 0x0005 MAC WRITE BIT3 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x08, 0x00,
        /* 0x0005 MAC WRITE BIT0 BIT0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x01, 0x01,
        /* 0x0005 MAC WRITE BIT0 0 */
        0x05,0x00, 0xFF, (0<<4)|0x00, 0x01, 0x01, 0x00,
        /* 0x0027 MAC WRITE 0xff 0x35 */
        0x27,0x00, 0xFF, (0<<4)|0x00, 0x01, 0xFF, 0x35,
        /* END */
        0xFF,0xFF, 0xFF, (0<<4)|0x00, 0x04, 0x00, 0x00,
    };

    /* Чип уже вне suspend (sdio86=0x00), поэтому CARDDIS_TO_CARDEMU с его
       поллингом «ждать suspend» не сходится. Запускаем сразу CARDEMU_TO_ACT. */
    {
        int rp = pseq_exec(seq_ce2ac, sizeof(seq_ce2ac)/7);
        uart_print("  pwon rc=");
        uart_print_int(rp);
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
 * Последовательность из драйвера:
 *  1) REG_TXPAUSE(0x522) = 0xFF — блокировать TX-очереди (чип не передаёт);
 *  2) RF 0xB0 = 0xDFBF0 (LDO ON);
 *  3) RF 0x00 = 0 — перевести RF в LCK-режим (как в драйвере перед триггером);
 *  4) RF_CHNLBW (0x18) |= 0x08000 (бит 15 = LCK trigger);
 *  5) опрос: ждать сброса bit15 (LCK завершён), таймаут ~1 с (100 x 10 мс);
 *  6) RF 0xB0 = 0xDFFF0 (LDO OFF);
 *  7) восстановить канал в RF_CHNLBW[7:0];
 *  8) восстановить RF 0x00 = 0x30000 (штатный дефолт; LCK оставил его 0,
 *     что ломает тракт и заставляет IQK выдавать маркеры сбоя 0x142/0x42);
 *  9) REG_TXPAUSE = 0x00 — снять блокировку. */
void h3_wifi_rf_lck(void)
{
    uint32_t lc, ch;

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
        uart_print("  lck: sf_pre=0x"); uart_print_hex(sf16); uart_print("\r\n");
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
        uart_print("  lck: sf8=0x"); uart_print_hex(sf8); uart_print("\r\n");
        h3_sdc_write_wlan_reg32(0x0100, 0x0000063Fu);
        udelay(100);
        {
            unsigned int k2;
            for (k2 = 0; k2 < sizeof(bb_phy_reg)/sizeof(bb_phy_reg[0]); k2 += 2)
                h3_sdc_write_wlan_reg32(bb_phy_reg[k2], bb_phy_reg[k2+1]);
            udelay(100);
            uart_print("  lck: bb_config loaded (");
            uart_print_int((int)(sizeof(bb_phy_reg)/sizeof(bb_phy_reg[0])/2));
            uart_print(" regs)\r\n");
        }
    }
    /* Проверка живости BB/RF (0xE30 rTx_IQK_Tone_A пишем и читаем; RF 0x18
       через LSSI — должен вернуть не 0). */
    {
        uint32_t tst = 0;
        h3_sdc_write_wlan_reg32(0x0E30, 0x18008C1Cu);
        udelay(10);
        h3_sdc_read_wlan_reg32(0x0E30, &tst);
        uart_print("  lck: bb_tone=0x");
        uart_print_hex(tst);
        uart_print(" rf18=0x");
        uart_print_hex(rf_read(0x18));
        uart_print("\r\n");
    }
    /* 1) блокировать TX. */
    h3_sdc_write_wlan_reg8(0x0522, 0xFF);
    /* 2) Выставить канал 1 (2412 МГц) в RF_CHNLBW[7:0] — LCK калибрует
          утечку гетеродина на текущей частоте; канал 0 = «ничто». */
    ch = rf_read(0x18);
    uart_print("  lck: chnlbw_pre=0x");
    uart_print_hex(ch);
    uart_print("\r\n");
    ch = (ch & ~0xFFu) | 0x01u;          /* канал 1 */
    rf_write(0x18, ch);
    udelay(100);
    /* Лог после установки канала (ожидаем биты [7:0]=1). */
    ch = rf_read(0x18);
    uart_print("  lck: chnlbw_set=0x");
    uart_print_hex(ch);
    uart_print("\r\n");
    /* 3) LDO ON. */
    rf_write(0xB0, 0xDFBF0);
    /* 4) подготовка: RF 0x00 = 0 — перевести RF в LCK-режим. */
    rf_write(0x00, 0x00000);
    udelay(50);
    lc = rf_read(0x00);
    uart_print("  lck: rf00=0x");
    uart_print_hex(lc);
    uart_print("\r\n");
    /* 5) триггер LCK: bit15 = 1. */
    lc = rf_read(0x18);
    rf_write(0x18, lc | 0x08000);
    uart_print("  lck: chnlbw_trig=0x");
    uart_print_hex(lc | 0x08000);
    uart_print("\r\n");
    /* 6) опрос: ждать сброс bit15 (LCK завершён), таймаут ~1 с (100 x 10 мс). */
    {
        unsigned int t = 0;
        lc = rf_read(0x18);
        while ((lc & 0x08000u) && (++t < 100)) {
            udelay(10000);
            lc = rf_read(0x18);
        }
        if (lc & 0x08000u) {
            uart_print("  lck: TIMEOUT after ");
            uart_print_int((int)(t * 10));
            uart_print(" ms\r\n");
        } else {
            uart_print("  lck: done in ");
            uart_print_int((int)(t * 10));
            uart_print(" ms\r\n");
        }
        uart_print("  lck: chnlbw_done=0x");
        uart_print_hex(lc);
        uart_print("\r\n");
    }
    /* 7) LDO OFF. */
    rf_write(0xB0, 0xDFFF0);
    /* 8) Восстановить канал: LCK затирает биты [7:0]. */
    lc = rf_read(0x18);
    rf_write(0x18, (lc & ~0xFFu) | 0x01u);
    udelay(100);
    lc = rf_read(0x18);
    uart_print("  lck: chnlbw_restored=0x");
    uart_print_hex(lc);
    uart_print("\r\n");
    /* 9) Восстановить RF 0x00: LCK перевёл его в специальный режим (0),
          для нормальной работы тракта и IQK нужен штатный дефолт 0x30000
          (первая запись rf_radioa.h). Без этого IQK даёт насыщение
          0x142/0x42 (маркеры сбоя из драйвера). */
    rf_write(0x00, 0x30000);
    udelay(100);
    lc = rf_read(0x00);
    uart_print("  lck: rf00_restored=0x");
    uart_print_hex(lc);
    uart_print("\r\n");
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
    unsigned int i;

    uart_print("  iqk: start\r\n");

    /* Диагностика/гарантия: RF 0x00 должен быть 0x30000 (штатный дефолт).
       Если LCK оставил 0 — исправляем: без этого IQK даёт насыщение. */
    v = rf_read(0x00);
    uart_print("  iqk: rf00_pre=0x");
    uart_print_hex(v);
    uart_print("\r\n");
    if (v != 0x30000u) {
        rf_write(0x00, 0x30000);
        udelay(100);
        v = rf_read(0x00);
        uart_print("  iqk: rf00_fix=0x");
        uart_print_hex(v);
        uart_print("\r\n");
    }

    /* RF_WE_LUT (0xEF) bit19=1 ставится ВНУТРИ каждой фазы Tx/Rx —
       RF-таблица базового блока заканчивается записью 0xEF=0x1A0,
       которая сбрасывает бит 19 (write-enable LUT). Если ставить его
       до таблицы, защищённые регистры (0x30/0x31/0x32/0xDF/0x56)
       «пишутся» только в shadow и не применяются к аналоговому тракту,
       из-за чего IQK даёт насыщение 0x142/0x42. */

    /* H2C-обвязка до IQK (sdio_halinit.c:1275-1293): firmware координирует
       калибровку — отправить H2C 0x6D={1} (H2C_8188F_BT_WLAN_CALIBRATION),
       дождаться, что FW приняла кадр (0x1CC bit0=0), затем поллить 0x1E7
       bit0 (FW выполнила свою часть), но НЕ выключать 8051 — иначе FW
       не обработает H2C. */
    {
        uint8_t en = 0;
        uint8_t hm = 0;
        uint32_t h2c_cmd = 0;

        /* 8051 включён? (FEN_CPUEN = 0x03 bit2). */
        h3_sdc_read_wlan_reg8(0x03, &en);
        uart_print("  iqk: mcu_pre=0x"); uart_print_hex((uint32_t)en); uart_print("\r\n");

        /* Отправить H2C box0: [0]=ElementID 0x6D, [1..]=data {0x01}. */
        h3_sdc_write_wlan_reg8(0x01CC, 0x00);        /* HMETFR: сбросить «FW ещё читает» */
        h2c_cmd = 0u | (0x01u << 8) | (0x6Du << 0);  /* le32: [0]=0x6D, [1]=0x01 */
        h3_sdc_write_wlan_reg32(0x01D0, h2c_cmd);    /* HMEBOX0 */
        /* Ждать, пока FW заберёт (0x1CC bit0 взведётся как «читает», затем
           спадёт когда закончит) — как _is_fw_read_cmd_down: valid=bit0,
           ждём valid==0. */
        {
            unsigned int t = 0;
            do {
                udelay(1000);
                h3_sdc_read_wlan_reg8(0x01CC, &hm);
            } while ((hm & 0x01u) && (++t < 100));
            uart_print("  iqk: h2c_sent 0x6D={1} 1cc=0x");
            uart_print_hex((uint32_t)hm);
            uart_print("\r\n");
        }
        /* Поллить 0x1E7 bit0 (FW сообщила о готовности к калибровке). */
        {
            unsigned int t = 0;
            uint8_t s = 0;
            do {
                udelay(10000);
                h3_sdc_read_wlan_reg8(0x01E7, &s);
            } while (!(s & 0x01u) && (++t < 40));    /* ~400 мс, как драйвер */
            uart_print("  iqk: cal_ready 1e7=0x");
            uart_print_hex((uint32_t)s);
            uart_print("\r\n");
        }
    }

    /* TXPAUSE: заблокировать TX-очереди на время калибровки. */
    h3_sdc_write_wlan_reg8(0x0522, 0xFF);

    /* BB-подготовка тракта (phy_IQCalibrate_8188F, до циклов):
       RFMOD bit24=0, путь, мультиплексор, RF-интерфейс. */
    h3_sdc_read_wlan_reg32(0x0800, &v);
    h3_sdc_write_wlan_reg32(0x0800, v & ~(1u << 24));
    h3_sdc_write_wlan_reg32(0x0C04, 0x03A05600);   /* rOFDM0_TRxPathEnable */
    h3_sdc_write_wlan_reg32(0x0C08, 0x000800E4);   /* rOFDM0_TRMuxPar */
    h3_sdc_write_wlan_reg32(0x0874, 0x25204000);   /* rFPGA0_XCD_RFInterfaceSW */

    /* RF_CHNLBW = 0x8C01 — как драйвер держит перед IQK (phy_LCK_8188F:
       канал 1, BW, бит 15 = mode-калибровка, rtl8188f_phycfg.c:748).
       У нас после LCK 0x401 — без бита 15 и других бит, чем отличается
       состояние RF-тракта от драйверного перед калибровкой. */
    rf_write(0x18, 0x8C01);
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

    /* RF-таблица (Array_RadioA_8188F): грузим ТОЛЬКО базовый блок
       (первые 36 пар, до канальных/условных секций 0x051+), чтобы не
       перезаписать канал и не поднять бит 15 (0xFC07 из хвоста таблицы).
       После загрузки восстанавливаем канал 1 в RF_CHNLBW[7:0]. */
    {
        unsigned int k, n;
        uint32_t rf18;
        n = sizeof(rf_radioa) / sizeof(rf_radioa[0]);
        if (n / 2 > 36) n = 36 * 2;
        for (k = 0; k + 1 < n; k += 2) {
            rf_write((uint8_t)(rf_radioa[k] & 0xFF), rf_radioa[k + 1]);
            udelay(1);
        }
        rf18 = rf_read(0x18);
        rf18 = (rf18 & ~0xFFu) | 0x01u;  /* канал 1, снять бит 15 */
        rf_write(0x18, rf18);
        udelay(50);
        uart_print("  iqk: rf_config loaded (");
        uart_print_int((int)(n / 2));
        uart_print(" regs) rf18=0x");
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
        uart_print("  iqk: rf30=0x"); uart_print_hex(rf_read(0x30));
        uart_print(" rf31=0x"); uart_print_hex(rf_read(0x31));
        uart_print(" rf32=0x"); uart_print_hex(rf_read(0x32));
        uart_print(" rfdf=0x"); uart_print_hex(rf_read(0xDF));
        uart_print(" rf56=0x"); uart_print_hex(rf_read(0x56));
        uart_print(" rfEF=0x"); uart_print_hex(rf_read(0xEF));
        uart_print("\r\n");
        /* 0x948 (Path_SEL_BB): должен быть 0x00000080 (Path A ADDA включён). */
        {
            uint32_t d;
            h3_sdc_read_wlan_reg32(0x0948, &d);
            uart_print("  iqk: sel948=0x"); uart_print_hex(d); uart_print("\r\n");
        }
        h3_sdc_write_wlan_reg32(0x0948, 0x00000080);   /* Path A */

        /* e94/e9c/eac ДО триггера — убедиться, что они обновляются
           движком, а не висят на дефолте. */
        {
            uint32_t d;
            h3_sdc_read_wlan_reg32(0x0E94, &d);
            uart_print("  iqk: pre e94=0x"); uart_print_hex(d);
            h3_sdc_read_wlan_reg32(0x0E9C, &d);
            uart_print(" e9c=0x"); uart_print_hex(d);
            h3_sdc_read_wlan_reg32(0x0EAC, &d);
            uart_print(" eac=0x"); uart_print_hex(d);
            uart_print("\r\n");
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
            uart_print("  iqk: e28=0x"); uart_print_hex(rb);
            h3_sdc_read_wlan_reg32(rTx_IQK_Tone_A, &rb);
            uart_print(" e30=0x"); uart_print_hex(rb);
            h3_sdc_read_wlan_reg32(rRx_IQK_Tone_A, &rb);
            uart_print(" e34=0x"); uart_print_hex(rb);
            h3_sdc_read_wlan_reg32(rTx_IQK_PI_A, &rb);
            uart_print(" e38=0x"); uart_print_hex(rb);
            h3_sdc_read_wlan_reg32(rIQK_AGC_Rsp, &rb);
            uart_print(" e4c=0x"); uart_print_hex(rb);
            h3_sdc_read_wlan_reg32(rIQK_AGC_Pts, &rb);
            uart_print(" e48=0x"); uart_print_hex(rb);
            uart_print("\r\n");
        }

        /* one shot: LOK & IQK. */
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF9000000);
        h3_sdc_write_wlan_reg32(rIQK_AGC_Pts, 0xF8000000);
        udelay(25000);                  /* IQK_DELAY_TIME_8188F = 25 мс */

        /* reload RF 0xdf и выход из IQK-режима. */
        h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
        rf_write(0xDF, 0x00180);
        (void)rf_read(0x08);            /* LOK_Result (не сохраняем) */

        h3_sdc_read_wlan_reg32(rTxPwBefore, &e94);
        h3_sdc_read_wlan_reg32(rTxPwAfter,  &e9c);
        h3_sdc_read_wlan_reg32(rRxPwAfter,  &eac);
        uart_print("  iqk: tx e94=0x"); uart_print_hex(e94);
        uart_print(" e9c=0x"); uart_print_hex(e9c);
        uart_print(" eac=0x"); uart_print_hex(eac);
        uart_print("\r\n");
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

        h3_sdc_read_wlan_reg32(rRxPwAfter, &eac);
        h3_sdc_read_wlan_reg32(rTxPwBefore, &e94);
        h3_sdc_read_wlan_reg32(rTxPwAfter, &e9c);
        uart_print("  iqk: rx1 eac=0x"); uart_print_hex(eac);
        uart_print(" e94=0x"); uart_print_hex(e94);
        uart_print(" e9c=0x"); uart_print_hex(e9c);
        uart_print("\r\n");
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

        h3_sdc_read_wlan_reg32(rRxPwAfter,  &eac);
        h3_sdc_read_wlan_reg32(rRxPwBefore, &ea4);
        uart_print("  iqk: rx2 eac=0x"); uart_print_hex(eac);
        uart_print(" ea4=0x"); uart_print_hex(ea4);
        uart_print("\r\n");
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
       после калибровки): 0xe28=0, 0xe30/0xe34=0x01008C00. */
    h3_sdc_write_wlan_reg32(rFPGA0_IQK, 0x00000000);
    h3_sdc_write_wlan_reg32(rTx_IQK_Tone_A, 0x01008C00);
    h3_sdc_write_wlan_reg32(rRx_IQK_Tone_A, 0x01008C00);
    h3_sdc_write_wlan_reg8(0x0522, 0x00);   /* TXPAUSE снять */

    /* H2C-обвязка после IQK (sdio_halinit.c:1291-1293): сообщить FW об
       окончании калибровки — H2C 0x6D={0}. */
    {
        uint32_t h2c_cmd = 0;
        h3_sdc_write_wlan_reg8(0x01CC, 0x00);
        h2c_cmd = 0u | (0x00u << 8) | (0x6Du << 0);
        h3_sdc_write_wlan_reg32(0x01D0, h2c_cmd);
        udelay(1000);
    }
    uart_print("  iqk: done\r\n");
}

/* ---- RX-тест: проверка живости RX-пути без завершённого IQK ----
 * 1) RF_CHNLBW = 0x401 (канал 1, 20 МГц) — БЕЗ бита 15 (в 0x8C01 он
 *    оставался от IQK-режима и блокировал приём);
 * 2) FIFO-граница TX/RX (0x0114, rxff_bndy высокий байт);
 * 3) RCR=0x7000208e / RRSR=0x0440 / RXFLTMAP / MAR — приём mgmt+bcast;
 * 4) REG_CR бит 6 (RXDMA_EN) — включить RX DMA;
 * 5) опрос счётчика принятых пакетов (RXPKT_NUM 0x287, device 8). */
void h3_wifi_rf_rx_test(void)
{
    uint8_t num = 0;
    uint16_t sf16 = 0;
    uint32_t d = 0;
    unsigned int t;

    uart_print("  rx: start\r\n");

    /* 0) Разбудить firmware из LPS (sdio_halinit.c:938-1005 «set rpwm» +
           «polling cpwm»): HRPWM1 (0x80) toggle bit7|PS_STATE bit6,
           затем поллить смену bit7 в CPWM (0x38) — FW подтверждает
           пробуждение. Плюс диагностика MCUFWDL (0x0080) / SYS_FUNC_EN
           (0x0002) побитно. */
    {
        uint8_t rpwm = 0, cpwm_orig = 0, cpwm_now = 0;
        unsigned int wt;
        /* MCUFWDL / SYS_FUNC_EN до пробуждения. */
        h3_sdc_read_wlan_reg8(0x0080, &rpwm);
        uart_print("  rx: mcu=0x");
        uart_print_hex((uint32_t)rpwm);
        uart_print(" (fw_rdy=");
        uart_print_int((int)((rpwm >> 1) & 1));
        uart_print(" err=");
        uart_print_int((int)((rpwm >> 2) & 1));
        uart_print(" rst=");
        uart_print_int((int)((rpwm >> 3) & 1));
        uart_print(")\r\n");
        h3_sdc_read_wlan_reg16(0x0002, &sf16);
        uart_print("  rx: sf16=0x");
        uart_print_hex((uint32_t)sf16);
        uart_print(" (cpu_en=");
        uart_print_int((int)((sf16 >> 2) & 1));
        uart_print(")\r\n");
        /* CPWM текущий (toggle). */
        h3_sdc_cmd52_read(0, 0x0038, &cpwm_orig);
        /* set rpwm: toggle bit7 + PS_STATE bit6. */
        h3_sdc_cmd52_read(0, 0x0080, &rpwm);
        rpwm = (uint8_t)((rpwm & 0x80) + 0x80);
        rpwm |= 0x40;
        h3_sdc_cmd52_write(0, 0x0080, rpwm);
        uart_print("  rx: rpwm=0x");
        uart_print_hex((uint32_t)rpwm);
        uart_print("\r\n");
        /* Поллим CPWM: смена bit7 (toggle) = FW проснулась. */
        wt = 0;
        do {
            udelay(1000);
            h3_sdc_cmd52_read(0, 0x0038, &cpwm_now);
        } while (((cpwm_orig ^ cpwm_now) & 0x80) == 0 && (++wt < 100));
        uart_print("  rx: cpwm=0x");
        uart_print_hex((uint32_t)cpwm_now);
        uart_print(" (wake=");
        uart_print_int((int)(((cpwm_orig ^ cpwm_now) & 0x80) ? 1 : 0));
        uart_print(")\r\n");
    }

    /* 1) Канал 1 / 20 МГц: 0x401 (как после LCK), НЕ 0x8C01 (bit15!). */
    rf_write(0x18, 0x00000401);
    udelay(100);
    uart_print("  rx: rf18=0x");
    uart_print_hex(rf_read(0x18));
    uart_print("\r\n");

    /* 2) LLT + границы FIFO (rtl8188f_InitLLTTable, _InitPageBoundary):
       - AUTO_LLT (0x0224) |= BIT16 — автопостроение LLT-таблицы
         страниц SRAM (без этого RX-буфер не выделен, fifopg=0);
       - TRXFF_BNDY+2 (0x0116) = RX_DMA_BOUNDARY = 0x3EFF (16K-0x100-1). */
    h3_sdc_read_wlan_reg32(0x0224, &d);
    h3_sdc_write_wlan_reg32(0x0224, d | (1u << 16));
    {
        unsigned int lt = 0;
        do {
            udelay(2000);
            h3_sdc_read_wlan_reg32(0x0224, &d);
        } while ((d & (1u << 16)) && (++lt < 500));   /* до 1 с */
        uart_print("  rx: llt=0x");
        uart_print_hex(d);
        uart_print("\r\n");
    }
    h3_sdc_write_wlan_reg16(0x0116, 0x3EFF);   /* rxff_bndy (0x0116, 16-бит) */
    h3_sdc_read_wlan_reg16(0x0116, (uint16_t *)&d);
    d &= 0xFFFF;
    uart_print("  rx: rx_bndy=0x");
    uart_print_hex(d);
    uart_print("\r\n");

    /* 2b) TX-границы (_InitTxBufferBoundary, sdio_halinit.c:321-324):
         txpktbuf_bndy = TX_PAGE_BOUNDARY_8188F = 0xF1
         (0xFF - BCNQ 0x08 - BCNQ1 0x00 - WOWLAN 0x07 + 1) в 4 регистра. */
    h3_sdc_write_wlan_reg8(0x0424, 0xF1);   /* TXPKTBUF_BCNQ_BDNY */
    h3_sdc_write_wlan_reg8(0x0425, 0xF1);   /* TXPKTBUF_MGQ_BDNY */
    h3_sdc_write_wlan_reg8(0x045D, 0xF1);   /* TXPKTBUF_WMAC_LBK */
    h3_sdc_write_wlan_reg8(0x0114, 0xF1);   /* TRXFF_BNDY (tx start) */

    /* _InitNetworkType: REG_CR[17:16] = NT_LINK_AP (0x2). */
    h3_sdc_read_wlan_reg32(0x0100, &d);
    d = (d & ~0x30000u) | (0x2u << 16);
    h3_sdc_write_wlan_reg32(0x0100, d);
    uart_print("  rx: nettype_ap, cr=0x");
    uart_print_hex(d);
    uart_print("\r\n");

    /* 3) RCR (0x0608): APP_PHYST_RXFF|APP_ICV|APP_MIC (уже в 0x7000208E)
       + AAP (бит 0) — принимать все физические пакеты (для скана);
       RRSR (0x0440), RXFLTMAP, MAR — приём mgmt/broadcast. */
    h3_sdc_write_wlan_reg32(0x0608, 0x7000208Fu);   /* + AAP */
    /* RRSR (0x0440): базовые скорости. */
    h3_sdc_write_wlan_reg32(0x0440, 0x00000F0Fu);
    /* RXFLTMAP: все типы кадров. */
    h3_sdc_write_wlan_reg16(0x06A4, 0xFFFF);   /* RXFLTMAP2 */
    h3_sdc_write_wlan_reg16(0x06A2, 0x0400);   /* RXFLTMAP1: ps-poll */
    h3_sdc_write_wlan_reg16(0x06A0, 0xFFFF);   /* RXFLTMAP0: mgmt */
    /* MAR (0x0620): broadcast + свой MAC (пишем FF, приём всех). */
    h3_sdc_write_wlan_reg32(0x0620, 0xFFFFFFFF);
    h3_sdc_write_wlan_reg32(0x0624, 0xFFFFFFFF);

    /* 4) REG_CR (0x0100): бит 6 = HCI_RXDMA_EN, бит 7 = RXDMA_ENABLE,
       бит 8 = MACTXEN, бит 9 = MACRXEN (sdio_halinit.c:1240 «Enable
       MACTXEN/MACRXEN block») — 32-бит RMW, чтобы НЕ затереть bit17
       (NETTYPE_AP) из шага выше. Без MACRXEN 802.11-MAC не запущен
       и RX невозможен в принципе. */
    h3_sdc_read_wlan_reg32(0x0100, &d);
    d |= (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9);
    h3_sdc_write_wlan_reg32(0x0100, d);
    h3_sdc_read_wlan_reg32(0x0100, &d);
    uart_print("  rx: cr=0x");
    uart_print_hex((uint32_t)d);
    uart_print(" (bit17=");
    uart_print_int((int)((d >> 17) & 1));
    uart_print(" bit8=");
    uart_print_int((int)((d >> 8) & 1));
    uart_print(" bit9=");
    uart_print_int((int)((d >> 9) & 1));
    uart_print(")\r\n");

    /* 4b) REG_RXPKT_NUM (0x287 device 8) = счётчик пакетов в RXPKTBUF;
           его бит 18 (RW_RELEASE_EN) = пауза RX DMA. Если взведён —
           DMA заморожена и пакеты не выкладываются в хост. Сбрасываем
           (release), как rtw_hal_release_rx_dma (hal_com.c:1566). */
    h3_sdc_read_wlan_reg32(0x0287, &d);
    h3_sdc_write_wlan_reg32(0x0287, d & ~(1u << 18));
    h3_sdc_read_wlan_reg32(0x0287, &d);
    uart_print("  rx: rxpk=0x");
    uart_print_hex(d);
    uart_print(" (rw_release=");
    uart_print_int((int)((d >> 18) & 1));
    uart_print(")\r\n");

    /* 4c) SDIO HIMR (0x14, device 0) — 32-битный: RX_REQUEST (BIT0),
           AVAL (BIT1), TXERR (BIT2) в байте 0x14 + C2HCMD (BIT17 =
           бит1 байта 0x16). Раньше писали только байт 0x14 (0x01) —
           C2H-маска (0x16) не включалась, поэтому C2H-события не шли.
           HISR (0x18) сбросить. */
    h3_sdc_cmd52_write(0, 0x0014, 0x07);   /* RX_REQUEST|AVAL|TXERR */
    h3_sdc_cmd52_write(0, 0x0015, 0x00);
    h3_sdc_cmd52_write(0, 0x0016, 0x02);   /* C2HCMD (BIT17) */
    h3_sdc_cmd52_write(0, 0x0017, 0x00);
    h3_sdc_cmd52_write(0, 0x0018, 0xFF);   /* HISR: clear */
    h3_sdc_cmd52_read(0, 0x0014, &num);
    uart_print("  rx: himr_lo=0x");
    uart_print_hex((uint32_t)num);
    h3_sdc_cmd52_read(0, 0x0016, &num);
    uart_print(" hi=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");

    /* 4e) Диагностика (W.165): порог агрегации, drvinfo, RXDMA_CTRL,
           C2H_EVT (firmware mailbox), HISR до свипа. */
    h3_sdc_read_wlan_reg8(0x0280, &num);
    uart_print("  rx: agg_th=0x");
    uart_print_hex((uint32_t)num);
    h3_sdc_read_wlan_reg8(0x060F, &num);
    uart_print(" drvinfo=0x");
    uart_print_hex((uint32_t)num);
    h3_sdc_read_wlan_reg8(0x0286, &num);
    uart_print(" rxdma=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");
    h3_sdc_read_wlan_reg32(0x01A0, &d);
    uart_print("  rx: c2h_evt=0x");
    uart_print_hex(d);
    uart_print("\r\n");
h3_sdc_cmd52_read(0, 0x0018, &num);
    uart_print("  rx: hisr_before=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");

    /* 4d) RX-DMA настройки из драйвера (sdio_halinit.c):
         - PBP (0x0104) = PBP_128|PBP_128 (0x11) — разбиение SRAM;
         - RX_DRVINFO_SZ (0x060F) = 4 — размер драйвер-инфо перед
           802.11-кадром;
         - TRXDMA_CTRL (0x010C) |= RXDMA_AGG_EN (BIT2) — агрегация;
         - RXDMA_MODE_CTRL (0x0290) |= RXDMA_AGG_MODE_EN (BIT1). */
    h3_sdc_write_wlan_reg8(0x0104, 0x11);          /* PBP 128+128 */
    h3_sdc_write_wlan_reg8(0x060F, 0x04);          /* RX_DRVINFO_SZ */
    h3_sdc_read_wlan_reg8(0x010C, &num);
    h3_sdc_write_wlan_reg8(0x010C, (uint8_t)(num | 0x04));  /* RXDMA_AGG_EN */
    h3_sdc_read_wlan_reg8(0x0290, &num);
    h3_sdc_write_wlan_reg8(0x0290, (uint8_t)(num | 0x02));  /* RXDMA_AGG_MODE_EN */
    h3_sdc_read_wlan_reg8(0x0290, &num);
    uart_print("  rx: rxdma_mode=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");

    /* readback ключевых регистров + FIFO. */
    h3_sdc_read_wlan_reg32(0x0608, &d);
    uart_print("  rx: rcr=0x"); uart_print_hex(d);
    h3_sdc_read_wlan_reg32(0x0440, &d);
    uart_print(" rrsr=0x"); uart_print_hex(d);
    h3_sdc_read_wlan_reg32(0x0620, &d);
    uart_print(" mar=0x"); uart_print_hex(d);
    uart_print("\r\n");
    h3_sdc_read_wlan_reg16(0x06A0, (uint16_t *)&d);
    d &= 0xFFFF;
    uart_print("  rx: flt0=0x"); uart_print_hex(d);
    h3_sdc_read_wlan_reg16(0x06A2, (uint16_t *)&d);
    d &= 0xFFFF;
    uart_print(" flt1=0x"); uart_print_hex(d);
    h3_sdc_read_wlan_reg16(0x06A4, (uint16_t *)&d);
    d &= 0xFFFF;
    uart_print(" flt2=0x"); uart_print_hex(d);
    h3_sdc_read_wlan_reg32(0x0204, &d);
    uart_print(" fifopg=0x"); uart_print_hex(d);
    h3_sdc_read_wlan_reg32(0x011C, &d);
    uart_print(" rxffptr=0x"); uart_print_hex(d);
    uart_print("\r\n");

    /* 4e) Диагностика: порог агрегации, drvinfo, RXDMA_CTRL, C2H_EVT,
           HISR до свипа. */
    h3_sdc_read_wlan_reg8(0x0280, &num);
    uart_print("  rx: agg_th=0x");
    uart_print_hex((uint32_t)num);
    h3_sdc_read_wlan_reg8(0x060F, &num);
    uart_print(" drvinfo=0x");
    uart_print_hex((uint32_t)num);
    h3_sdc_read_wlan_reg8(0x0286, &num);
    uart_print(" rxdma=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");
    h3_sdc_read_wlan_reg32(0x01A0, &d);
    uart_print("  rx: c2h_evt=0x");
    uart_print_hex(d);
    uart_print("\r\n");
    h3_sdc_cmd52_read(0, 0x0018, &num);
    uart_print("  rx: hisr_before=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");

    /* 5) Опрос RX-доступности через SDIO_REG_RX0_REQ_LEN (0x1C,
          локальное SDIO-пространство, device 0) — на SDIO-чипах это
          единственный признак прихода пакета в RX FIFO (sdio_ops.c:2196),
          а не «pktnum» (USB-регистр). Свип каналов 1 → 6 → 11. */
    for (t = 0; t < 12; t++) {
        uint32_t ch;
        uint16_t rxlen = 0;
        /* Канал в [7:0], BW в [11:8]=0x4: 1=0x401, 6=0x406, 11=0x40B. */
        if (t < 4)      ch = 0x401u;
        else if (t < 8) ch = 0x406u;
        else            ch = 0x40Bu;
        rf_write(0x18, ch);
        udelay(50000);   /* 50 мс на перестройку синтезатора */
        udelay(150000);  /* 150 мс слушаем */
        h3_sdc_cmd52_read(0, 0x001C, &num);      /* RX0_REQ_LEN, device 0 */
        rxlen = num;
        uart_print("  rx: t=");
        uart_print_int((int)t);
        uart_print(" ch=");
        uart_print_int((int)(ch & 0xFF));
        uart_print(" rxlen=0x");
        uart_print_hex((uint32_t)rxlen);
        /* RXPKT_NUM (0x287 dev8): счётчик пакетов во внутреннем RXPKTBUF
           FW — растёт ли, если чип физически принимает (MACRXEN включён). */
        h3_sdc_read_wlan_reg8((8u << 13) | 0x0287, &num);
        uart_print(" pkt=0x");
        uart_print_hex((uint32_t)num);
        uart_print("\r\n");
    }

    /* HISR после свипа — поднялся ли RX_REQUEST (bit0) от FW. */
    h3_sdc_cmd52_read(0, 0x0018, &num);
    uart_print("  rx: hisr_after=0x");
    uart_print_hex((uint32_t)num);
    uart_print("\r\n");

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

    /* 0) Для загрузки прошивки переключаемся на 1-bit шину (данные по D0).
        Блок size Fn1 ставим 512 (как в драйвере sdio_intf.c: sdio_set_block_size
        (func, 512)); он нужен для блок-режима данных, но сама прошивка
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

            uart_print("  fwdl page=");
            uart_print_int((int)page);
            uart_print(" off=0x");
            uart_print_hex(off);
            uart_print(" len=0x");
            uart_print_hex(page_end - off);
            uart_print("\r\n");

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
                    uart_print("  fwdl CMD53W FAIL rc=");
                    uart_print_int(rc);
                    uart_print(" w=");
                    uart_print_int((int)w);
                    uart_print("\r\n");
                    return -40 - rc;
                }
            }
            if (r4) {   /* хвост: дополняем нулями до слова */
                uint8_t tail[4] = {0, 0, 0, 0};
                for (uint32_t k = 0; k < r4; k++) tail[k] = b[off + n4 * 4 + k];
                rc = h3_sdc_cmd53_write_bytes(1, ftaddr + n4 * 4, 4, tail);
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
 *    0b00 = 1-bit, 0b01 = 4-bit, 0b10 = 8-bit (Linux mmc/sdio.c).
 * 2) Аппаратно: SDC_CTYPE (0x0C) = 1 — шина 4-bit.
 *
 * В рабочей цепочке (W.136) 4-bit не используется: перед загрузкой прошивки
 * шина принудительно ставится в 1-bit (h3_wifi_download_fw).
 * Возвращает 0 при успехе; не-0 — ошибка. */
int h3_sdio_set_bus_width(int four_bit)
{
    int rc;
    uint8_t v;

    if ((rc = h3_sdc_cmd52_read(0, 0x07, &v)) != 0) return rc;
    /* Сохранить прочие биты (высокоскоростной режим и пр.), обновить только
       поле ширины [2:0]: 0b00=1-bit, 0b01=4-bit. */
    uint8_t nv = (uint8_t)((v & ~0x07u) | (four_bit ? 0x01u : 0x00u));
    if ((rc = h3_sdc_cmd52_write(0, 0x07, nv)) != 0) return rc;

    /* Аппаратная ширина шины контроллера: 0=1-bit, 1=4-bit. */
    SDC_REG(SDC_CTYPE) = four_bit ? 0x1u : 0x0u;
    __asm volatile("dsb" ::: "memory");
    return 0;
}