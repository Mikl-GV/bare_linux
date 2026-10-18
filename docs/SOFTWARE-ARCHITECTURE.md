# Как работает ПО MultiTool H3 (bare-metal) — архитектура и код функций

> Документ описывает работу прошивки `h3_bare.bin` для Allwinner H3 (Orange Pi Lite)
> с точки зрения кода: инициализация, главный цикл, модули, ключевые функции.
> Все фрагменты — подлинный код из `src/` (не пересказ).
> Дата: 2026-10-17. Проект: `E:\Giga_1\Bare_linux`.

---

## 1. Общая картина

Прошивка — bare-metal приложение для 4× Cortex-A7 (ARMv7), без ОС и библиотек.
Загружается U-Boot (`go 0x40000000`) или FEL+SPL на адрес 0x40000000.

Состав (по `Makefile`):

| Модуль | Файл | Роль |
|---|---|---|
| Загрузчик | `src/boot.s` | векторы, MMU-таблица, передача в kmain |
| Прошивка WiFi | `src/fw_blob.S` | `.incbin "fw_nic.bin"` — NIC-прошивка RTL8189FTV |
| Главный | `src/main.c` | `kmain()`: инициализация, меню, главный цикл |
| UART | `src/uart.c` | UART0 115200 8N1 (диагностика) |
| Порты/GPIO | `src/ports.c` | PIO настройка, LED, кнопки |
| Таймер | `src/sys.c` | High-Speed Timer → `micros()`, `udelay()` |
| I2S/звук | `src/i2s.c` | PLL_AUDIO, I2S0 → MAX98357A, кольцо+DMA |
| DMA | `src/h3_dma.c` | DMAC (sun6i), LLI-циклическое кольцо |
| MMU/кэш | `src/h3_mmu.c` | `mmu_mark_uncached()`, `dcc_clean_range()` |
| I2C bit-bang | `src/h3_i2c_bb.c` | SDA/SCL программно |
| PCF8574 | `src/pcf8574.c` | кнопки через расширитель 0x27 |
| Дисплей | `src/display.c` | переиспользует фреймбуфер U-Boot (DE2) |
| Шрифт | `src/font8x8.c` | 8×8 растровый шрифт |
| Безопасность | `src/secure.c` | `secure_unlock()` — снять TrustZone-замки |
| GIC | `src/gic.c` | маршрутизация прерываний (distributor+CPU iface) |
| **SDIO/WiFi** | **`src/h3_sdc.c`** | **контроллер MMC, CMD52/53, RTL8189FTV: power-on, FW, RF LCK/IQK, RX-тест** |

Точка входа — `kmain()` (main.c:225):

```c
void kmain(void)
{
    secure_unlock();
    gic_init();
    sys_timer_init();
    gpio_init();
    uart_init();
    uart_print("\r\n=== MultiTool H3 bare-metal ===\r\n");
    uart_print("FW v" FW_VERSION_STR "\r\n");
    set_led(1);
    /* PA0-3 = JTAG (RMW) */
    volatile uint32_t *pio_cfg0 = (volatile uint32_t *)0x01C20800;
    *pio_cfg0 = (uint32_t)((*pio_cfg0 & 0xFFFF0000u) | 0x00003333u);

    uart_print("i2c init...\r\n");
    i2cbb_init(PORTG, 9, PORTG, 8);
    pcf8574_init();
    uart_print("i2c ok\r\n");

    uart_print("i2s init...\r\n");
    if (i2s_init() != 0) uart_print("I2S FAIL\r\n");
    else                 uart_print("i2s ok\r\n");

    /* --- WiFi: SDC1 + SDIO-карта RTL8189FTV --- */
    uart_print("wifi init...\r\n");
    h3_sdc1_init();
    /* ...диагностика CCU/PIO... */
    int rc = h3_sdio_init_chip();
    if (rc == 0) {
        wifi_active = 1;
        /* CCCR, CIS ID (0x024C:F179), power-on, Fn1 enable, FW download, eFuse/MAC */
        /* ... */
        h3_wifi_rf_lck();
        h3_wifi_rf_rx_test();
    }
    uart_print("wifi ok\r\n");

    uart_print("display init...\r\n");
    display_init();
    draw_ui();
    uart_print("display ok\r\n");

    /* --- Главный цикл --- */
    for (;;) {
        led_heartbeat();
        uint32_t btns = pcf8574_read();
        if (btns != prev_buttons) handle_buttons(btns);
        udelay(10000);
    }
}
```

