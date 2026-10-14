# База знаний: Allwinner H3 (Orange Pi Lite) — bare-metal, звук I2S0 → MAX98357A

> Рабочий документ. Каждый факт помечен источником:
> - **[СТЕНД]** — проверено на нашем стенде (JTAG/FEL/логи);
> - **[LINUX]** — из официального драйвера Linux (sun4i-i2s.c / sun6i-dma);
> - **[РЕФ]** — из референсного репозитория uli/allwinner-bare-metal;
> - **[ИНОЕ]** — из внешней выжимки, НЕ проверено, возможны ошибки;
> - **[ГИПОТЕЗА]** — рабочие догадки, требуют проверки.
>
> Ревизия: 2026-10-11.

---

## 1. Проект

- Каталог: `E:\Giga_1\Bare_linux`.
- Прошивка: `h3_bare.bin` (грузится на 0x40000000 — U-Boot `go` или FEL+SPL).
- Цель: HDMI-меню + звук I2S0 → MAX98357A (кнопки), этап WiFi — подъём RTL8189FTV.
- Состояние:
  - **Звук:** готов и работает (FEL и SD) на DMA (H3 не кэш-когерентен — нужен
    clean D-cache перед стартом DMA; см. H3-BAREMETAL-GUIDE.md §5).
  - **WiFi (рабочая точка W.171):** SDC1-драйвер, SDIO CMD52/CMD53, чтение CIS-ID
    (manf=0x024C, card=0xF179 — RTL8189FTV), PowerSeq CARDEMU→ACTIVE, загрузка
    прошивки (fwdl rc=0, **MCUFWDL=0xC6 = все 4 бита маски драйвера
    MCUFWDL_RDY|FWDL_ChkSum_rpt|WINTINI_RDY|RAM_DL_SEL — 8051 загружена и
    готова**, 0xC6 это НЕ ошибка), чтение eFuse (MAC пуст — FF) и программная
    установка MAC в REG_MACID 0x610-0x615 (подтверждена readback).
    **RF-калибровка LCK работает** (LSSI-доступ к RF-регистрам через
    BB 0x824+edge → 0x8A0, канал читается/пишется). Ключевое: для LSSI обязателен
    полный BB-конфиг (bb_phy_reg[] из halhwimg8188f_bb.c). Подробности в
    HARDWARE-REF.md и H3-BAREMETAL-GUIDE.md §9.
    **RX-путь**: собрана полная RX-инициализация по драйверу
    (LLT → TX-границы → rxff_bndy=0x3EFF → PBP → DRVINFO_SZ=4 → NETTYPE_AP →
    RCR/RXFLTMAP/MAR → RXDMA agg → REG_CR bit8/9 MACRXEN/MACTXEN → HIMR
    32-бит вкл. C2HCMD → RW_RELEASE=0) и диагностика (RX0_REQ_LEN 0x1C,
    RXPKT_NUM 0x287, C2H_EVT 0x01A0, HISR 0x18). **Симптом W.171**: при всей
    конфигурации rxlen=0/pkt=0/hisr=0/c2h=0 — 8051 не генерирует события.
    Движок IQK не сходится (0x142/0x42 — насыщение, маркеры сбоя из драйвера)
    при любых константах/5-и retry/H2C/8051-off — вероятно, без полного
    драйверного окружения (LPS-exit → rf_on → полный RF-конфиг) или FW
    не в рабочем цикле (нет C2H). Следующий шаг — см. NEXT-CHAT-PROMPT.md.

---

## 2. Карта I2S0 (H3/sun8i; база 0x01C22000)

