# HARDWARE.md — аппаратная справка Allwinner H3 (Orange Pi Lite)

> Консолидация бывших `KNOWLEDGE.md` + `HARDWARE-REF.md` + `HARDWARE-WIFI.md` +
> `REGISTER-MAP.md`. Проверено на стенде (JTAG/FEL/логи) — помечено **[СТЕНД]**;
> из даташита H3 V1.2 — **[ДАТАШИТ]**; из Linux-драйверов — **[LINUX]**; гипотезы — **[ГИПОТЕЗА]**.
> Ревизия: 2026-10-21.

## 1. Карта периферии (базы)

| Блок | Адрес | Примечание |
|---|---|---|
| CCU | 0x01C20000 | такты/сбросы |
| PIO (GPIO) | 0x01C20800 | порты A..H |
| R_PIO | 0x01F02C00 | always-on, порт L (WIFI_EN=PL7) |
| PRCM | 0x01F01400 | always-on RCU |
| UART0 | 0x01C28000 | PA4=TX PA5=RX, 115200 |
| DMAC | 0x01C02000 | sunxi-DMA, канал +0x100+N*0x40 |
| I2S0 | 0x01C22000 | звук MAX98357A (PA18/19/20) |
| SMTA | 0x01C23400 | Secure Memory Touch Arbiter |
| GIC dist / cpu | 0x01C81000 / 0x01C82000 | GIC-400 |
| MMC0 (SD) | 0x01C0F000 | SD-карта |
| MMC1 (SDC1) | 0x01C10000 | WiFi RTL8189FTV (PG0-PG5) |
| USB0 (OTG/MUSB) | 0x01C19000 | FEL-кабель, НЕ трогаем |
| PHY1: EHCI / OHCI | 0x01C1B000 / 0x01C1B400 | флешка (DP1/DM1) |
| PHY2: EHCI / OHCI | 0x01C1C000 / 0x01C1C400 | клавиатура AVATTO i8 |
| (EHCI0/OHCI0, OTG) | 0x01C1A000 / 0x01C1A400 | не используется |
| USB PHY ctrl | 0x01C19400 | PHYCTL (A33) |
| pmu1 / pmu2 | 0x01C1B800 / 0x01C1C800 | passby 0x701; SIDDQ в +0x10 bit1 |
| HSTMR | 0x01C60000 | 64-bit счётчик вниз (~97 МГц) |
| SRAM A1 | 0x00000000 | трамплин CPU1..3 |

## 2. Память (линкер)

| Адрес | Назначение |
|---|---|
| 0x40000000 | код (U-Boot `go` 0x40000000 / FEL) |
| 0x40100000 | `.coherent` — LLI DMA (название условное, маппится cacheable, спасает clean) |
| 0x40200000 | `.dma_buf` — кольцо звука 64 КБ |
| 0x40300000 | `.fb_hdmi` — резервный FB 2.4 МБ (в FEL не подключён к DE) |
| 0x5F800000 | emu-FB 320x240 RGB565 (задел) |
| 0x5F900000 | HDMI-FB 1024x600 XRGB8888 (U-Boot) |
| ~0x60000000 | стек (растёт вниз от верха DRAM) |

## 3. Звук: I2S0 → MAX98357A

Пины: PA18=SYNC(LRC), PA19=CLK(BCLK), PA20=DOUT — function 2 в CFG2.
PA10=SD усилителя (output, HIGH=работа).

### Ключевые регистры (0x01C22000)

| Смещение | Регистр | Значение на стенде [СТЕНД] |
|---|---|---|
| 0x00 | CTL | 0x00060115 — BCLK_OUT(18), LRCK_OUT(17), SDO0_EN(8), MODE_LEFT(1<<4), TX_EN(2), GL_EN(0) |
| 0x04 | FMT0 | 0x00001F37 — LRCKPER=32((32-1)<<8), SR=3(16bit<<4), SW=7(32bit) |
| 0x08 | FMT1 | 0x0 — SEXT=0 (LSB pad) |
| 0x0C | ISTA | 0x44 |
| 0x14 | FCTL | 0x000400F0 — TXTL=64(0x40<<12), **TXIM=0** (данные в MSB, критично) |
| 0x18 | FSTA | 0x003C0000 — уровень TX FIFO (~60) при играющем DMA |
| 0x1C | INT | TX_DRQ(7) при старте; после — 0 (DMA без IRQ) |
| 0x20 | TXFIFO | запись пар `[L<<16][R<<16]` |
| 0x24 | CLKD | 0x152 — MCLKO_EN(8), BCLKDIV=5(<<4, /8), MCLKDIV=2(/2) |
| 0x28/0x2C | TXCNT/RXCNT | счётчики сэмплов |
| 0x30 | CHCFG | TXSLOT=2, RXSLOT=2 |
| 0x34 | TXCHSEL | OFFSET(1)<<12, CHEN(2)<<4, CHSEL |
| 0x44 | TXCHMAP | 0x76543210 |

