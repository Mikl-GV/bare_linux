# Аппаратная справка (H3 / Orange Pi Lite) — проверено на стенде

> Источник: `E:\Giga_1\pico-retro-new-gen` (референс, только чтение) + H3 Datasheet.
> Значения помечены как проверенные на стенде; всё остальное — из даташита.
> Это справочник для написания чистого кода, НЕ копируемый модуль.

## Базы периферии

| Блок | Адрес | Примечание |
|---|---|---|
| CCU | 0x01C20000 | такт/сбросы |
| PIO (GPIO) | 0x01C20800 | порты A..H |
| R_PIO / PRCM | 0x01F01400 | always-on, порт L |
| UART0 | 0x01C28000 | PA4=TX PA5=RX, 115200 |
| DMAC | 0x01C02000 | sunxi-DMA, канал +0x100+N*0x40 |
| I2S0 | 0x01C22000 | звук MAX98357A |
| GIC dist | 0x01C81000 | |
| GIC cpu | 0x01C82000 | banked, каждый CPU видит свои |
| USB OHCI1 | 0x01C1B400 | |
| USB OHCI2 | 0x01C1C400 | |
| MMС0 (SD) | 0x01C0F000 | SD-карта |
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