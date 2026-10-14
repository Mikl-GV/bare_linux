# Аппаратная справка (H3 / Orange Pi Lite) — проверено на стенде

> Источник: `E:\Giga_1\pico-retro-new-gen` (референс, только чтение) + H3 Datasheet.
> Значения помечены как проверенные на стенде; всё остальное — из даташита.
> Это справочник для написания чистого кода, НЕ копируемый модуль.

## Базы периферии

| Блок | Адрес | Примечание |
|---|---|---|
| CCU | 0x01C20000 | такт/сбросы |
| PIO (GPIO) | 0x01C20800 | порты A..H |
| R_PIO | 0x01F02C00 | always-on, порт L (WIFI_EN=PL7) |
| PRCM | 0x01F01400 | always-on RCU (R_REG) |
| UART0 | 0x01C28000 | PA4=TX PA5=RX, 115200 |
| DMAC | 0x01C02000 | sunxi-DMA, канал +0x100+N*0x40 |
| I2S0 | 0x01C22000 | звук MAX98357A |
| GIC dist | 0x01C81000 | |
| GIC cpu | 0x01C82000 | banked, каждый CPU видит свои |
| USB OHCI1 | 0x01C1B400 | |
| USB OHCI2 | 0x01C1C400 | |
| MMC0 (SD) | 0x01C0F000 | SD-карта |
| MMC1 (SDC1) | 0x01C10000 | WiFi-чип RTL8189FTV (SDIO, PG0-PG5) |
| R_CPUCFG | 0x01F01C00 | запуск вторичных ядер |
| SRAM A1 | 0x00000000 | трамплин CPU1..3 |

## Память (карта)

| Адрес | Назначение |
|---|---|
| 0x40000000 | код (U-Boot `go`) |
| 0x50000000 | буфер загрузки ROM с SD |
| 0x5F800000 | emu-кадровый буфер 320x240 RGB565 |
| 0x5F900000 | HDMI-кадровый буфер 1024x600 XRGB8888 |
| ~0x60000000 | стек (растёт вниз от верха DRAM) |

## Звук (I2S0 → MAX98357A), проверено на стенде

Пины: PA18=SYNC(LRC), PA19=CLK(BCLK), PA20=DOUT(DIN) — функция 2 в CFG2.
PA10=SD усилителя (output, HIGH=работа).

- PLL_AUDIO 24.576 МГц: `PLL_AUDIO_CTRL=(1<<31)|(1<<24)|((14-1)<<8)|(14-1)`; `PLL_AUDIO_PAT=0xC000AC02`; ждать lock bit28.
- CCU: `BUS_CLK_GATING2 |= (1<<12)` (I2S0), reset `CCU_BASE+0x2D0 |= (1<<12)`, `I2SPCM0_CLK=(0x0B0)` биты [17:16]=3 (mux pll-audio-8x), бит31=1.
- I2S0 регистры: CTRL 0x00 (BCLK_OUT=bit18, LRCK_OUT=bit17, TX_EN=bit2, SDO_EN0=bit8, GL_EN=bit0), FMT0 0x04 (SR=3 16bit<<4, SW=7 32слот<<0, LRCKPER=32 через (32-1)<<8), CLK_DIV 0x24 (MCLK_EN=bit8, BCLK_DIV=5<<4, MCLK_DIV=2<<0), CHAN_CFG 0x30 (TXSLOT=2), TX_CSEL 0x34, TX_CMAP 0x44=0x76543210, FIFO_TX 0x20 (пары: L<<16|...см. ниже), FIFO_CTL 0x14 (TXTL=0x40<<12), INT 0x1C (TX_DRQ=bit7).
- Формат DMA-буфера: 32-бит слова, сэмпл в СТАРШИХ битах. Пара = 2 слова `[L<<16][R<<16]`.
- Скорости: LRCK=48 кГц; BCLK=3.072 МГц (PLL/8); MCLK=PLL/2.

## DMA (sunxi-DMA → I2S TX), проверено на стенде

- База 0x01C02000. Канал N: `+0x100 + N*0x40`:
  EN 0x00, PAU 0x04, LLI 0x08, CFG 0x0C(RO), CUR_SRC 0x10(RO), PARA 0x1C.
  DMA_STA 0x30 (bitN=канал busy), DMA_IRQ_EN 0x00, DMA_IRQ_PEND 0x10, DMA_AUTO_GATE 0x28 (=0x4), DMA_PKG_NUM 0x130.