Формат FIFO [ДАТАШИТ 8.6.7.6 + СТЕНД]: 32-бит слово, сэмпл в **старших** 16 битах;
пара = `[L<<16][R<<16]`. **TXIM (FCTL bit2)=0** — иначе FIFO берёт LSB (0) → тишина.

### Тактирование (CCU) [ДАТАШИТ 4.3.5.28]

| Регистр | Адрес | Назначение |
|---|---|---|
| PLL_AUDIO_CTRL | 0x01C20008 | PLL аудио 24.576 МГц |
| PLL_AUDIO_PAT | 0x01C20284 | SDM-паттерн |
| I2S_PCM0_CLK | 0x01C200B0 | mux PLL_AUDIO, bit31=gate |
| BUS_CLK_GATING2 | 0x01C20068 | I2S0 = bit12 |
| BUS_SOFT_RST3 | 0x01C202D0 | I2S0 = bit12 (НЕ 14 — это I2S2) |
| BUS_CLK_GATING0 | 0x01C20060 | DMA = bit6 |
| BUS_SOFT_RST0 | 0x01C202C0 | DMA = bit6 |

- **I2SPCM0_CLK bits[17:16]**: 00=PLL_AUDIO(8X), 11=PLL_AUDIO(1X). Для 48 кГц нужен
  **mux=11** (1X=24.576М), иначе BCLK неверный → тишина. [СТЕНД]
- Схема 48 кГц: AudioPLL=24.576М → SCLK 24.576М → BCLK /8 = 3.072М (=48k×32×2) → MCLK /2 = 12.288М.
- PLL_AUDIO_CTRL: `(1<<31)|(1<<24)|(N-1)<<8|(M-1)`; 48k: N=14, M=14, PAT=0xC000AC02;
  44.1k: N=7, M=8, PAT=0xC0010D84. Ждать LOCK (bit28).

### Порядок старта/останова (Linux sun4i_i2s_start/stop_playback)

```c
I2S_FCTL |= (1<<25);      /* flush TX FIFO */
I2S_TXCNT  = 0;           /* сброс TX counter */
I2S_CTL  |= TX_EN;
I2S_INT  |= (1<<7);       /* TX DRQ enable */
/* stop: I2S_CTL &= ~TX_EN; I2S_INT &= ~TX_DRQ; */
```

### Когерентность DMA (ГЛАВНЫЙ урок) [СТЕНД/JTAG]

H3 **НЕ кэш-когерентен** для DMA: CPU пишет LLI/буфер в D-cache, DMAC читает
физическую память мимо кэша → получает мусор (JTAG: по 0x40100000 `CFG=0xea000016`,
`CUR_SRC="eGON"`). Решение — **clean D-cache перед стартом DMA**, НЕ перестройка MMU:

- `dcc_clean_range(addr,len)` (src/h3_mmu.c): `DCCMVAC (mcr p15,0,c7,c10,1)` по 32-байт
  линиям + DSB; выравнивание `& ~31` / `(end+31)&~31`.
- Вызовы: `g_lli` в `h3_dma_audio_init` (после заполнения LLI), `_dma_buf` в
  `i2s_play_freq` (после synth).
- ❌ Нельзя перестраивать TTBR0 на лету (Data Abort) и отключать MMU (Undefined
  Instruction) — код исполняется из DRAM.
- Направления: DMA читает → **clean**; DMA пишет → **invalidate** (`DCIMVAC c7,c6,1`);
  двунаправленно → clean+invalidate (`DCCIMVAC c7,c14,1`).

### DMA sun6i (0x01C02000) [СТЕНД]

- Канал n: `+0x100 + n*0x40` (EN 0x00, PAU 0x04, LLI 0x08, CFG 0x0C RO, CUR_SRC 0x10 RO).
  DMA_STA 0x30 (bitN=busy), DMA_SEC_REG **0x20** (=DMA_SEC_REG, НЕ soft-reset!),
  AUTO_GATE 0x28 (=0x4).
- LLI = 6 слов (cfg,src,dst,len,para,next), выравнивание **32 байта** (правило sunxi-dma).
  Кольцо: next1→desc2, next2→desc1 (без 0xfffff800). **Один самозацикленный LLI
  молчит — нужно ≥2 [СТЕНД].**
