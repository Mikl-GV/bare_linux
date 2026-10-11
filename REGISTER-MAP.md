# Карта регистров Allwinner H3 — актуальные значения

> Снимки значений, проверенные на стенде (FEL-загрузка, наша прошивка).
> Это рабочий справочник для отладки; подробные выводы — в H3-BAREMETAL-GUIDE.md
> и KNOWLEDGE.md. Ревизия: 2026-10-11 (этап WiFi).

## I2S0 (0x01C22000) — эталон (работает)

```
0x01C22000 CTL    = 00060111   <- BCLK_OUT(18) LRCK_OUT(17) SDO0(8) MODE_LEFT(1<<4) TX_EN(2) GL_EN(0)
0x01C22004 FMT0   = 00001f37   <- LRCKPER=32(0x1f<<8) SR=3(16b,<<4) SW=7(32b)
0x01C22008 FMT1   = 00000000   <- SEXT=0 (zeros LSB pad)
0x01C2200C ISTA   = 00000044
0x01C22014 FCTL   = 000400f0   <- TXTL=64(0x40<<12) TXIM=0 RXOM=0
0x01C22018 FSTA   = 003c0000   <- TX FIFO level 0x3c=60 (почти полон -> DMA пишет)
0x01C2201C INT    = 00000000   <- DRQ через DMA (INT reg не обязателен)
0x01C22020 TXFIFO = 00000000
0x01C22024 CLKD   = 00000152   <- MCLKO_EN(8) BCLKDIV=5(<<4=0x50) MCLKDIV=2
0x01C22028 TXCNT  = 0001248d   <- считает отправленные сэмплы
0x01C2202C RXCNT  = 00000000
0x01C22030 CHCFG  = 00000011   <- TXSLOT=2, RXSLOT=2
0x01C22034 TXSEL  = 00001031   <- OFFSET=1(12) CHEN=2(<<4) CHSEL=2-1
```

## CCU (0x01C20000)

```
PLL_AUDIO_CTRL(0x08) = 91000d0d   <- EN(31) SDM(24) LOCK(28) N=13 M=13 + PAT
I2SPCM0_CLK(0xB0)    = 80030000   <- bit31=on, bits17:16=11 => PLL_AUDIO 1X
GATING0 (0x60) = 00884040   <- DMA(bit6), HSTMR(bit19)
GATING2 (0x68) = 00001020   <- I2S0(bit12)
RST0    (0x2C0)= 00884040   <- DMA/HSTMR deassert
RST3    (0x2D0)= 00001000   <- I2S0 deassert
SDMMC1_CLK (0x8C) = 8002000F   <- SCLK_GATING(31), SRC_OSC24M, N=2, M=16 (~750 кГц)
RST0     (0x2C0) bit9 = 1     <- MMC1 deassert (GATING0 bit9=1)
```

## DMA (0x01C02000)

```
0x01C02000 IRQ_EN0   = 00000000
0x01C02020 SEC_REG   = 0000FFFF   <- все каналы non-secure
0x01C02028 AUTO_GATE = 00000004   <- авто-gating
0x01C02030 DMA_STA   = 00000010   <- при активном звуке канал 4 busy
```

## SMTA / TrustZone (0x01C23400)

- SET-регистры читаются как 0 всегда (WO); STA показывает реальную защиту.
- У нас STA=FF/7F — всё non-secure, защиты сняты.

## GIC (0x01C81000 dist / 0x01C82000 cpu)

```
DIST_CTL(0x00) = 00000003   <- EnableGrp0|EnableGrp1
CPUIF_CTL(0x00)= 00000003   <- Grp0|Grp1 enabled
CPUIF_PMR(0x04)= 000000f8   <- prio mask
```

## SDC1 / MMC1 (0x01C10000) — WiFi, снимок после нашей инициализации

```
0x00 GCTL  = 20000110   <- INT_ENB(4) CD_DBC(8) + AHB FIFO mode (bit31) при передаче
0x04 CKC   = 00030001   <- CCLK_ENB(16) CCLK_CTRL(17) DIV=1 (~375 кГц init)
0x08 TMOR  = 00FFFFFF40
0x0C CTYPE = 00000001   <- 4-bit шина
0x10 BLKSZ = 00000200   <- 512
0x38 RISR  = 00000004   <- после CMD: CMD_DONE (для data-команд ждём DATA_DONE/bit3)
0x3C STA   = 0001AD06   <- FIFO/статус
0x44 FUNS  = 0000CEAA   <- включить функции контроллера
```

Начальные значения снимка (до инициализации):
```
pin: pl_before=0x400, pl_after=0x480 (WIFI_EN PL7 поднят)
pg_cfg0=0x77772222, pg_cfg1=0x00777722 (PG0-5 = function 2 = SDC1)
```

## Фактические значения из UART-лога (W.040, работает)

```
sdio_init rc=0
ccid0=0x32 ccid1=0x02
cis addr=0x1000 rc=0 manf=0x024C   <- Realtek подтверждён
bus4 rc=0
pwon rc=0
sdio100 v=0x07  sdio86 v=0x00      <- чип проснулся (не 0xEA)
fn1 ioen=0x02                       <- функция 1 включена
fn1 c53w rc=0                       <- блочная запись CMD53 работает
```