| Смещение | Регистр | Назначение | Значение / биты |
|---|---|---|---|
| 0x00 | I2S_CTL | Управление | BCLK_OUT=18, LRCK_OUT=17, MODE_SEL[5:4], TXEN=2, RXEN=1, GEN=0, SDO0_EN=8 → 0x00060117 [РЕФ] |
| 0x04 | I2S_FMT0 | Формат 0 | SW(3)<<0, SR(3)<<4, LRCK_PERIOD-1<<8, LRCKR<<20 → 0x1F33 [РЕФ] |
| 0x08 | I2S_FMT1 | Формат 1 | 0x0 (PCM без знакового расширения) [РЕФ] |
| 0x0C | I2S_INT_STA | Статус прерываний (sun8i) | [LINUX] |
| 0x10 | I2S_RXFIFO | RX FIFO | — |
| 0x14 | I2S_FCTL | Управление FIFO | TXTL(0x40)<<12, RXTL<<4, TXIM(1)<<2, RXOM<<0, FRX=24, FTX=25 |
| 0x18 | I2S_FSTA | Статус FIFO | TXE=28 (TX FIFO empty), уровень [24:16] [РЕФ] |
| 0x1C | I2S_DMA_INT_CTRL | DMA/IRQ разрешение | TX_DRQ_EN=7, RX_DRQ_EN=3 [LINUX] |
| 0x20 | I2S_FIFO_TX | TX FIFO (запись) | 32-бит [LINUX: reg_offset_txdata=0x20; СТЕНД] |
| 0x24 | I2S_CLK_DIV | Делители тактов | MCLK_EN=8, BCLKDIV<<4, MCLKDIV<<0 [LINUX] |
| 0x28 | I2S_TX_CNT | Счётчик TX сэмплов | [LINUX] |
| 0x2C | I2S_RX_CNT | Счётчик RX сэмплов | [LINUX] |
| 0x30 | I2S_CHAN_CFG | Каналы | RX_SLOT_NUM<<4, TX_SLOT_NUM<<0 [LINUX] |
| 0x34 | I2S_TX_CHAN_SEL | Выбор TX | OFFSET(1)<<12, CHSEL<<0, CHEN<<4 [LINUX] |
| 0x44 | I2S_TX_CHAN_MAP | Карта TX | 0x76543210 [LINUX] |
| 0x54 | I2S_RX_CHAN_SEL | Выбор RX | [LINUX] |
| 0x58 | I2S_RX_CHAN_MAP | Карта RX | [LINUX] |

**Замечание про адреса [СТЕНД+LINUX]:** 0x20 = FIFO_TX (подтверждено и стендом:
DMA писал в 0x01C22020 — звук шёл). 0x24 = CLK_DIV, 0x28 = TX_CNT.
Те, кто пишут «0x20 = INT, 0x24 = TXFIFO», путают с раскладкой A10/sun4i
(TX FIFO там на 0x0C). **Всё выше — для H3/sun8i.**

---

## 3. Ключевые биты I2S_CTL (проверено)

```c
I2S_CTL_BCLK_OUT  (1<<18)   // H3 генерирует BCLK (master)
I2S_CTL_LRCK_OUT  (1<<17)   // H3 генерирует LRCK (master)
I2S_CTL_SDO0_EN   (1<<8)    // выход данных SDO0 (PA20 DOUT)
I2S_CTL_MODE_SEL(n) (n<<4)  // 0=PCM,1=Left(LJ),2=Right(RJ); I2S = LEFT+offset
I2S_CTL_TXEN      (1<<2)
I2S_CTL_RXEN      (1<<1)
I2S_CTL_GEN       (1<<0)    // глобальный enable
```

**Мастер-режим = BCLK_OUT|LRCK_OUT (биты 18/17), НЕ биты 5:6.** [LINUX][РЕФ]
Биты 5:6 — это MODE_SEL (формат), их путают с Master/Slave в некоторых
выжимках. [ИНОЕ — ошибочно]

---

## 4. Тактирование (CCU)

| Регистр | Адрес | Назначение |
|---|---|---|
| PLL_AUDIO_CTRL | 0x01C20008 | PLL аудио 24.576 МГц |
| PLL_AUDIO_PAT_CTRL0 | 0x01C20284 | Pattern (sigma-delta дробь) |
| I2S_PCM0_CLK | 0x01C200B0 | Такт I2S0 (mux PLL_AUDIO) |
| BUS_CLK_GATING2 | 0x01C20068 | I2S0 = бит 12 |
| BUS_SOFT_RST3 | 0x01C202D0 | I2S0 = бит 12 (НЕ 14 — бит14 это I2S2!) [РЕФ] |
| BUS_CLK_GATING0 | 0x01C20060 | DMA = бит 6 |
| BUS_SOFT_RST0 | 0x01C202C0 | DMA = бит 6 |