- LLI-дескриптор: 6 слов — cfg,src,dst,len,para,next. 2 циклических; `next` последнего → первый.
- CFG-поля H3: SRC_WIDTH[10:9], SRC_BURST[7:6], SRC_DRQ[4:0], SRC_MODE[5], DST_WIDTH[26:25], DST_BURST[23:22], DST_DRQ[20:16], DST_MODE[21].
- DRQ: SDRAM=1, I2S0_TX=3=0x01C22020.
- Ширина 32бита=2, burst4=1, burst1=0; IO-mode=1 для DST.
- Критично:
  - Полный сброс DMAC перед настройкой (BUS_SOFT_RESET0 bit6: and-then-or).
  - `DMA_IRQ_PEND=0xFFFFFFFF` перед стартом канала (иначе канал замирает).
  - LLI обязан лежать в uncached-памяти (иначе stale кэш).
- INTID аудио-DMA = **82** (= 32+SPI50) — НЕ 32+82! OPHR = SPI 50.
- PKG-прерывание: `DMA_IRQ_EN |= (1<<1) << (ch*4)`.

## USB

OHCI1/OHCI2. Для клавиатуры: USB HID boot protocol. USB Audio (внешний ЦАП) — класс UAC.

## Запуск вторичных ядер (H3, проверено)

- R_CPUCFG 0x01F01C00: RST_CTRL(cpu)=+0x40+cpu*0x40, CTRL=+0x44, STATUS=+0x48.
- Трамплин в SRAM A1 (0x00000000): 5 слов — sp, entry, bx. После записи flush c7,c10,1 + dsb.
- Запуск: RST_CTRL=0 (удержать) → udelay → RST_CTRL=3 (release). Ждать magic в SRAM.
- В non-secure (PSCI u-boot): через SMC PSCI_CPU_ON 0x84000003.

## JTAG H3 (проверено по даташиту H3 Table 3-2)

> Важно: это ПРАВИЛЬНАЯ распиновка из даташита. В интернете встречается
> неверная инфа (PA21=JTAG_TCK) — PA21 НЕ JTAG (это PCM0_DIN). Не путать.

| Пин H3 | Function (Alt) | JTAG-сигнал |
|---|---|---|
| PA0 | Function 3 | JTAG_MS (TMS) |
| PA1 | Function 3 | JTAG_CK (TCK) |
| PA2 | Function 3 | JTAG_DO (TDO) |
| PA3 | Function 3 | JTAG_DI (TDI) |

- Все 4 пина — в CFG0 порта A (PIO 0x01C20800), Function 3 (значение 0x3 на пин).
- Коммутация: `CFG0 = (CFG0 & 0xFFFF0000) | 0x00003333`.
- H3 НЕ включает JTAG сам по себе — пины надо переключить кодом (в boot.s)
  до подключения отладчика.
- Отладчик: JTAGprobe (Raspberry Pi Pico, CMSIS-DAP). Прошивка — в `tools/jtag/`.
  Конфиг OpenOCD — `tools/jtag/h3_pico_jtag.cfg`, IDCODE DAP 0x5ba00477.
- Полная проверенная инструкция — `tools/jtag/JTAG-SETUP.md`.

## WiFi (RTL8189FTV на SDC1), проверено на стенде

Чип RTL8189FTV (Realtek, SDIO) подключён к контроллеру SDC1 (MMC1) H3.

### Питание и пины

- **WIFI_EN = PL7** (R_PIO data 0x01F02C10, бит 7, активный high). Чип питается от
  3.3 В постоянно; энубл-сигнал поднимаем на старте (power-cycle PL7).
- **SDC1 на PG0-PG5** (function 2): PG0=CLK, PG1=CMD, PG2=D0, PG3=D1, PG4=D2, PG5=D3.
  Настройка: `PORTG CFG0 [15:0] = 0x2222`, `CFG1 [7:0] = 0x22` (код в h3_sdc1_init).

### SDIO-протокол (проверено на стенде, рабочая точка W.136)