---

## 2. Периферия нижнего уровня

### 2.1 UART (src/uart.c)

UART0 на 115200 8N1. U-Boot уже задал такт/скорость; код лишь включает такт,
снимает reset и настраивает PA4=TX (F2):

```c
void uart_init(void)
{
    set_pin_mode(PORTA, 4, 2);              /* PA4 = UART0 TX */
    H3_CCU_GATING3 |= (1u << 16);           /* такт UART0 */
    H3_CCU_RST4    |= (1u << 16);           /* снять reset */
    volatile uint32_t n = 1000;
    while (n--) {}
}

void uart_putc(char c)
{
    uint32_t t = 0;
    while (!uart_tx_ready()) {
        if (++t > 100000) return;           /* защита от вечного цикла */
    }
    UART0_THR = (uint32_t)(unsigned char)c;
}

void uart_print(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
```

`uart_print_hex()` печатает `000ABCDE`-стиль 8 hex-цифр; `uart_print_int()` — десятичное.

### 2.2 Порты/GPIO (src/ports.c)

Обёртка над PIO-регистрами (data/cfg/pull):

```c
void set_pin_mode(uint32_t port, uint32_t pin, uint32_t mode)
{
    volatile uint32_t *cfg = cfg_reg(port, pin);
    uint32_t shift = (pin % 8) * 4;
    uint32_t v = *cfg;
    v &= ~(0xFUL << shift);
    v |= (mode & 0xF) << shift;
    *cfg = v;
}

void set_pin_data(uint32_t port, uint32_t pin, uint32_t data)
{
    volatile uint32_t *ddr = (volatile uint32_t *)(port + 0x10);
    if (data) *ddr |= (1u << pin);
    else      *ddr &= ~(1u << pin);
}

void gpio_init(void)
{
    H3_APB0_CLK_GATING |= (1u << 0);        /* такт порта L (always-on) */
    __asm volatile("dsb" ::: "memory");
}

void set_led(int on)                        /* PL10 — пользовательский LED */
{
    set_pin_mode(PORTL, 10, 1);
    set_pin_data(PORTL, 10, on ? 1 : 0);
}
```

### 2.3 Таймер (src/sys.c)

High-Speed Timer 0x01C60000, 64-бит счётчик вниз, реально ~96.8 МГц (берём 97):

```c
uint32_t micros(void)
{
    return (~HSTMR_CURNT_LO) / HSTMR_MHZ;   /* 97 */
}

void udelay(uint32_t us)
{
    uint32_t cur = HSTMR_CURNT_LO;
    uint32_t ticks = us * HSTMR_MHZ;
    while ((uint32_t)(cur - HSTMR_CURNT_LO) < ticks) { }
}
```

`sys_timer_init()` делает полный reset HSTMR (RST0 bit19), включает gate и counter.

### 2.4 MMU/кэш (src/h3_mmu.c)

H3 не кэш-когерентен: DMAC читает физическую память в обход L1/L2. Память
буферов помечается uncached, а перед стартом DMA кэш чистят:

```c
void mmu_mark_uncached(uint32_t addr)
{
    uint32_t l1_base, ref, idx;
    __asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(l1_base));
    l1_base &= ~0x3FFFu;
    idx = (addr >> 20) & 0xFFF;
    ref = ((volatile uint32_t *)l1_base)[idx];
    switch (ref & 3u) {
    case 0b10u: {   /* Section (1MB) */
        uint32_t ne = (addr & 0xFFF00000u) | (ref & 0x0008DDF0u)
                    | (1u << 16) | (0u << 3) | (0u << 2) | 0b10u;
        ((volatile uint32_t *)l1_base)[idx] = ne;
        dcc_clean_line(l1_base + idx * 4);
        break;
    }
    case 0b11u: {   /* L2 (large page 16КБ) */
        uint32_t l2_base = ref & 0xFFFFFC00u;
        uint32_t n = (addr >> 14) & 0x3FFu;
        volatile uint32_t *l2 = (volatile uint32_t *)l2_base;
        uint32_t e0 = l2[n * 2 + 0];
        if ((e0 & 3u) == 0b01u) {
            e0 &= ~((1u << 3) | (1u << 2));   /* C=0,B=0 */
            l2[n * 2 + 0] = e0;
            dcc_clean_line(l2_base + n * 8);
        }
        break;
    }
    default: break;
    }
    __asm volatile("dsb" ::: "memory");
    __asm volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));  /* TLBIALL */
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

void dcc_clean_range(uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~0x1Fu;
    uint32_t end = (addr + len + 0x1Fu) & ~0x1Fu;
    for (uint32_t a = start; a < end; a += 32)
        dcc_clean_line(a);                  /* DCCMVAC */
    __asm volatile("dsb" ::: "memory");
}
```

---

## 3. Звук: I2S0 → MAX98357A (src/i2s.c, src/h3_dma.c)

Формат: 48 кГц стерео 16 бит; отсчёт в СТАРШИХ 16 битах 32-битного слова FIFO,
пара `[L<<16][R<<16]` (TXIM=0, SW=32, SR=16, MODE=Left+offset). Стереокадр = 8 байт.

Кольцо: 4 периода × 1024 кадра = 32 КБ (85 мс при 48 кГц). Вывод потоковый:
приложение пишет PCM через `i2s_write()`, DMAC крутит кольцо по LLI-списку,
позиция чтения — из `CUR_SRC` DMA-канала (без прерываний).

### 3.1 PLL_AUDIO (i2s.c:139)

```c
static int pll_audio_program(unsigned fs)
{
    uint32_t pat; unsigned n, m; int i;
    if (fs == 48000u)      { pat = 0xC000AC02u; n = 14; m = 14; } /* 24.576 МГц */
    else if (fs == 44100u) { pat = 0xC0010D84u; n = 7;  m = 8;  } /* 22.5792 МГц */
    else return -1;

    H3_CCU_PLL_AUDIO_PAT  = pat;
    H3_CCU_PLL_AUDIO_CTRL = (1u << 31) | (1u << 24)
                          | ((uint32_t)(n - 1) << 8) | (uint32_t)(m - 1);
    for (i = 0; i < 100; i++) {
        if (H3_CCU_PLL_AUDIO_CTRL & (1u << 28)) return 0;   /* LOCK */
        udelay(100);
    }
    return -1;
}
```

### 3.2 Кольцо и DMA (i2s.c + h3_dma.c)

```c
/* i2s.c — позиция проигрывания по CUR_SRC DMA-канала */
static void poll_played(void)
{
    uint32_t pos = h3_dma_audio_pos_bytes() / BYTES_PER_FRAME;
    if (pos >= RING_FRAMES) pos = RING_FRAMES - 1u;
    if (pos < g_last_pos)
        g_played += (uint64_t)(RING_FRAMES - g_last_pos) + pos;
    else
        g_played += (uint64_t)(pos - g_last_pos);
    g_last_pos = pos;
}
```

```c
/* h3_dma.c — конфигурация LLI: RAM(linear,32b,burst8) -> I2S0_TX_FIFO */
uint32_t cfg = CFG_SRC_DRQ(DRQ_SDRAM) | CFG_SRC_MODE_LIN
             | CFG_SRC_BURST(BURST_8) | CFG_SRC_WIDTH(2)
             | CFG_DST_DRQ(DRQ_I2S0_TX) | CFG_DST_MODE_IO
             | CFG_DST_BURST(BURST_8) | CFG_DST_WIDTH(2);
uint32_t period_bytes = buf_bytes / (uint32_t)periods;
for (int i = 0; i < periods; i++) {
    struct dma_lli *l = &g_lli[i];
    l->cfg  = cfg;
    l->src  = buf_phys + (uint32_t)i * period_bytes;
    l->dst  = H3_I2S0_TX_FIFO;
    l->len  = period_bytes;
    l->para = NORMAL_WAIT;
    l->p_next = (uint32_t)(uintptr_t)&g_lli[(i + 1) % periods];  /* циклически */
}
dcc_clean_range((uint32_t)(uintptr_t)&g_lli[0], /* LLI вне кэша для DMAC */
                (uint32_t)(uintptr_t)&g_lli[periods] -
                (uint32_t)(uintptr_t)&g_lli[0]);
```