**Источник такта I2S0 [даташит 4.3.5.28]:** I2S/PCM0_CLK_REG (0x0B0).
- бит31 = SCLK_GATING (0=off,1=on)
- биты[17:16] CLK_SRC_SEL: 00=**PLL_AUDIO(8X)**, 01=(8X)/2, 10=(8X)/4,
  **11=PLL_AUDIO** (1X). Для 48k НЕОБХОДИМ mux=11 (PLL_AUDIO 1X=24.576M).
  (Раньше ставили 00=8X — BCLK был неверным → тишина.)

Схема частот (48 кГц):
- AudioPLL (SDM) = 24.576 МГц
- I2SPCM0_CLK mux=11 → модульный SCLK = 24.576 МГц
- BCLKDIV=5(÷8) → BCLK = 3.072 МГц = 48k × 32 × 2
- MCLKDIV=2(÷2) → MCLK = 12.288 МГц

Наш код инициализации (I2S0):
```c
PLL_AUDIO_PAT  = 0xC000AC02u;
PLL_AUDIO_CTRL = (1<<31)|(1<<24)|(13<<8)|13;   // N=14,M=14,SDM -> 24.576M
udelay(3000);  // ждать bit28 (LOCK)
H3_CCU_GATING2 |= (1u << 12);
CCU_RST_I2S0 &= ~(1<<12); udelay(10);
CCU_RST_I2S0 |=  (1<<12); udelay(10);
I2SPCM0_CLK = (I2SPCM0_CLK & ~(3<<16)) | (3<<16) /*mux 11=PLL_AUDIO*/ | (1<<31);
```

---

## 5. Пины (PIO, PA-порт)

| Пин | Функция | Регистр/биты |
|---|---|---|
| PA0 | JTAG_TMS (F3) | PIO_CFG0 [3:0]=3 |
| PA1 | JTAG_TCK (F3) | [7:4]=3 |
| PA2 | JTAG_TDO (F3) | [11:8]=3 |
| PA3 | JTAG_TDI (F3) | [15:12]=3 |
| PA4 | UART0_TX (F2) | [19:16]=2 |
| PA5 | UART0_RX (F2) | [23:20]=2 |
| PA10 | SD усилителя (GPIO out, HIGH=работа) | PIO_CFG2 [7:4]=1 |
| PA18 | I2S0 LRCK(SYNC) (F2) | PIO_CFG2 [11:8]=2 |
| PA19 | I2S0 BCLK (F2) | PIO_CFG2 [15:12]=2 |
| PA20 | I2S0 DOUT (F2) | PIO_CFG2 [19:16]=2 |

**Важно [СТЕНД]:** запись `PIO_CFG0 = 0x3333` (полная) затирает PA4-5 → UART
умирает. Только RMW: `PIO_CFG0 = (PIO_CFG0 & 0xFFFF0000) | 0x3333`.

---

## 6. U-Boot vs FEL — фактическая разница регистров (снято JTAG [СТЕНД])

### 6a. Чистое состояние ПОСЛЕ U-Boot (SD), до нашей инициализации
Снято прошивкой jtagonly (только включила JTAG-пины PA0-3=0x3333, периферию
не трогала). U-Boot 2025.01. Полный лист — REGISTER-MAP.md (СНИМОК 2).

| Регистр | FEL/эталон (работает) | U-Boot чистый (не работает) | Что делаем мы |
|---|---|---|---|
| GICD_CTLR (0x81000) | 0x3 | **0x0** | gic_init(): =3 |
| GICC_CTLR (0x82000) | 0x3 | **0x0** | gic_init(): =3 |
| PLL_AUDIO_CTRL (0x08) | 0x91000d0d | **0x00035514** (выкл) | enable+lock |
| I2SPCM0_CLK (0xB0) | 0x80030000 | **0x0** (выкл) | bit31=1, mux=11 |
| GATING2 bit12 (I2S0) | 1 | **0** | gate on |
| RST3 bit12 (I2S0) | 1 | **0** | deassert |
| GATING0 bit6 (DMA) | 1 | **0** | gate on |
| RST0 bit6 (DMA) | 1 | **0** | deassert |
| DMA_SEC_REG (0x20) | 0xFFFF | **0x0** (secure) | =0xFFFF |
| SMTA DEC1/DEC2 | FF/7F | FF/7F (уже non-sec) | — (не трогаем) |

**Итог:** U-Boot держит DMA и I2S0 и в gate и в reset, PLL_AUDIO и I2SPCM0_CLK
ВЫКЛЮЧЕНЫ, GIC выключен, DMA-каналы secure. Наша init всё это включает
(проверено: после init все значения сошлись с эталоном FEL).

