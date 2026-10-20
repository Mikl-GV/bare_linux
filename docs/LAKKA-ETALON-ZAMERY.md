# Эталон: замеры работающего Linux (Lakka 2.3.1) — WiFi RTL8189FTV

> Дата: 2026-10-16. Снято живьём по SSH с платы Orange Pi Lite,
> загруженной **Lakka-Allwinner.orangepi_lite.arm-2.3.1** (тот же образ,
> что распакован в `ref/Lakka-Allwinner.orangepi_lite.arm-2.3.1.image`).
> Плата питaется, WiFi **работает** (wlan0 UP, link ready).

## Доступ

- SSH: `root@10.72.75.55`, пароль `root` (включён в настройках Lakka).
- Ядро: `Linux Lakka 4.14.18 #1 SMP Fri Sep 13 14:36:07 CEST 2019 armv7l`.
- UART (COM13): консоль U-Boot; после старта ядра молчит (Lakka уводит
  приглашение на HDMI), зато паникует в drm/DMA — для замеров использовать SSH.

## 1. Драйвер

- Модуль: **`8189fs v4.3.24_15589.20151023`** — В ТОЧНОСТИ наша версия
  (`tools/wifi/rtl8189fs/include/rtw_version.h: DRIVERVERSION v4.3.24_15589.20151023`).
- Прошивка: `/lib/firmware/rtlwifi/rtl8188efw.bin` = **11216 байт** = наш
  `fw_nic.bin` (0x2BD0). Для 8189F используется именно 8188E-FW.
- Интерфейс: `wlan0`, MAC `02:81:38:27:88:cd` (локально-администрируемый —
  как и мы генерируем).

## 2. SDIO-шина mmc1 (debugfs, ЖИВЫЕ значения)

`/sys/kernel/debug/mmc1/mmc1:0001/ios`:

| Параметр | Значение | Наш bare-metal (до W.234) | Расхождение |
|---|---|---|---|
| clock | **50000000 Hz** (50 МГц) | 6 МГц (24М/2/2) | в 8 раз ниже |
| actual clock | 50000000 Hz | — | — |
| bus width | **2 (4 bits)** | 1-bit (4-bit отключён) | 1 vs 4 |
| timing spec | **2 (sd high-speed)** | нет HS | HS не включали |
| vdd | 21 (3.3~3.4V) | 3.3V | ок |
| power mode | 2 (on) | — | ок |
| signal voltage | 0 (3.30V) | 3.3V | ок |

## 3. Тактирование (debugfs clk)

- `mmc1` rate = **50 МГц**; `mmc1_sample` = 50 МГц; `mmc1_output` = 50 МГц.
- `mmc1_sample/clk_phase` = **120°**; `mmc1_output/clk_phase` = **90°**.
- `clk_parent` = **200 МГц** (т.е. источник — PLL_PERIPH0/делитель, не OSC24M).
- `bus-mmc1` (AHB-обвязка) — рабочий.

Итог: `50 МГц = 200 МГц (parent) / 4`. Формула H3 sun8i mmc-клока:
`mmc_clk = parent / (N * M)` (подтверждено: 24М/2/16=750 кГц при N=2,M=16).
Для 50 МГц: **N=1, M=4** (200/4). Источник — `CLK_SRC_SEL=01 (PLL_PERIPH0)`.

Фазы для 50 МГц (в нашей таблице `sdc_prog_mmc_phase`): **output=90°, sample=120°**
— совпадает со снятым эталоном (строка `>25 МГц` нашей функции).

## 4. Что это значит для нашего кода

1. **Такт**: `H3_CCU_SDMMC1_CLK = SCLK_GATE | SRC_PLLPERI0 | N=1 | M=4` → 50 МГц.
2. **Фазы**: `sdc_prog_mmc_phase(50e6)` → out=90, smp=120.
3. **Ширина**: 4-bit (CCCR 0x07=0x02 + CTYPE=1) на всю работу (эталон — 4-bit).
4. **HS**: `timing sd-high-speed` — для SDIO HS включается через CCCR 0x13
   (SDR/high-speed) — проверить/включить.
5. Раньше наш «4-bit ломал data-фазу» — вероятно, из-за такта 6 МГц/неверных
   фаз; на 50 МГц + правильные фазы работает в Linux. Подлежит повторной проверке.

## 5. Ссылки

- `sunxi-mmc-4.14.c` — драйвер хоста (полный, у нас в tools/wifi/rtl8189fs).
- `ref/Lakka-Allwinner.orangepi_lite.arm-2.3.1.image` — образ (dtb, SYSTEM).
- `tools/wifi/W233-SDIO-TRACE.md` — наш полный поадресный трейс (для сверки).

## 6. Живая CAM-раскладка и REG_SECCFG (добавлено 2026-10-20)

Снято с той же платы по SSH (10.72.75.55), подключена к той же сети
**A05s пользователя М** (BSSID `9a:4d:56:7e:81:4f`, ch1, WPA2-PSK).
Файлы: `docs/lakka-live/LAKKA-CAM-SECURITY.md`, `docs/lakka-live/LAKKA-MACREG-FULL.txt`.

