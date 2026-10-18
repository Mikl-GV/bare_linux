# Карта регистров Allwinner H3 — актуальные значения

> Снимки значений, проверенные на стенде (FEL-загрузка, наша прошивка).
> Это рабочий справочник для отладки; подробные выводы — в H3-BAREMETAL-GUIDE.md
> и KNOWLEDGE.md. Ревизия: 2026-10-16 (эталон — распакованный образ Lakka 2.3.1).

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
0x04 CKC   = 00030000   <- CCLK_ENB(16) CCLK_CTRL(17) DIV=0 (такт 6 МГц, 24M/2/2)
0x08 TMOR  = 00FFFFFF40
0x0C CTYPE = 00000001   <- 4-bit шина (эталон dtb bus-width=<4>)
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

## Фактические значения из UART-лога (работает — полный init + RF LCK)

```
sdio_init rc=0
ccid0=0x32 ccid1=0x02
chip_id rc=0 manf=0x024C card=0xF179    <- RTL8189FTV подтверждён
pwon rc=0                                <- PowerSeq CARDEMU→ACTIVE
fn1 ioen=0x02                            <- функция 1 включена
fwdl rc=0 len=0x4972                     <- загрузка прошивки OK, FW READY
efuse rc=0 mac=FF:FF:FF:FF:FF:FF         <- eFuse пуст (Orange Pi Lite)
mac_set=OK macid_re=02:00:00:54:46:98    <- программный MAC записан и подтверждён
lck: sf_pre=0xFC00 sf8=0x17              <- BB/RF enable применился
lck: bb_config loaded (209 regs)         <- полный BB-конфиг (LSSI-интерфейс)
lck: lssi_8A0raw=0x00100000              <- LSSI readback ожил
lck: chnlbw_pre=0x400 set=0x401          <- канал 1 через LSSI (дефолт полоса 0x400)
lck: chnlbw_trig=0x8401 done=0x8401      <- LCK отработал (бит15 остаётся — норма)
lck: done
```

Ключевые регистры SDC1 во время работы (4-bit, 6 МГц — эталон dtb):
```
0x00 GCTL  = 0x20000110   <- INT_ENB(4) CD_DBC(8) + AHB FIFO (bit31) при передаче
0x04 CKC   = 0x00030000   <- CCLK_ENB(16) CCLK_CTRL(17) DIV=0 (6 МГц)
0x08 TMOR  = 0xFFFFFFFF   <- ставится на каждую передачу (макс. таймаут)
0x0C CTYPE = 0x00000001   <- 4-bit шина (эталон bus-width=<4>)
0x18 CMDR  = 0x80002675   <- CMD53 write: START|idx|RESP|DATA_TRANS|TRANS_DIR|WAIT_PRE
0x38 RISR  = 0x00000004   <- CMD_DONE (для data-команд ждём DATA_DONE/bit3)
0x3C STA   = 0x0001AD06   <- FIFO/статус
0x44 FUNS  = 0x0000CEAA   <- включить функции контроллера
0x200 FIFO = PIO-доступ
```

RF/BB-регистры чипа (WLAN IOREG, Path A), задействованные в LCK:
```
0x0100 REG_CR     = 0x0000063F  <- HCI_DMA|TXDMA|RXDMA|PROTOCOL|SCHEDULE|MAC_EN|RF_EN
0x0002 SYS_FUNC_EN= 0x0000FC17  <- BB/RF питание (BIT13) + BB из сброса (0x17)
0x001F REG_RF_CTRL= 0x07        <- RF_EN|RF_RSTB|RF_SDMRSTB
0x0E30 BB rTx_IQK_Tone_A = 0x18008C1C  <- запись-чтение работает (BB жив)
0x0820 BB HSSI-para1   = 0x01000100  <- LSSI длина данных (из BB-конфига)
0x0824 BB HSSI-para2   = 0x00390204  <- LSSI адрес/edge (из BB-конфига)
0x08A0 BB LSSI readback(Path A)      <- данные RF-регистра (readback)
RF 0x18 RF_CHNLBW = 0x400 (дефолт полоса) / 0x401 (канал 1) / 0x8401 (LCK trig)
RF 0x01 = 0x780   <- RF power-on
RF 0xB0 = 0xDFBF0 (LDO ON) / 0xDFFF0 (LDO OFF)
```