- CFG: SRC_WIDTH[10:9]=2(32b), SRC_BURST[7:6]=1(burst4), SRC_DRQ[4:0]=1(SDRAM),
  DST_WIDTH[26:25]=2, DST_BURST[23:22]=0(burst1), DST_DRQ[20:16]=3(I2S0_TX), DST_MODE[21]=1.
- Осиротевшие каналы U-Boot (JTAG: канал 4/5 = EN=1, DESC="SPL", CUR="eGON") —
  конфликтуют с нашей LLI. Лечение: `dma_reap_stray()` — гасить ТОЛЬКО канальными
  регистрами (EN=0, PAU=0, ждать busy=0, W1C по 2 битам канала), без CCU-сброса и
  без глобальных IRQ_EN. Канал выбирается `dma_pick_channel()`: сначала 5..11,
  потом 0..4.
- В FEL DMA стартует В RESET (RST0 bit6=0) — нужен условный deassert
  `if(!(RST0&BIT6)) RST0|=BIT6`, иначе регистры DMA не отвечают на запись. [СТЕНД]

### Звук в эталоне Lakka (для справки)

Lakka 2.3.1 выводит звук НЕ через MAX98357A: `CONFIG_SND_SOC_MAX98357A is not set`,
а через sun4i-i2s на **i2s2 (0x1C22800)** + HDMI (simple-audio-card, `mclk-fs=<128>`,
`frame-inversion`, TDM 2×32 слот) или через аналоговый кодек/SPDIF. Приложение —
RetroArch `audio_driver="alsathread"`, `audio_out_rate="48000"`, `audio_latency="64"`.
Наш маршрут I2S0 → MAX98357A — самостоятельная реализация (тест-тон), не копия эталона.

## 4. TrustZone (SMTA) и GIC

Блок 0x01C23400 — **SMTA** (Secure Memory Touch Arbiter), НЕ TZPC [ДАТАШИТ §4.18]:

```
SMTA_DECPORT1: STA 0x10, SET 0x14, CLR 0x18   <- bit1=DMA, bit3=Crypto, bit4=USB
SMTA_DECPORT2: STA 0x1C, SET 0x20, CLR 0x24   <- bit3=CCU, bit2=SysCtl, bit6=RTC
STA=RO (0=secure,1=non-secure); SET/CLR=WO.
```

Плюс `DMA_SEC_REG (0x01C02020)`: бит канала, 0=Secure,1=Non-secure.
Снять защиты (src/secure.c): `SMTA_DEC1_SET|=(1<<1)` (DMA), `SMTA_DEC2_SET|=(1<<3)` (CCU),
`DMA_SEC_REG=0xFFFFFFFF` — ПОСЛЕ gate+reset DMA.

GIC: U-Boot гасит GIC — обязателен `gic_init()` (distributor 0x01C81000: сброс
enable/pending/active, target CPU0, группы 0/1; CPUIF 0x01C82000: PMR=0xFF, CTL=grp0|grp1).

## 5. Пины (PIO)

| Пин | Функция | Регистр/биты |
|---|---|---|
| PA0-PA3 | JTAG TMS/TCK/TDO/TDI (F3) | PIO_CFG0 [15:0]=0x3333 |
| PA4 | UART0_TX (F2) | CFG0 [19:16]=2 |
| PA5 | UART0_RX (F2) | CFG0 [23:20]=2 |
| PA10 | SD усилителя (out, HIGH=работа) | CFG2 [7:4]=1 |
| PA18/19/20 | I2S0 LRCK/BCLK/DOUT (F2) | CFG2 [11:20]=0x222 |
| PG0-PG5 | SDC1 CLK/CMD/D0-D3 (F2) | CFG0=0x00222222 (W.639!) |
| PL7 | WIFI_EN (out, HIGH=вкл) | R_PIO CFG0 [31:28]=1 |
| PL10 | LED «жив» (out) | R_PIO CFG0, DATA bit10 |

**Важно [СТЕНД]:** запись `PIO_CFG0=0x3333` полной затирает PA4-5 → UART умирает.
Только RMW: `PIO_CFG0 = (PIO_CFG0 & 0xFFFF0000) | 0x3333`.
PG4/PG5 (SDC1_D2/D3) — забывали настраивать: биты [23:16] в PG_CFG0; без них
4-bit data-фаза мертва на любой частоте (W.639).

## 6. U-Boot vs FEL (JTAG-снимки) [СТЕНД]