- Порядок инициализации чипа (строго, подтверждён на стенде):
  `CMD5 (ждём C=1) → CMD3 (RCA) → CMD7 (select) → CMD52 вкл. Fn1 (CCCR 0x02) →
  CMD53 загрузка FW (byte-mode по 4 байта, адрес (8<<13)|0x1000) → eFuse →
  программный MAC в REG_MACID 0x610-0x615`.
  PowerSeq CARDEMU→ACTIVE (`h3_wifi_power_on_chip`) выполняется **ДО включения
  Fn1** — без него CMD53-write к функции 1 срывается (rc=-2, risr=0).
- Чистая I/O-карта: **CMD0/CMD8/ACMD41 не используются** (по спецификации SDIO).
- Рабочая ширина на этом этапе — **1-bit** (перед FW принудительно ставится 1-bit).
  4-bit: CCCR 0x07 [2:0] = 0b01 + `SDC_CTYPE=1` (по SDIO-спецификации); на стенде
  не используется до подтверждённого RX.
- CIS: адрес из CCCR 0x09-0x0B = 0x1000, тюпл 0x20 → Manufacturer 0x024C,
  Card/PID 0xF179 (RTL8189FTV).
- ID чипа печатается как `chip_id manf=0x024C card=0xF179`.
- MAC: eFuse по 0x11A на Orange Pi Lite пуст (FF) → задаётся программно
  (локально-администрируемый 02:00:00:XX:XX:XX) в REG_MACID 0x610-0x615
  через CMD53 без инкремента (0x610 len=4, 0x614 len=2).

### RF-доступ и калибровка (W.136, работает)

- **LSSI-запись RF** (Path A): BB 0x840 = `(rf_off << 20) | (data & 0xFFFFF)`.
- **LSSI-чтение RF**: BB 0x824 (`rFPGA0_XA_HSSIParameter2`) — адрес в [30:23]
  (сдвиг 23, НЕ 20), edge-триггер bit31 (записать со снятым → поднятым edge),
  пауза ~100 мкс, readback из BB **0x8A0** (LSSI) или 0x8B8 (PI, если bit8 в
  0x820=1); значение = readback & 0xFFFFF.
- **ВАЖНО**: LSSI работает только после загрузки полного BB-конфига
  (`bb_phy_reg[]` из `halhwimg8188f_bb.c`, 209 записей) — он настраивает
  RF-интерфейс (0x840 LSSI mode, 0x860/0x864/0x870 SW control). Без него
  readback = 0.
- **Включение BB/RF** (PHY_BBConfig8188F): SYS_FUNC_EN(0x02) |= BIT13|BIT0|BIT1
  → сброс BB (байт0=0x16 → 0x14) → REG_RF_CTRL(0x1F)=0x07 (RF_EN|RF_RSTB|
  RF_SDMRSTB) → RF reg 1 = 0x780 (power-on, ПОСЛЕ сброса!) → SYS_FUNC_EN=0x17
  → REG_CR(0x100)=0x63F → BB-конфиг.
- **LCK** (`h3_wifi_rf_lck`, по phy_LCCalibrate_8188F): TXPAUSE(0x522)=0xFF →
  RF reg 0xB0=0xDFBF0 (LDO ON) → канал 1 в RF_CHNLBW(0x18)[7:0] →
  RF_CHNLBW |= 0x8000 (бит 15 trigger) → 100 мс → RF 0xB0=0xDFFF0 (LDO OFF) →
  TXPAUSE=0x00. Бит 15 после LCK может остаться 1 — норма для 8188F.
- RF_CHNLBW дефолт 0x400 (бит 10 = полоса 20/40М), канал в [7:0].

### RX-инициализация (W.171, по rtl8188f_hal_init/sdio_halinit) 

Порядок (проверен, все адреса из драйвера 8188F):

1. **LLT auto-init** (`rtl8188f_InitLLTTable`): `0x0224 |= BIT16`, поллинг сброса (до 1 с).
2. **TX-границы** (`_InitTxBufferBoundary`): `txpktbuf_bndy=0xF1` = TX_PAGE_BOUNDARY_8188F
   (0xFF − BCNQ 0x08 − BCNQ1 0x00 − WOWLAN 0x07 + 1) в 0x0424, 0x0425, 0x045D, 0x0114.