## Снимок RX W.316 — приём РАБОТАЕТ (beacon/SSID декодирован)

> Актуально на W.316 (2026-10-18). Старые снимки ниже про `rxlen=0/pkt=0`
> и «блокер физический приёмный тракт» — УСТАРЕЛИ: RX-приём ожил.

```
rx: t=0 ch=1 rxlen=0x0127 blk rc=0 blk rc=0 fifo_rd rc=0 read=0x400 rxlen2=0
    BCN plen=0x0000010F ch=1 ssid=midea_b8_6515
```

Ключевые индикаторы живого RX-приёма (по всем 13 каналам):
- `trxst` (0x11C RXFF_PTR) монотонно растёт (FIFO реально заполняется).
- `pkt` (0x287) растёт с каждым каналом.
- `hisr` (0x18) bit0=1 = RX_REQUEST — FW выложила пакет.
- Парсер выводит `BCN plen= ch= ssid=` — кадр декодирован, SSID извлечён.

Почему раньше «ни один пакет» (W.269-W.315): НЕ прошивка 8051 и НЕ физика.
Причины: (1) host SDIO H3 не тянет многоблочный read CMD53 (30 блоков → rc=-1
и зависание) — выдать можно только по 1 блоку 512 б/CMD53; (2) парсер искал
кадр на off+28 (fallback drvsz=4), а реально beacon лежит на off+24;
(3) парсер гейтился по `frc==0`, но лишний big-read возвращал -1. Исправлено
в W.316: чтение по 1 блоку, drvsz из поля (0) без fallback, парсер по `acc>0`.

## Снимок RX-диагностики (полный init + RX-конфиг)

```
rx: mcu=0xC6 (fw_rdy=1 rst=0)      <- 8051 загружена: RDY|ChkSum|WINTINI|RAM_DL_SEL
rx: sf16=0xFC17 (cpu_en=1)          <- SYS_FUNC_EN (bit10 CPUEN взведён)
rx: rpwm=0xC0 cpwm=0x80 (tog_sw=1)  <- ACTIVE подтверждена по смене toggle (fn1)
rx: rf18=0x401                      <- канал 1, BW 20М
rx: llt=0x2020                      <- LLT построена (инкремент половины?)
rx: rx_bndy=0x3EFF                  <- RX_DMA_BOUNDARY (0x0116)
rx: cr=0x000207FF (bit17=1 bit8=1 bit9=1) <- NETTYPE_AP + MACTXEN + MACRXEN
rx: rxpkt=0x2X08000 (rw_release=0)  <- счётчик пакетов растёт; RXDMA_IDLE снят
rx: himr_lo=0x01                    <- RX_REQUEST размаскирован (fn1)
rx: hisr_before=0x01 (b0=1)         <- RX_REQUEST пришёл от FW
rx: BCN plen=0x10F ch=2 ssid=midea  <- beacon декодирован (RX-приём работает)
```

## Снимок (финал хост-стороны RX)