| Регистр | FEL/эталон | U-Boot чистый | Мы |
|---|---|---|---|
| GICD_CTLR / GICC_CTLR | 0x3 / 0x3 | **0x0 / 0x0** | gic_init(): =3 |
| PLL_AUDIO_CTRL | 0x91000D0D | **0x00035514** (выкл) | enable+lock |
| I2SPCM0_CLK | 0x80030000 | **0x0** | bit31=1, mux=11 |
| GATING2/RST3 bit12 (I2S0) | 1 | **0** | gate on, deassert |
| GATING0/RST0 bit6 (DMA) | 1 | **0** | gate on, deassert |
| DMA_SEC_REG | 0xFFFF | **0x0** (secure) | =0xFFFF |
| SMTA DEC1/DEC2 | FF/7F | FF/7F | — |

Итог: U-Boot держит DMA и I2S0 в gate+reset, PLL/I2SPCM0 выключены, GIC выключен,
DMA-каналы secure. Наша init всё включает (после init значения сходятся с FEL).

Также U-Boot использует для MMU двухуровневую L1→L2 таблицу (TTBR0=0x5FFF4000,
L1[0x400..0x402]=0xFFFFFFFF); mmu_mark_uncached() поддерживает оба типа записей
(section L1 для FEL, large page 16K L2 для U-Boot).

## 7. WiFi: RTL8189FTV на SDC1

Чип RTL8189FTV (Realtek, SDIO, 2.4 ГГц, 802.11 b/g/n 1T1R), QFN24, кварц 26 МГц.
Подключён к SDC1 (PG0-PG5), WIFI_EN=PL7 (active high), питание 3.3В (VCC-WIFI-IO).

### SDIO-протокол (проверено)

- Порядок: `CMD5 (ждём C=1) → CMD3 (RCA) → CMD7 (select) → PowerSeq CARDEMU→ACTIVE
  (ДО Fn1!) → вкл. Fn1 (CCCR 0x02 |= 0x02) → CMD53 загрузка FW (byte-mode по 4 байта,
  адрес (8<<13)|0x1000) → eFuse → программный MAC в REG_MACID 0x610-0x615`.
- Чистая I/O-карта: CMD0/CMD8/ACMD41 не используются.
- **Для data-команд SDC1 НЕ взводит CMD_DONE** — завершение по DATA_DONE (bit3).
  [СТЕНД]
- **Шина MMC1 — OLD mode** (bit30=0 в SDMMC1_CLK_REG); MODE_NEW не даёт ответа. [СТЕНД]
- **SDIO-адресация**: SDIO-local регистры (0x100, 0x86, RPWM/CPWM/HIMR/HISR) —
  device id **0** (через fn1!), WLAN IOREG (MAC 0x05...0xF0) — device id **8**,
  RX FIFO — device id **7** (адрес (7<<13)|(seq&3)). `ftaddr = (device_id<<13)|(offset&mask)`.
- CRC ответа для CMD52/CMD53 НЕ проверяется.
- CMD53 byte-mode: хвост (total не кратен 4) надо явно дописывать/дочитывать.
  Инкремент адреса (bit26) глючит на границе 4 байт (REG_MACID 0x610/0x614) —
  надёжнее отдельные CMD53 с inc=0.
- Функция 0 (CCCR/CIS) не поддерживает блочный CMD53 — тюплы читать байтово через CMD52.

### Скорость шины

- Рабочий режим (W.641): **50 МГц / 4-bit / HS** (эталон Lakka).
  `SDMMC1_CLK` = SCLK_GATE|MODE_NEW|SRC_PLLPERI0(200М)|N=1|M=4 → 50 МГц; CKC DIV=0.
  HS: CCCR 0x13 |= 0x02 (EHS, бит1; бит0 SHS — RO). New timing mode: фазы CCU
  игнорируются, вместо них `NTSR 0x5C |= BIT31`, `SAMP_DL 0x144 |= BIT7` (по sunxi-mmc).
- Раньше: 375 кГц (24М/2/16, N=2 M=16) на init; 6 МГц рабочий (W.235-248).
- 4-bit: CCCR 0x07 [2:0]=0b10 + SDC_CTYPE=1. **PG4/PG5 обязательны** (W.639).
- Host не тянет многоблочный read CMD53 — только 1 блок (512 б) за раз. [СТЕНД]

### Power-on чипа (PowerSeq)

`h3_wifi_power_on_chip()`: seq_ce2ac (CARDEMU→ACTIVE) — 0x05/0x06/0x27 записи +
поллинги готовности. Сначала `RSV_CTRL 0x1C=0` (unlock), затем BB/AFE clock
(`0x2B[4:2]=0x1C`). Состояние: `h3_wifi_power_state()` — A0[1:0], 1=ACTIVE.
Order: `CMD5/3/7 → pwon → fn1(CCCR 0x02) → fwdl`. [СТЕНД]

### SDC0 — слот SD-карты (W.78x) [СТЕНД]