### CAM (HW, /proc/net/rtl8189fs/wlan0/sec_cam)

```
id ctrl   addr              kid type MK GK valid
 4 0x8010 9a:4d:56:7e:81:4f   0 AES  0  0  1   <- pairwise (BSSID)
 5 0x8051 9a:4d:56:7e:81:4f   1 AES  0  1  1   <- group GTK (BSSID!, keyid=1)
```

- **pairwise** → CAM entry **4**, `ctrl = BIT15|BIT6?0|AES<<2|keyid0`, **addr = BSSID**.
- **group (GTK)** → CAM entry **5** (не 1, не 0), `ctrl = 0x8051` (`BIT15|BIT6|AES<<2|1`),
  **addr = BSSID** (НЕ null!).
- Остальные 0..15 — `ctrl=0`, invalid (мусор/остатки, чип их игнорирует).

### REG_SECCFG

```
reg_scr = 0x010c  (SCR_CHK_KEYID BIT8 | SCR_RxDecEnable BIT3 | SCR_TxEncEnable BIT2)
```

- **Без** `SCR_CHK_BMC` (BIT9), **без** `TXBCUSEDK/RXBCUSEDK` (BIT6/BIT7) — для
  WPA2-PSK (802.1X) их в эталоне НЕТ.
- `sec_info: hw_decrypted=1, aes_sw_dec_cnt=0` — RX-декрипт аппаратный.

### Вывод для bare-metal (W.537)

1. `wpa_cam_write_key(4, bssid, ptk+32, 0)` — pairwise (у нас так).
2. `wpa_cam_write_key(5, bssid, gk, 1)` — **GTK в slot 5 с MAC=BSSID** (а не 1+null).
3. `REG_SECCFG = 0x010C` (16-бит), а не 0xCC/0x30C/0x34C.
4. Прежняя запись GTK в entry 1 с null-MAC не находилась чипом при RX
   (`cam_cache_chk` сверяет MAC) → `swdec=1` на broadcast.

## 7. TX-путь: живые замеры (добавлено 2026-10-20, W.555-W.556)

Снято с той же платы (Lakka 2.3.1, SSH 10.72.75.55), подключена к A05s
(BSSID 9a:4d:56:7e:81:4f, ch1, WPA2). Послужило основой для фиксов W.555/W.556.

### REG_FWHW_TXQ_CTRL (0x0420) — КОРЕНЬ зависания TX

```
0x420 = 0xff311f00   (bit7 = EN_AMPDU_RTY_NEW = 1)
```

- **W.556**: наш код ставил `EN_AMPDU_RTY_NEW` (BIT7) в **0x0818** (неверно!),
  а эталон — в **0x0420** (`_InitRetryFunction`, sdio_halinit.c:632-642).
  Пока бит не стоял, 3-страничные кадры (DHCP) висели в LOQ вечно;
  после исправления — `loq` возвращается, очереди пустеют (лог W.555).

### RQPN (0x0200) — резерв страниц TX

```
0x200 = 0x00E7020C   (HPQ=0x0C/12, LPQ=0x02/2, PUBQ=0xE7/231, без LD_RQPN после инициализации)
```

- LPQ у эталона = **2 страницы** (мы ставим 8 — отклонение-страховка, не критично).
- DHCP (3 стр) у эталона проходит через **PUBQ** (проверка `rtw_hal_sdio_query_tx_freepage`
  = `FreePage[PageIdx] + FreePage[PUBLIC]`).

### Регистры очередей TX (idle, после DHCP)

```
0x400 = 0x017f80ff   (Q0)
0x404 = 0x007f80ff   (Q1)
0x408 = 0x007f80ff   (Q2/BE)
0x40C = 0x007f80ff   (Q3)
0x410 = 0x007f80ff   (MGQ)
0x414 = 0x007f80ff   (HGQ)
0x418 = 0x0fff00f8   (BCNQ)
0x41C = 0x000000f8   (CPU_MGQ)
0x41A = 0x0fff00f8   (TXPKT_EMPTY, 16-bit: 0x00F8)
```

- Наша прошивка после фикса W.555 даёт **идентичные** значения (q0..7 в логе W.555).
- До фикса (W.552): `0x41A = 0x00F80FFE` (bit0=0 — LOQ не пуст, кадр висит).

### TX rate и MACID (живая Lakka)

```
current_tx_rate: id0 9a:4d:56:7e:81:4f MCS7   (0x13 — используем для DHCP, W.556)
macid_info:  id 0 bmc=0 mac=9a:4d:56:7e:81:4f C AP
             id 1 bmc=1 mac=ff:ff:ff:ff:ff:ff RSVD
```

### SDIO FREE_TXPG (sdio_local 0x20)

```
0x20 = 0x0002000c → [0]=HIQ=0x0c(12), [2]=MIDQ=0x02(2)
0x24 = 0x00e70002 → [4]=LOWQ=0x02(2), [6]=PUBQ=0xe7(231)
```

- Формат `HalQueryTxBufferStatus8188FSdio` (sdio_ops.c:2273): чётные байты 8-байтного
  блока = HIQ[0], MIDQ[2], LOWQ[4], PUBQ[6] (16-bit LE).