### 6b. После нашей init в U-Boot-среде — применилось всё, кроме DMA-заполнения
Такты/сбросы/защиты сошлись с FEL. Но:
- I2S0 FSTA = 0x10800000 (FIFO ПУСТ) vs рабочий 0x003c0000 (FIFO польный 60)
- ISTA = 0x54 (TXU_INT = FIFO UNDERRUN)
- при этом DMA-канал 4: EN=1, DESC="SPL", CUR_SRC="eGON" (зомби U-Boot!)

Значит: регистры ок, но FIFO не наполняется — либо DMA-канал занят зомби
загрузчика, либо наш старт не перебивает его. Это и есть корень «гонки».
Решение — dma_reap_stray() + dma_pick_channel() (см. §8).

---

## 7. TrustZone-защита (ПО ДАТАШИТУ H3 V1.2, §4.11.4.5, §4.18) [СТЕНД/ПРОВЕРЕНО]

**Важно:** прежняя запись про "TZPC 0x01C23410-20" была из внешнего референса
и НЕ соответствовала даташиту. На H3 блок 0x01C23400 — это **SMTA**
(Secure Memory Touch Arbiter, §4.18), а не TZPC. Регистры порта:

```
SMTA_DECPORT0: STA 0x04, SET 0x08, CLR 0x0C
SMTA_DECPORT1: STA 0x10, SET 0x14, CLR 0x18   <- bit1=DMA, bit3=Crypto, bit4=USB
SMTA_DECPORT2: STA 0x1C, SET 0x20, CLR 0x24   <- bit3=CCU, bit2=SysCtl, bit6=RTC
```
STA=RO (0=secure,1=non-secure). SET=WO (1->non-secure), CLR=WO (1->secure).

Плюс **DMA_SECURE_REG** (DMA база + 0x20): бит канала, 0=Secure,1=Non-secure.

**Снять защиты для звука (secure.c):**
```c
DMA_SECURE_REG |= 0xF;        /* каналы DMA0-3 non-secure */
SMTA_DEC1_SET  |= (1<<1);     /* DMA non-secure */
SMTA_DEC2_SET  |= (1<<3);     /* CCU non-secure */
```
U-Boot может вести CPU в Non-Secure → Secure-каналы DMA и CCU недоступны.

**Глобальные регистры прерываний:** U-Boot гасит GIC. Карточно нужен
`gic_init()` (distributor 0x01C81000: сброс enable/pending/active, target CPU0,
включить группы 0/1; CPU interface 0x01C82000: PMR=0xFF, CTL=grp0|grp1). Сделан
в src/gic.c. Для tone с DMA-IRQ внешние irq не обязательны, но GIC инициализация
обязательна чтобы вообще не было мусора.

---

## 8. DMA sun6i (что проверено [СТЕНД])

- База 0x01C02000; канал n: регистры +0x100 + n*0x40.
- LLI = 6 слов (cfg, src, dst, len, para, next).
- Конец одноразового списка: `next = 0xfffff800`.
- Кольцо: next1 → addr(desc2), next2 → addr(desc1) (без 0xfffff800).
- **Выравнивание дескриптора: строго 32-байт** (правило sunxi-dma) [ИНОЕ+СТЕНД].
- DRQ: SDRAM=1, I2S0_TX=3.
- Запуск: gate bit6 → деассерт reset bit6 → LLI в uncached RAM → CH_LLI=addr →
  CH_EN=1.
- CFG для memcpy 0x04c104c1; I2S TX (RAM→FIFO): SRC 32b burst4, DST 32b burst1
  DRQ=3.

**Критично [СТЕНД]:** один самозацикленный LLI на весь буфер — **молчит**.
Два LLI по полбуфера — работают (звук был). Проверено достоверно.

### Важно: 0x01C02020 — это DMA_SEC_REG, НЕ soft-reset [даташит 4.11.4.5]
- Регистр 0x20 DMA — **DMA_SEC_REG** (бит на канал: 0=Secure,1=Non-secure).
- НЕ путать с «soft-reset». Мягкий сброс DMAC делается через CCU 
  BUS_SOFT_RESET0 bit6. Но в SD-режиме это РИСКОВАННО (см.ниже).