Тестовый тон — таблица синуса 256 точек (int16):

```c
static inline int32_t sin_t(uint32_t phase) { return (int32_t)sin_tab[(phase >> 24) & 0xFF]; }
```

---

## 4. SDIO и WiFi-чип RTL8189FTV (src/h3_sdc.c)

Хост-сторона — 1:1 с эталонным драйвером rtl8189fs (v4.3.24_15589). Чип
подключён к SDC1 (PG0-PG5). Шина стенда — 1-bit/50 МГц (4-bit ломает CMD53).

### 4.1 Инициализация контроллера MMC (h3_sdc1_init, h3_sdc.c:120)

1. `wifi_power_on()` — питание чипа (WIFI_EN=PL7);
2. CCU: gate MMC1 + полный цикл reset (assert→deassert);
3. модульный такт MMC1: OSC24M, N=2, M=16 (≈375 кГц для SD-init);
4. пины PG0..PG5 = function 2 (SDC1);
5. reset контроллера (GCTL_RESET), HWRST active→reset→active, 1 мс;
6. CKC: CCLK_EN + делитель (375 кГц), SD_FUNS=0xCEAA, ширина 1-bit, блок 512.

### 4.2 CMD52/CMD53 (PIO, без IDMA)

```c
int h3_sdc_cmd52_read(uint32_t func, uint32_t reg, uint8_t *out)
{
    uint32_t arg = (0u << 31) | ((func & 7u) << 28) | ((reg & 0x1FFFF) << 9);
    int rc = h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
    if (rc != 0) return rc;
    *out = (uint8_t)(SDC_REG(SDC_RESP0) & 0xFF);
    return 0;
}

int h3_sdc_cmd53_write_bytes(uint32_t func, uint32_t addr, uint32_t len, const void *data)
{
    uint32_t arg = (1u << 31) | ((func & 7u) << 28)
                 | (0u << 27)               /* byte mode */
                 | (1u << 26)               /* inc */
                 | ((addr & 0x1FFFF) << 9)
                 | (len & 0x1FF);
    return h3_sdc_block_transfer(53, arg, len, 1, 1, (void *)data);
}
```

Обмен данными — **PIO** через FIFO-регистр контроллера:

```c
/* h3_sdc_block_transfer (h3_sdc.c:2837) — запись */
SDC_REG(SDC_GCTL) |= (1u << 31);                    /* FIFO_AC_MODE_AHB */
cmdr_val = SDC_CMDR_START | (cmd & SDC_CMDR_IDX_MASK)
         | SDC_CMDR_RESP_RCV | SDC_CMDR_DATA_TRANS
         | SDC_CMDR_TRANS_DIR | SDC_CMDR_WAIT_PRE;
SDC_REG(SDC_CMDR) = cmdr_val;
for (i = 0; i < words; i++) {
    sdc_wait(..., SDC_STAR_FIFO_FULL, 0);
    fifo[0] = in[i];                                /* 1 слово за раз */
}
sdc_wait(..., SDC_STAR_FIFO_EMPTY, 1);              /* дождаться отправки */
```

Столько-big диагностика: `sdc_diag_*` (cmdr/risr/sta/idst/dmac) фиксируются при
ошибках и печатаются в main.c.

### 4.3 Загрузка NIC-прошивки (h3_wifi_download_fw + fw_write_image)

Протокол — `rtl8188f_FirmwareDownload`:

1. `_FWDownloadEnable(1)`: 0x03|=0x04 (8051 enable), 0x80|=0x01 (FWDL),
   0x82&=0xf7 (reset 8051);