3. **RX-граница** (`_InitPageBoundary`): `0x0116 = 0x3EFF` (16-бит) = RX_DMA_BOUNDARY_8188F
   (0x4000 − 0x100 − 1). ВАЖНО: 0x0116, не 0x0115.
4. **PBP** (`_InitTransferPageSize`): `0x0104 = 0x11` (PBP_128|PBP_128).
5. **DRVINFO_SZ** (`_InitDriverInfoSize`): `0x060F = 0x04` (единицы 8 байт).
6. **NETTYPE_AP** (`_InitNetworkType`): `REG_CR[17:16] = 0x2` (32-бит RMW!).
7. **RCR/RRSR/RXFLTMAP/MAR** (`_InitWMACSetting`): 0x0608=0x7000208F (AAP|APP_PHYST_RXFF|
   APP_ICV|APP_MIC|AMF|CBSSID...), 0x0440=0x000F0F (скорости), 0x06A0/0x06A2/0x06A4,
   0x0620/0x0624=0xFFFFFFFF.
8. **RXDMA** (`_initSdioAggregationSetting`): `0x010C |= BIT2` (RXDMA_AGG_EN),
   `0x0290 |= BIT1` (RXDMA_AGG_MODE_EN).
9. **REG_CR MAC-блок** (sdio_halinit.c:1240 «Enable MACTXEN/MACRXEN block»):
   `0x0100 |= BIT6|BIT7|BIT8(MACTXEN)|BIT9(MACRXEN)` — БЕЗ bit8/9 802.11-MAC не запущен,
   RX невозможен в принципе. (у нас из BB-конфига REG_CR=0x63F без них).
10. **SDIO HIMR** (0x14, fn0, 32-бит!): байт 0x14 = 0x07 (RX_REQUEST|AVAL|TXERR),
    байт 0x16 = 0x02 (C2HCMD BIT17). HISR (0x18) = 0xFF (clear).
11. **RW_RELEASE_EN**: `0x287 &= ~BIT18` (release RX DMA; BIT18=1 — пауза DMA).
12. **Канал**: RF_CHNLBW(0x18) = 0x401 (канал 1, BW 20М; НЕ 0x8C01 — там бит 15=rежим калибровки).

### Индикаторы RX (диагностика W.171)

- **RX0_REQ_LEN**: `0x1C` (fn0) — длина ожидающего RX-запроса (16-бит); >0 = данные в FIFO.
- **RXPKT_NUM**: `0x287` (device 8) — число пакетов в RXPKTBUF (FW), биты [7:0].
  `0x02000000` в 32-бит = BIT25 (не счётчик! счётчик в младших битах).
- **C2H_EVT**: `0x01A0` — событие firmware→host (0 = FW не генерирует).
- **HISR**: `0x18` (fn0) — статус SDIO-прерываний (bit0 RX_REQUEST).
- **MCUFWDL**: `0x0080` — `0xC6` = MCUFWDL_RDY|FWDL_ChkSum_rpt|WINTINI_RDY|RAM_DL_SEL
  — **ШТАТНОЕ состояние загруженной и готовой 8051** (маска value_expected драйвера).
  Бит 2 = FWDL_ChkSum_rpt (подтверждение ЧК), НЕ ошибка!

### Важные особенности (из отладки)

- Такт MMC1: **OLD mode** (бит30=0), НЕ MODE_NEW — иначе карта не отвечает.
  `SDMMC1_CLK_REG` (0x01C2008C) = SCLK_GATING | SRC_OSC24M | (2<<16) | (15<<0)
  ≈ 750 кГц, затем CKC немая делитель (DIV=1 → ~375 кГц на инициализации).
- Для SDIO-команд CRC ответа НЕ проверяется (CMD52/CMD53).
- Для data-команд контроллер НЕ взводит CMD_DONE — завершение ждём по
  DATA_DONE (TRANS_COMPLETE). Без этого блочная запись/чтение «зависали».
- SDIO-локальные регистры чипа (0x100, 0x86) лежат по device id **0**;
  регистры WLAN IOREG (MAC 0x05, 0x06, 0xF0) — по device id **8**.
  SDIO-адрес команды = `(device_id << 13) | (offset & mask)`.

Документация по чипу и драйверу — в `tools/wifi/HARDWARE-WIFI.md`, референсный
драйвер Linux — в `tools/wifi/rtl8189fs/` (не включается в репозиторий).