```
rx: mcu=0xC6 (fw_rdy=1 rst=0)      <- 8051 исполняет FW (WINTINI_RDY=1)
rx: rpwm=0x00 cpwm=0x00 (active=1) <- FW подтвердила активный режим
rx: h2c_pwrmode 1cc=0x0            <- H2C 0x20 SET_PWR_MODE принят (box0 0x1D0)
rx: fbr=0x07 cccr_set=0x01         <- FBR INT_EN + CCCR IENM включены
rx: rf18=0x401                     <- канал (свип: 0x201/0x206/0x20B как SwChnl)
rx: rfmod=0x83075700               <- CCK/OFDM-блоки ON (было 0x83045700)
rx: cr=0x000207FF (bit17/8/9=1)    <- NETTYPE_AP + MACTXEN + MACRXEN
rx: rxpk=0x02000000 (rw_release=0)  <- 0x284: RXDMA_IDLE (старшие биты; счётчик [15:0]=0); RW_RELEASE=0
rx: himr_lo=0x07 hi=0x03           <- RX_REQUEST|AVAL|TXERR + C2HCMD
rx: rcr=0x7000200F                 <- CBSSID_BCN(bit7) снят; AB(bit3) сохранён
rx: c2h_evt=0x0 hisr=0x0           <- 8051 не генерирует события
rx: t=0..11 ch=1/6/11 rxlen=0 pkt=0<- ни одного декодированного кадра
```

**Вывод (хост-сторона)**: хост-сторона 1:1 по драйверу (все шаги: H2C 0x20,
CCCR/FBR, RPWM active, RFMOD CCK/OFDM, RCR ~CBSSID_BCN, каналы SwChnl 0x201),
FW жива и читает H2C, но RX-событий нет. Блокер — физический приёмный тракт.

## Снимок (точка восстановления — RF-тракт исчерпан)

Ключевые регистры после полной инициализации:

```
# LCK (RF-таблица до LCK с пропуском 0x18, чистый канал)
lck: rf_tab loaded (136 regs)  rfread df=0x100 56=0x51CF0 30=0x10000 ...
lck: rf18_after_tab=0x401  chnlbw_set=0x401  done in 70 ms
lck: rf00_restored=0x33D95   <- из RF-таблицы (не 0x30000!)

# IQK (PI-режим + MAC-настройки, но 0x142/0x42 стабильно)
iqk: pi_on 860=0x01000100 868=0x01000100   <- PI-режим RF-интерфейса
iqk: mac_cal set            <- TXPAUSE=0x3F, BCN_CTRL 0x550/0x551 &=~BIT3, 0x40 &=~BIT5
iqk: rf30=0x20000 rf31=0xF rf32=0x7FF7 rfdf=0x900 rf56=0x5102A rfEF=0x80000
iqk: tx e94=0x01420000 e9c=0x00420000 eac=0x04002000 <- FAIL (0x142/0x42)

# RX-конфигурация
rx: rfmod=0x83045700        <- RFC MOD: bCCKEn(24)|bOFDMEn(25) ON
rx: rqpn=0x00E7020C          <- REG_RQPN (0x200): HPQ 0x0C|LPQ 0x02<<8|NQ 0x07<<24
rx: llt=0x2020  rx_bndy=0x3F7F
rx: bwop=0x04                <- REG_BWOPMODE (0x603): BW_OPMODE_20MHZ
rx: cr=0x000207FF (macrx_en=1 mactx_en=1)  <- MACTXEN=6, MACRXEN=7
rx: rxdma_mode=0x0E agg_th=0x010F  <- RXDMA_MODE_CTRL (0x290), AGG_PG_TH (0x280)
rx: rcr=0x7000604E rrsr=0xFFF mar=0xFFFFFFFF
rx: fifopg=0x00E7020C        <- readback RQPN (0x200)

# Свип — FW не приняла НИ ОДНОГО пакета
rx: t=0..11 ch=1/6/11 rxlen=0 rxpkt=0x00020000 (RXDMA_IDLE) pkt=0
rx: c2h_evt=0 hisr=0
```

**Вывод (RF-тракт)**: BB/RF/MAC полностью 1:1 с драйвером (209+66+136 регистров,
LCK 70 мс, RQPN, BWOPMODE, агрегация, RCR, H2C active), IQK-обвязка по эталону
(PI+MAC+RF+BB), но FW не приняла ни одного пакета (счётчик RXPKT_NUM [15:0]=0).
Блокер — физический приёмный тракт (сигнал не доходит до демодулятора) либо
требуется эталонный лог с Linux-драйвера. Далее: NEXT-CHAT-PROMPT.md Шаг D
(probe request, физика, эталонный лог).