### Осиротевшие DMA-каналы после U-Boot — корень «то рычание, то тишина» [СТЕНД]
- После `go` код U-Boot не исполняется, НО его включённый DMA-канал — автономный
  агент: продолжает возить данные в DRAM, пока не остановят. В JTAG-дампе видно
  канал 4: EN=1, DESC потом "SPL", CUR_SRC="eGON" — остаток чтения eGON-образа.
- Такой «зомби» конфликтует с нашим LLI/буфером -> гонка: перебил чужое =
  звук (рычание), не перебил = тишина.
- Лечение (`dma_reap_stray()` в h3_dma.c): гасить ТОЛЬКО канальными регистрами
  (EN=0, PAU=0, ждать busy=0 в DMA_STA с таймаутом, W1C по 2 битам канала),
  БЕЗ CCU-сброса и БЕЗ трогания глобальных IRQ_EN. USB имеет собственный
  HC-DMA (не общий 0x01C02000), HDMI — регистры DE2/TCON: их не задеваем.
- После reap выбираем канал через dma_pick_channel() (сначала предпочт., потом
  любой свободный по DMA_STA), а не жестко канал 4.

### ВАЖНО: в SD-режиме канал 4 ПРОДОЛЖАЕТ жить дескриптором SPL [СТЕНД, JTAG]
- JTAG-снимок всех 12 каналов в U-Boot: канал 4 = EN=1, DESC=0x034c5053("SPL"),
  CFG=0xea000016, CUR_SRC=0x4e4f4765("eGON"). Он стабильно держится между
  снимками и ИГНОРИРУЕТ наши записи DESC_ADDR/EN -> звук на канале 4 не идёт.
- Каналы 5..11 полностью свободны (все 0). Поэтому dma_pick_channel() теперь
  выбирает СНАЧАЛА 5..11 (выше SPL-диапазона), затем 0..4. Ожидаем ровный звук
  в U-Boot на канале 5+.
- Замечание: реестра 0x20 DMA = DMA_SEC_REG (не soft-reset). Аналогично в FEL
  DMA стартует В RESET (RST0 bit6=0) — нужен условный deassert, иначе регистры
  DMA (SEC/AUTOGATE/каналы) не отвечают на запись (см. §ниже "FEL deassert").

---

## 9. I2S TX FIFO формат — TXIM (КРИТИЧНО, [даташит 8.6.7.6])

- FIFO пишется 32-бит словами. **Пара = [L<<16][R<<16]** (сэмпл в СТАРШИХ битах).
- **TXIM (FCTL bit2) должен быть 0** («данные в MSB»). При TXIM=1 FIFO берёт
  младшие 16 бит (0) → тишина на выходе. Это была ОДНА из причин.
- TXTL (FCTL[18:12]) = 0x40 → trigger level 64 (DRQ просит FIFO когда
  empty-place > 64).

## 10. Правильный порядок плайбека [LINUX sun4i_i2s_start_playback]

При каждом старте (после init):
```c
I2S_FCTL |= (1<<25);      // flush TX FIFO
I2S_TXCNT  = 0;           // сброс TX counter
I2S_CTL  |= TX_EN;        // TX block enable
I2S_INT  |= (1<<7);       // TX DRQ enable
```
I2S_CTL включает BCLK_OUT|LRCK_OUT|SDO0_EN|MODE_LEFT(1)|TX_EN|GL_EN.
stop: I2S_CTL &= ~TX_EN; I2S_INT &= ~TX_DRQ.

## 11. Корректность DMA (без прерываний)

При DMA с LLI прерывания можно НЕ включать (IRQ_EN=0): pending-биты не
блокируют канал. `IRQ_PEND=0xFFFFFFFF` в главном цикле (старый i2s_tick)
был ВРЕДЕН — снимал все каналы и мог ломать U-Boot DMA. Убран.

**LLI обязан лежать в uncached.** В linker.ld .coherent (g_lli) вынесен на
0x40100000 (своя 1МБ-секция), .dma_buf на 0x40200000, обе uncached через
mmu_mark_uncached(). НЕ помещать g_lli в секцию кода (0x40000000) — DMAC
читал стэйл-данные кэша.

## 12. Сравнение H3 vs H313/H616 (для будущего) [ИНОЕ]