Отдельный контроллер (0x01C0F000, регистры те же, что SDC1 — см. h3_sdc.h).
Реализация — `h3_sdc0.c` (двухфазный: штатно НАСЛЕДУЕТ MMC0 от U-Boot —
Фаза A, только CMD17; полный подъём с нуля — Фаза B — лишь для legacy-пути
без U-Boot):

- **CCU**: gate MMC0 = GATING0 bit8; **модульный reset (RST0 bit8) НЕ трогаем**
  после U-Boot (глобальный сброс модуля!) — «убиваем зомби» локально:
  `SDC_GCTL=RESET` + `SDC_HWRST` (0→1 мс→1).
- **Такт**: `SDMMC0_CLK` (0x88) = OSC24M, N=2, M=16 → init ~750/375 кГц
  (как SDC1). Формат битов тот же, что SDMMC1_CLK (0x8C).
- **Пины**: PC0..PC5 = function 2 (SDC0 CLK/CMD/D0-D3); PORTC cfg0:
  `&=~0x00FFFFFF; |=0x00222222`. PC6/PC7 не трогаем.
- **Инициализация карты**: CMD0 (SEND_INIT, 80 тактов) → CMD8 (0x1AA,
  SDSC может не ответить — не фатально) → CMD55/ACMD41 (HCS=1, поллинг
  busy до 1 с) → CMD2 (CID) → CMD3 (RCA) → CMD7 (select) → CMD9 (CSD,
  размер) → CMD55/ACMD6 (4-bit, SDC_CTYPE=1) → CMD16 (blocklen 512) →
  CMD58 (READ_OCR: CCS bit30 → SDSC/SDHC-адресация).
- **Чтение**: CMD17, ждём CMD_DONE, выгребаем FIFO по `STATUS_FIFO_LEVEL`
  (0x3C bit17..), затем DATA_DONE. Только 1 блок (512 Б) за команду
  (многоблок — то же ограничение, что SDC1). Запись — CMD24 (заделка).

### Таблица разделов / FAT32 (W.78x)

- Карта: **MBR**; раздел выбирается по наличию `/roms` (типы 0x0B/0x0C —
  только FAT32; FAT16 не поддерживается). На SD целевой раздел обычно 1,
  на USB-флешке — 0.
- FAT32-ридер (`fat.c`): ищет раздел, в корне которого есть папка `/roms`
  (если такого нет — первый валидный FAT32); BPB: sec/clu (13), reserved
  (14), fats (16), fat_size (36), root_clu (44); data_start = part +
  reserved + fats*fat_size.
- Чтение: кластер→сектор `data_start + (cl-2)*spc`; FAT-цепочка по
  `part + reserved + cl*4` (с кэшем FAT-сектора); каталоги 8.3 + LFN
  (фрагменты на диске от старшего номера к младшему). Запись:
  fat_mkdir/fat_write_file (8.3-имя)/fat_delete. Источник секторов — SD
  или USB-флешка (fat_set_block_source).

### Загрузка прошивки (MCUFWDL)

- `fw_nic.bin` (18802 б) встроена в бинарь через `src/fw_blob.S` (.incbin).
  Заголовок 32 б (RT_8188F_FIRMWARE_HDR) пропускается при загрузке.
- Протокол: REG_MCUFWDL=0x80: |=0x01 (FWDL), +2 &=0xF7 (8051 reset);
  запись страницами 4096 б через CMD53 byte-mode (по 4 б), старт (8<<13)|0x1000;
  поллинг ChkSum_rpt; FreeToGo: |=BIT1 (MCUFWDL_RDY), &=~BIT6, полный 8051 reset
  (0x1D.0/0x03.2), поллинг `MCUFWDL_RDY|ChkSum|WINTINI|RAM_DL_SEL` → штатно 0xC6.
- **MCUFWDL=0xC6 — штатно** (8051 загружена), НЕ ошибка. [СТЕНД]

### RF-доступ (LSSI Path A) [СТЕНД]

- Запись RF: BB 0x840 = `(rf_off<<20) | (data&0xFFFFF)`.
- Чтение RF: BB 0x824 — адрес в [30:23] (сдвиг 23), edge-триггер bit31
  (снять→поднять), пауза ~100 мкс, readback из BB 0x8A0 (LSSI) или 0x8B8 (PI,
  если bit8 в 0x820).
- LSSI работает только после загрузки полного BB-конфига (`bb_phy_reg[]`,
  209 записей — настраивает RF-интерфейс 0x840/0x860/0x864/0x870).
- Включение BB/RF (PHY_BBConfig8188F): SYS_FUNC_EN(0x02)|=BIT13|BIT0|BIT1 →
  сброс BB (байт0 0x16→0x14) → REG_RF_CTRL(0x1F)=0x07 → RF reg1=0x780 (power-on
  ПОСЛЕ сброса) → SYS_FUNC_EN=0x17 → REG_CR(0x100)=0x63F → BB-конфиг.