2. запись прошивки **постранично** (4096 б), page-select `MCUFWDL+2[2:0]`,
   байт-режим CMD53, старт `(8<<13)|0x1000`, хвост без добивки нулями;
3. полиинг `FWDL_ChkSum_rpt`;
4. `_FWDownloadEnable(0)`: снять FWDL;
5. `_FWFreeToGo`: 0x80|=MCUFWDL_RDY, &=~WINTINI_RDY, полный `_8051Reset8188`
   (0x1D bit0 снять→взвести, 0x03 bit2 снять→взвести), полиинг
   `WINTINI_RDY|ChkSum_rpt|MCUFWDL_RDY|RAM_DL_SEL` → штатно MCUFWDL=0xC6.

```c
static int fw_write_image(const uint8_t *b, uint32_t size)
{
    uint8_t t;
    WREG8(REG_MCUFWDL_OFF, (t | FWDL_ChkSum_rpt));      /* сброс chksum до записи */
    uint32_t page = 0, off = 0;
    while (off < size) {
        uint32_t page_end = off + 4096; if (page_end > size) page_end = size;
        t = (t & 0xF8) | (page & 0x07);                 /* page-select */
        WREG8(REG_MCUFWDL_OFF + 2, t);
        uint32_t ftaddr = (8u << 13) | (FW_8188F_START & 0x7FFFu);
        for (uint32_t i = 0; i < n4; i++)               /* слова по 4 б */
            h3_sdc_cmd53_write_bytes(1, ftaddr + i * 4, 4, b + off + i * 4);
        if (r4) h3_sdc_cmd53_write_bytes(1, ftaddr + n4 * 4, r4, b + off + n4 * 4);
        off = page_end; page++;
    }
    for (w = 0; w < 500; w++) {                         /* полиинг chksum */
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -35;
        if (t & FWDL_ChkSum_rpt) return 0;
        udelay(100);
    }
    return -38;
}
```

Прошивка встроена в образ через fw_blob.S (`.incbin "fw_nic.bin"`), 18802 байта,
идентична `Array_MP_8188F_FW_NIC` из драйвера (SHA256 C016CF54...).

### 4.4 RF-доступ (LSSI, rf_write/rf_read, h3_sdc.c:847)

```c
static void rf_write(uint8_t off, uint32_t data)
{
    h3_sdc_write_wlan_reg32(0x840, (((uint32_t)off & 0xFF) << 20) | (data & 0xFFFFF));
}

static uint32_t rf_read(uint8_t off)
{
    uint32_t t2 = 0, p1 = 0, rb = 0;
    h3_sdc_read_wlan_reg32(0x824, &t2);
    t2 = (t2 & ~0x7F800000u) | (((uint32_t)off & 0xFF) << 23);
    h3_sdc_write_wlan_reg32(0x824, t2 & ~0x80000000u);
    h3_sdc_read_wlan_reg32(0x824, &t2);
    h3_sdc_write_wlan_reg32(0x824, t2 & ~0x80000000u);
    h3_sdc_write_wlan_reg32(0x824, t2 | 0x80000000u);   /* edge-триггер чтения */
    udelay(100);
    h3_sdc_read_wlan_reg32(0x820, &p1);
    if (p1 & (1u << 8))
        h3_sdc_read_wlan_reg32(0x8B8, &rb);             /* PI */
    else
        h3_sdc_read_wlan_reg32(0x8A0, &rb);             /* LSSI */
    return rb & 0xFFFFF;
}
```

### 4.5 LCK-калибровка (h3_wifi_rf_lck, h3_sdc.c:891)

Последовательность (phy_LCCalibrate_8188F):
0) BB-конфиг (209) + AGC (66) + RF-таблица (136, **с пропуском записи 0x18**),
   затем RF 0x18=0x401; 1) TXPAUSE=0xFF; 2) RF 0xB0=0xDFBF0 (LDO ON);
3) RF 0x00=0; 4) RF 0x18 |= 0x8000 (LCK-триггер); 5) полиинг сброса bit15
(1 с); 6) RF 0xB0=0xDFFF0; 7) RF 0x18=0x401; 8) RF 0x00=0x33D95;
9) TXPAUSE=0. Итог: `lck: done in 70 ms`.