| Параметр | H3 (OPi Lite) | H313/H616 (TV-box) |
|---|---|---|
| CPU | Cortex-A7 ×4 (ARMv7) | Cortex-A53 ×4 (ARMv8-A) |
| Тулчейн | arm-none-eabi (`-mcpu=cortex-a7`) | aarch64-none-elf (`-march=armv8-a`) |
| CCU | 0x01C20000 | 0x03001000 |
| PIO | 0x01C20800 | 0x0300B000 |
| UART0 | 0x01C28000 | 0x05000000 |
| GIC | 0x01C81000/0x01C82000 (GIC-400) | 0x03021000/0x03022000 |
| Старт RAM | 0x40000000 | 0x40000000 |
| I2S | 0x01C22000 | 0x05060000 |

---

## 11. Комьюнити / источники [ИНОЕ]

- **IRC `#linux-sunxi`** на Libera.chat — главный оплот (разработчики U-Boot/Linux).
- **GitHub**: `uli/allwinner-bare-metal` (MIT, фреймворк: audio_i2s.c, dma.c,
  mmu.c), форки utmux/dwelch67.
- **Форум Armbian** — темы по H3/H616, тактированию, GICv2.
- **Китайские**: Gitee, CSDN, EEFocus — по sun50iw9/H616 дисплей и DMA.

---

## 13. Открытые вопросы / TODO (этап WiFi)

- [x] Загрузка прошивки чипа: `h3_wifi_download_fw` — **работает (fwdl rc=0, W.107+)**.
- [x] PowerSeq CARDEMU→ACTIVE до включения Fn1 — **работает (pwon rc=0)**.
- [x] eFuse: MAC пуст (FF) на Orange Pi Lite, задаётся программно в REG_MACID.
- [x] **RF включение + LSSI-доступ + калибровка LCK** — работают (W.136): канал
      читается/пишется через LSSI (BB 0x824+edge → 0x8A0), RF 0x18 дефолт 0x400.
- [ ] IQK (калибровка квадратур) — следующий шаг (phy_IQCalibrate_8188F, path A).
- [ ] После калибровки: инициализация MAC/RF, канал и RX-путь (RCR/MAR, приём
      beacon). Это этап RX/TX.
- [ ] Далее: 802.11 (probe/scan), ассоциация, WPA2, TCP/IP, SMB/HTTP.
- [ ] Прошивку стоит грузить с SD-карты, а не встраивать в бинарь (для компактности
      образа и переносимости).
- [ ] sunxi-fel собран и живёт в `tools/fel/` (из исходников sunxi-tools).

## 14. Уроки (не наступать дважды)

1. Полная запись `PIO_CFG0=0x3333` убивает UART0-TX (PA4) — только RMW. [СТЕНД]
2. Один LLI на H3 молчит; два LLI — работают. [СТЕНД]
3. U-Boot гасит GIC И держит DMA/I2S0 в gate+reset, PLL и I2SPCM0_CLK выкл. [СТЕНД/JTAG]
4. Диагностировать регистры ДО нашей инициализации, а не после. [СТЕНД]
5. JTAG = PA0-3 (F3, 0x3333). Данные про PF — ошибочны. [СТЕНД]
6. 0x20 I2S = TXFIFO; 0x20 DMA = DMA_SEC_REG (не soft-reset). [СТЕНД]+[LINUX]+[даташит]
7. **LLI DMA НЕ в секцию кода** — выносить в uncached 1МБ-секцию. [даташит]
8. **TXIM=0** (данные в MSB) — иначе тишина при формате [L<<16][R<<16]. [даташит]
9. **I2SPCM0_CLK mux=PLL_AUDIO (не 8x)** — иначе неверный BCLK. [даташит+Linux]
10. **После U-Boot `go` есть осиротевшие DMA-каналы** — гасить только канальными
    регистрами + W1C, без CCU-сброса и без 0xFFFFFFFF. [СТЕНД/JTAG]
11. **openocd по конфигу делает halt и НЕ resume** — «зависания платы» при отладке
    часто мои: всегда после снятия карты делать `resume`+`shutdown`.
12. Всё нужное для проекта хранить в папке проекта (temp-каталог gigatool чистится).
    sunxi-fel собран и живёт в `tools/fel/` (из исходников sunxi-tools).
13. **Для data-команд SDC1 НЕ взводит CMD_DONE** — ждать DATA_DONE
    (TRANS_COMPLETE), иначе блок-операция возвращает -2 с risr=0. [СТЕНД]