### RF-калибровки

- **LCK** (h3_wifi_rf_lck): TXPAUSE(0x522)=0xFF → RF 0xB0=0xDFBF0 (LDO ON) →
  RF_CHNLBW(0x18)=0x401 → |=0x8000 (trigger) → 100 мс → RF 0xB0=0xDFFF0 (LDO OFF) →
  TXPAUSE=0. RF 0x00=RfAC сохраняется/восстанавливается. Бит15 после LCK может
  остаться 1 — норма для 8188F. RF-таблица грузится с пропуском записи 0x18
  (таблица ставит 0xFC07 — ломает LCK). [СТЕНД]
- **IQK** (h3_wifi_rf_iqk): PI-режим (0x860/0x868=0x01000100 ON / 0x01000000 OFF),
  MAC-настройки (TXPAUSE=0x3F, BCN_CTRL 0x550/0x551 &=~BIT3, GPIO 0x40 &=~BIT5),
  RF 0xEF/0x30/0x31/0x32/0xDF/0x56 по драйверу, тоны 0xE30..0xE4C, one-shot 0xE48,
  LOK_Result (RF 0x08) сохраняется после Tx и восстанавливается после Rx one-shot.
  Критерий: Tx OK eac[28]=0, e94[25:16]!=0x142, e9c!=0x42; Rx OK eac[27]=0,
  ea4[25:16]!=0x132, eac[25:16]!=0x36. Выход: E28=0, тоны 0x01008C00, SI-режим,
  восстановление Path_SEL (BB 0x948, RF 0xB0) и BB (0x800/0xC04/0xC08/0x874). [СТЕНД]

### RX-конфигурация (по rtl8188f_hal_init / sdio_halinit) [СТЕНД]

- Рабочий канал: RF_CHNLBW = 0xC00|channel (BW20), НЕ 0x8C01 (bit15=режим калибровки).
- RFMOD (0x800) |= **bCCKEn=BIT24 | bOFDMEn=BIT25** (НЕ bit16/17!) — иначе приёмник глух.
- RCR (0x608) = 0x700060CE (стационарный) / 0x7000604E (скан, снят CBSSID_BCN).
- RRSR (0x440) = 0xFFFF1; RXFLTMAP0/1/2 (0x6A0/0x6A2/0x6A4): скан — 0xFFFF/0x0400/0,
  connected — 0xFFFF/0x0400/0xFFFF.
- RQPN (0x200)=0x80E1080C (HPQ 0x0C|LPQ 0x08<<8|PUBQ 0xE1<<16|LD), NPQ (0x214)=0x02 —
  LPQ=8, чтобы 3-страничные data-кадры влезали в LOQ (W.499).
- TX-границы (0x424/0x425/0x45D/0x114/0x209)=0xF8; rx_bndy (0x116)=0x3F7F.
- LLT (0x224)|=BIT16, поллинг сброса; PBP (0x104)=0x11; DRVINFO (0x60F)=0x04.
- RXDMA: 0x280=0x20000101 (agg), 0x290=0x0E, TRXDMA_CTRL (0x10C)=0x3F5B0|0x4 (RMW).
- REG_CR (0x100) |= BIT6|BIT7 (MACTXEN/MACRXEN) + BIT9, &=~BIT8 — итог 0x206FF (живой).
- SDIO TX_CTRL (SDIO-local 0x00) &= 0xFFFFF8 (DRV: before MAC on).
- BAR_MODE (0x4CC)=0x0201FFFF, HWSEQ (0x423)=0xFF, EARLY_MODE (0x4D0)=0.
- EN_AMPDU_RTY_NEW: REG_FWHW_TXQ_CTRL **0x420** |= BIT7 (НЕ 0x818!), ACKTO 0x634=0x40.
- Антенна: REG_LEDCFG2 (0x4E) |= BIT7 (DPDT_SEL_EN).
- HIMR (SDIO-local 0x14, fn1!): 4 байта {0x01,0x00,0x02,0x00} = RX_REQUEST|C2HCMD.
  HISR (0x18) = 0xFF (W1C clear). CCCR 0x04 |= 0x03 (IENM|IEN1), FBR 0x100 |= 0x01.
- RPWM (SDIO-local 0x80): активный запрос = S4 0x0C|toggle(bit7)|ACK(bit6 при S<2),
  маска 0xC1; поллинг CPWM (0x38) по смене toggle bit7. [СТЕНД]