### 4.6 IQK-калибровка (h3_wifi_rf_iqk, h3_sdc.c:1155)

phy_IQCalibrate_8188F Path A 1T1R:
- H2C 0x6D={1} (BT_WLAN_CALIBRATION), PI-режим: 0x860/0x868=0x01000100;
- MAC-настройки: TXPAUSE=0x3F, BCN_CTRL &=~BIT3, GPIO 0x40 &=~BIT5;
- Tx one-shot (0xE48=0xF9000000→0xF8000000, 25 мс) — сохранить LOK_Result из
  RF 0x08 (**W.265**); проверка e94/e9c/eac (Tx OK);
- Rx two-pass (RF_TXPA_G2, refine), после каждого — восстановить RF 0x08=LOK_Result;
- выход: 0xE28=0, тоны 0x01008C00, SI-режим 0x860/0x868=0x01000000,
  restore Path_SEL (BB 0x948, RF 0xB0), restore BB (0x800/0xC04/0xC08/0x874),
  TXPAUSE=0, H2C 0x6D={0}.

```c
/* сохранение LOK_Result после Tx one-shot (W.265-D8) */
lok_result = rf_read(0x08);
uart_print("  iqk: lok08=0x"); uart_print_hex(lok_result); uart_print("\r\n");
/* ... в Rx-проходах: */
rf_write(0x08, lok_result);
```

Итог на стенде: Tx OK / Rx OK, `lok08=0x8F800` (живой чип 0xF800).

### 4.7 RX-конфигурация и свип (h3_wifi_rf_rx_test, h3_sdc.c:1670)

1. пробуждение FW: RPWM(0x80)=0x00, полиинг CPWM(0x38)=0x00; прерывания FBR/
   CCCR; H2C SET_PWR_MODE (box0 0x1D0 + EX 0x1F0);
2. RF_CHNLBW=0x401 (канал 1, 20 МГц), RFMOD |= BIT24|BIT25 (bCCKEn|bOFDMEn);
3. RQPN (0x200=0xE7020C)+NPQ(0x214=0x02), LLT (0x224|=BIT16), rx_bndy
   (0x116=0x3F7F), BWOPMODE=0x04, SIFS/RL/EDCA;
4. RCR=0x7000604E, RRSR=0x1F5F, RXFLTMAP, MAR; REG_CR |= MACTXEN|MACRXEN
   → 0x0206FF;
5. antenna_sel: LEDCFG2 |= BIT7 (W.264);
6. свип RF 0x18 = 0xC01/0xC06/0xC0B (каналы 1/6/11, BW20), по 3 probe request
   (`h3_wifi_tx_probe_req`), слушание 150 мс, диагностика rxlen/rxpkt/hisr/cca.

### 4.8 Probe request (h3_wifi_tx_probe_req, h3_sdc.c:3111)

```c
/* tx_desc 40 байт: OFFSET=40, BMC|FIRST|LAST|OWN, QUEUE_SEL=QSLT_MGNT=0x12,
   USE_RATE=1, TX_RATE=1M, RETRY_LIMIT=12 */
buf[2] = (uint8_t)TXDESC_SIZE_W220;
buf[3] = (uint8_t)((1u << 0) | (1u << 2) | (1u << 3) | (1u << 7));
buf[5] = (uint8_t)(0x12u << 0);
buf[13] = (uint8_t)(1u << 0);
buf[18] = (uint8_t)((1u << 1) | (12u << 2));
/* кадр: probe request, bcast, наш MAC из 0x610-0x615, IE: SSID, Rates, DS */
uint32_t total = (40 + plen + 3) & ~3u;
uint32_t fifo_addr = (WLAN_TX_HIQ_DEVICE_ID_W220 << 13) | ((total / 4) & 0x1FFFu);
return h3_sdc_cmd53_write_bytes(1, fifo_addr, total, buf);
```