14. **Шина MMC1 — OLD mode** (бит30=0 в SDMMC1_CLK_REG); MODE_NEW не даёт ответа
    карты. [СТЕНД]
15. **SDIO-адресация двух доменов**: SDIO-local регистры — device id 0, WLAN IOREG —
    device id 8. Смешивание даёт неверные значения (0xEA). [СТЕНД]
16. **Функция 0 (CCCR/CIS) не поддерживает блочный CMD53** — тюплы читать по
    байтам через CMD52. Блочный CMD53 — только для функции 1 (WLAN IOREG/FW). [СТЕНД]
17. **Для CMD52/CMD53 CRC ответа НЕ проверять** (SDIO-команды) — иначе ложная
    ошибка CRC. [СТЕНД]
18. **PowerSeq CARDEMU→ACTIVE обязателен ДО включения Fn1 и ДО загрузки FW**:
    без него CMD53-write к функции 1 срывается (rc=-2, risr=0), хотя CMD52
    проходят. Порядок: CMD5/3/7 → pwon → fn1(CCCR 0x02) → fwdl. [СТЕНД W.106→W.107]
19. **CMD53 byte-mode: хвост (total не кратен 4) надо явно дописывать/дочитывать.**
    Write: FIFO требует ВСЕ байты BYCNT — остаток писать упакованным словом.
    Read: для len<4 FIFO пуст в момент проверки — ждать данные (sdc_wait
    FIFO_EMPTY=0), иначе хвост читается нулями. [СТЕНД W.115→W.116]
20. **CMD52 write маска данных — `val & 0xFF`, НЕ `0x7F`** (терялся 8-й бит). [КОД]
21. **Побайтовое чтение через CMD53 (len=1 на каждый байт) даёт сдвиг/дубли** —
    читать одним CMD53 по 4 байта (или 2 по явному адресу, inc=0). [СТЕНД W.113→W.114]
22. **Инкремент адреса (bit26) CMD53 в byte mode глючит на границе 4 байт**
    (REG_MACID 0x610/0x614): надёжнее раздельные CMD53 с inc=0 на явные адреса. [СТЕНД W.114→W.115]
23. **Для RF-доступа (LSSI) обязателен полный BB-конфиг** (bb_phy_reg[] из
    halhwimg8188f_bb.c, 209 записей): он настраивает RF-интерфейс (0x840 LSSI
    mode, 0x860/0x864/0x870). Без него LSSI-readback = 0. [СТЕНД W.133→W.136]
24. **LSSI-чтение 8188F**: адрес RF в BB 0x824 [30:23] (сдвиг 23, не 20!),
    edge-триггер bit31 (снять → поднять), пауза, readback из BB 0x8A0 (LSSI)
    или 0x8B8 (PI, если bit8 в 0x820). Запись: BB 0x840 = (off<<20)|data. [СТЕНД]
25. **Порядок включения BB/RF (PHY_BBConfig8188F)**: SYS_FUNC_EN|=BIT13|BIT0|BIT1
    → сброс BB (0x16→0x14) → RF_CTRL(0x1F)=0x07 → RF reg1=0x780 (power-on ПОСЛЕ
    сброса!) → SYS_FUNC_EN=0x17 → REG_CR=0x63F → BB-конфиг. [СТЕНД W.135→W.136]
26. **После LCK бит 15 в RF_CHNLBW может остаться 1** — это норма для 8188F:
    драйвер не поллит сброс, а ждёт 100 мс и выключает LDO (RF 0xB0=0xDFFF0). [СТЕНД]

---

## Приложение: JTAG-подключение

```
openocd -f tools/jtag/h3_pico_jtag.cfg    # CMSIS-DAP, 500 кГц, TAP 0x5ba00477
```
- Чтение регистров: `mdw <addr> <count>` через telnet :4444.
- ВАЖНО: конфиг делает `halt`; после чтения всегда `resume` + `shutdown`, иначе
  плата «зависает» (она не виснет — это мой halt).
- Для снятия чистой карты после U-Boot — прошивка jtagonly (только JTAG-пины).

Распиновка: PA0=TMS(pin13→GP14), PA1=TCK(11→GP19), PA2=TDO(22→GP21),
PA3=TDI(15→GP18), GND(pin6). Скорость 500 кГц (выше — нестабильно).