- RW_RELEASE_EN: 0x284 &= ~BIT18 (release RX DMA). C2H clear: 0x1AF=0.
- H2C SET_PWR_MODE (id 0x20): box0 0x1D0={id,0,2,4}, EX 0x1F0={0,0x0C,0x40};
  ждать valid==0 в 0x1CC; **НЕ писать 0x00 в 0x1CC — сбивает valid-бит.** [СТЕНД]

### Индикаторы RX

- **RX0_REQ_LEN**: 0x1C (SDIO-local, fn1) — накопленный объём, >0 = данные.
  Ориентир завершения пакета — сброс в 0 после выдачи.
- **RXPKT_NUM**: 0x284 (device 8), счётчик [15:0]; bit17=RXDMA_IDLE; 0x287 — верхний байт.
- **HISR**: 0x18 (fn1) — bit0=RX_REQUEST, bit17=C2HCMD.
- **MCUFWDL** 0x80 = 0xC6 — штатно (см. выше).

### Свип (сканирование)

- Каналы 1..13: RF 0x18 = 0xC00|ch, 50 мс на перестройку, до 3 probe request,
  300 мс слушание, чтение RX FIFO по 1 блоку (512 б), парсер beacon на off+24.
- Notch-фильтр (phy_SpurCalibration_8188F) для каналов 5..8: 0xC40[28:24]+bit9,
  0xD40/0xD4C и т.д. (для остальных — сброс).
- Парсер: drvsz=(rxbuf[off+2]&0x0F)<<3, fr=off+24+drvsz+shift; BCN: fc&0xF0==0x80,
  SSID IE id=0, DS IE id=3 (реальный канал AP), RSN IE id=0x30, WMM IE id=0xDD
  (OUI 00:50:F2 type 2). `rx_seq` сбрасывается на старте каждого скана (W.664).

## 8. TX-путь (802.11 data, DHCP/EAPOL)

- Транзакции: ожидание свободных страниц HIQ/LOQ (SDIO-local 0x20: [0]=HIQ,
  [4]=LOQ, 16-бит LE), OQT-слот (0x2A), запись CMD53 byte-mode в FIFO
  (HIQ device 4, LOQ device 6; адрес (dev<<13)|((total/4)&0x1FFF)).
- TX-дескриптор (40 б): OFFSET=40, BMC (bit0 b3) для multicast-dst, LAST|FIRST|OWN,
  MACID=0 (pairwise), QUEUE_SEL (0x12=MGNT для HIQ/mgmt, 0=BE для LOQ),
  USE_RATE+TX_RATE=1M (DHCP/EAPOL), RETRY_LIMIT_EN=1 lim=12, AGG_BREAK (b8 bit16)
  для одиночного data, HWSEQ_EN (b33 bit15) для non-QoS, **checksum** (XOR 16
  16-бит слов → байты 28-29, обязателен для SDIO).
- Шифрование: Protected (fc bit11) + SEC_TYPE=AES (txdesc[6] bits7:6=0xC0) +
  8-байт CCMP IV (PN0 PN1 0 (0x20|keyid<<6) PN2..PN5) — только после 4-way,
  кроме EAPOL (уходит plaintext).
- QoS Data (0x88) — только если WMM IE в beacon цели; иначе обычный Data.
  seq для QoS пишется **и в 802.11-заголовок, и в TX-дескриптор**
  (`SET_TX_DESC_SEQ_8188F` = pTxDesc+36, биты 12..23 → байты 37/38:
  `buf[37]=seq[3:0]<<4`, `buf[38]=seq[11:4]`). **Чип при HWSEQ_EN=0 берёт seq
  из дескриптора, а не из заголовка** — без записи в дескриптор все data-кадры
  уходят с seq=0, и AP отбрасывает их как дубликаты (корень «OFFER есть,
  ACK никогда», W.770). HWSEQ_EN (b33 bit15) — только non-QoS/EAPOL.
- RETAINED: 3-страничные кадры (DHCP 320 Б) идут через **HIQ** (W.584);
  LOQ — до 2 стр. (актуально для больших data-кадров).

### STA-режим (REG_MSR 0x0102) [СТЕНД]

- После успешной ассоциации MSR (0x0102) обязан быть **0x02 = `_HW_STATE_STATION_`**
  (эталон: `rtw_mlme_ext.h:57-60`: 0x00 NOLINK, 0x01 ADHOC, 0x02 STATION, 0x03 AP).
- **Корень «ПК не может пинговать» (W.778):** раньше писался 0x01 (ADHOC). При
  MSR=ADHOC чип неверно классифицирует кадры: unicast от AP (ToDS/FromDS)
  принимает, а broadcast-ARP от других клиентов и все data с DA не-наш-MAC
  отфильтровывает — пинг с ПК не шёл, хотя плата↔шлюз работала. После
  0x02 broadcast/FromDS принимаются: ping с ПК 0% потерь (мин 6 мс, средн. 14 мс).