Перед записью — проверка свободных страниц HIQ (`h3_wifi_tx_free_pages_hiq`),
после записи драйвер НЕ делает триггер — FW сама стартует передачу.

---

## 5. Дисплей и меню (src/display.c, src/main.c)

Дисплей переиспользует фреймбуфер U-Boot (DE2 UI-канал), не трогая DE/HDMI/TCON:

```c
int display_init(void)
{
    uint32_t laddr = UI_TOP_LADDR;      /* 0x01103010 */
    uint32_t pitch = UI_PITCH;          /* 0x0110300C */
    if (laddr >= 0x40000000u && laddr < 0x60000000u) {
        fb = (volatile uint32_t *)laddr;
        fb_pitch = (pitch >= FB_STRIDE && pitch <= 8192) ? pitch : FB_STRIDE;
        mmu_mark_uncached(laddr);       /* DE читает физическую память */
        fb_clean();
    } else {
        extern unsigned char _fb_hdmi_start[];
        fb = (volatile uint32_t *)(uintptr_t)_fb_hdmi_start;
        fb_pitch = FB_STRIDE;
    }
    return 0;
}
```

Меню — конечный автомат (SCR_MAIN / SCR_TONE / SCR_SETTINGS). Рендер рисует
прямоугольники, текст по центру, маркер выбора; кнопки — PCF8574 через I2C
bit-bang. Heartbeat — `led_heartbeat()` (каждые 500 мс переключает PL10).

---

## 6. Управление кнопками (src/pcf8574.c + h3_i2c_bb.c)

I2C bit-bang: SCL=PG9, SDA=PG8, PCF8574 @0x27. Детект нажатий — по фронту
`btns & ~prev_buttons` (main.c:173-175).

---

## 7. Поток выполнения WiFi-этапа (сводно, актуально на W.316)

```
SDIO CMD5→3→7 → CCCR → chip_id 0x024C:F179 → PowerSeq CARDEMU→ACTIVE
 → Fn1 enable → FW download (5 страниц, chksum OK, MCUFWDL=0xC6)
 → eFuse/MAC (программный 02:00:00:xx) → REG_MACID verify
 → LCD: MAC(101)+BB(207)+AGC(66)+RF(136) → LCK
 → RX: FW active (RPWM/CPWM), H2C SET_PWR_MODE, IGI/MSR, FBR/CCCR,
   RFMOD, RQPN/LLT/bndy, antenna_sel, BWOPMODE, CR=0x206FF
 → IQK: PI, Tx/Rx one-shot, LOK_Result, restore
 → RX-sweep ch 1..13, probe request, чтение RX FIFO по 1 блоку
   → парсер beacon → SSID (W.316: BCN plen= ch= ssid=)
 → C2H-ping (H2C 0x43) → C2H-ABSENT (см. NEXT-CHAT-PROMPT)
```

**Текущий статус (W.338):** хост-инициализация 1:1 с драйвером, **RX-приём
работает** — чип принимает beacon'ы на всех 13 каналах, парсер извлекает SSID.
Хост-чтение RX-FIFO по 1 блоку (512 б)/CMD53 (host H3 не тянет многоблок).
RPWM/CPWM/HIMR/HISR пишутся через **fn1** (SDIO-local), не fn0 — с W.329 FW
подтверждает переход в ACTIVE по смене toggle (cpwm=0x80, tog_sw=1), HIMR=0x01.
Эксперимент W.337/W.338: регистр HRPWM1 принимает полный 0xCC (rb=0xCC), но
CPWM state[3:0] остаётся 0 — на этой связке FW репортит ACK только через
toggle; маска 0xC1 у драйвера не баг, C2H-ABSENT **не связана** с RPWM.
Реализован JOINBSS-движок (host передаёт Auth/AssocReq кадры, не H2C 0x23;
захват BSSID+channel из скана; вызов из wifi_layer с Retry). Осталось:
обработка Auth/AssocResp из RX-пути для подтверждения ассоциации, затем
фикс IP (192.168.x.199) к цели «эмулятор + сетевой доступ к ROM на SD».
Дальнейшие шаги — в `NEXT-CHAT-PROMPT.md`.