- Связанность (`msr & 0x03` в трёх местах): 0x01 = adhoc, 0x02 = sta,
  0x03 = ap; бит 0 старого кода (0x01 или 0x03) означал «adhoc или ap».

## 9. Эталон (живая Lakka 2.3.1) — ключевые значения

- CAM: pairwise → slot 4 (ctrl=0x8010, MAC=BSSID, keyid 0); GTK → slot 5
  (ctrl=0x8051, MAC=BSSID!, keyid 1). REG_SECCFG (0x680) = 0x010C
  (CHK_KEYID|RxDecEn|TxEncEn, без CHK_BMC).
- REG_FWHW_TXQ_CTRL (0x420) = 0xFF311F00 (bit7 EN_AMPDU_RTY_NEW).
- RQPN (0x200) = 0x00E7020C (LPQ 2, PUBQ 0xE7) — наш LPQ=8 это страховка.
- RCR (0x608) = 0x700060CE; TRXDMA_CTRL (0x10C) = 0xF5B4.
- SDIO FREE_TXPG (0x20) = 0x0002000C (HIQ=12, MIDQ=2), 0x24 = 0x00E70002 (LOWQ=2, PUBQ=0xE7).
- TX rate: MCS7 (0x13) — мы для DHCP/EAPOL используем 1M (надёжнее).
- Драйвер: 8189fs v4.3.24_15589.20151023; прошивка rtl8188efw.bin = наш fw_nic.bin (0x2BD0).

## 10. Уроки (не наступать дважды)

1. Полная запись `PIO_CFG0=0x3333` убивает UART0-TX (PA4) — только RMW. [СТЕНД]
2. Один LLI на H3 молчит; два LLI — работают. [СТЕНД]
3. U-Boot гасит GIC и держит DMA/I2S0 в gate+reset, PLL/I2SPCM0 выкл. [СТЕНД]
4. JTAG = PA0-3 (F3, 0x3333). PA21 НЕ JTAG (это PCM0_DIN) — в интернете ошибаются.
5. 0x20 I2S = TXFIFO; 0x20 DMA = DMA_SEC_REG (не soft-reset). [СТЕНД]
6. LLI DMA — выносить в uncached (или чистить D-cache) — DMAC читает мимо кэша.
7. TXIM=0 (данные в MSB, [L<<16][R<<16]) — иначе тишина.
8. I2SPCM0_CLK mux=PLL_AUDIO 1X (11), не 8X.
9. После U-Boot `go` есть осиротевшие DMA-каналы — гасить только канальными
   регистрами + W1C, без CCU-сброса и без 0xFFFFFFFF.
10. data-команды SDC1: ждать DATA_DONE, не CMD_DONE. Шина MMC1 — OLD mode.
11. SDIO-адресация: device 0 = local (fn1), 8 = WLAN IOREG, 7 = RX FIFO.
12. Функция 0 (CCCR/CIS) — только CMD52 побайтово; CMD53 блочно — только fn1.
13. CMD52/CMD53 CRC ответа не проверять (ложная CRC-ошибка).
14. PowerSeq CARDEMU→ACTIVE — ДО включения Fn1 и ДО fwdl.
15. CMD53 byte-mode: хвост (total%4) дописывать/дочитывать явно; inc глючит на
    границе 4 байт — использовать inc=0 для 0x610/0x614.
16. LSSI-чтение 8188F: адрес в 0x824 [30:23] (сдвиг 23), readback 0x8A0/0x8B8.
17. RFMOD: bCCKEn=BIT24, bOFDMEn=BIT25 (не bit16/17).
18. RF-таблицу грузить с пропуском 0x18; после неё RF 0x18=0x401; RF 0x00 после
    LCK — сохранять RfAC (не хардкодить 0x33D95/0x30000).
19. H2C: не писать 0x00 в 0x1CC (HMETFR) — сбивает valid-бит мейлбокса.
20. RXPKT_NUM (0x284): bit17=RXDMA_IDLE (0x00020000), счётчик [15:0].
21. PG4/PG5 (SDC1_D2/D3) — настраивать в PG_CFG0 (W.639), иначе 4-bit мёртв.
22. В FEL DMA в reset (RST0 bit6=0) — условный deassert обязателен.
23. Хвостов отладки быть не должно: диагностика — под `WIFI_VERBOSE_LOG`/
    `WIFI_TX_DIAG`/`WIFI_RX_DIAG`, не «всегда-выполняемая под мёртвым флагом».