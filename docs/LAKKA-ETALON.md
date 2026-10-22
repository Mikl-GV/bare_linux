# LAKKA-ETALON.md — эталонные замеры живой Lakka 2.3.1 (H3 + RTL8189FTV)

> Консолидация бывших `LAKKA-ETALON-ZAMERY.md`, `rtl8189fs-lakka-live-dump.md`,
> `rtl8189fs-lakka-live-rx-dump-w7.md`, `rtl8189fs-live-snapshot.md`,
> `rtl8189fs-live-snapshot-big.md`, `lakka-live/LAKKA-CAM-SECURITY.md`,
> `lakka-live/LAKKA-LIVE-SNAPSHOT-W537.md`, `lakka-live/LAKKA-MACREG-FULL.txt`.
> Все данные сняты по SSH с платы, загруженной **Lakka-Allwinner.orangepi_lite.arm-2.3.1**
> (тот же образ, что в `ref/`), драйвер 8189fs v4.3.24_15589.20151023, ядро 4.14.18.
> Образ используется только для чтения. Ревизия: 2026-10-21.

## 1. Доступ и конфигурация

- SSH: `root@10.72.75.55` (пароль root), или `root@192.168.1.15` (держал).
- Версия: `Linux Lakka 4.14.18 #1 SMP Fri Sep 13 14:36:07 CEST 2019 armv7l`.
- Модуль: **`8189fs v4.3.24_15589.20151023`** — В ТОЧНОСТИ наша версия
  (`tools/wifi/rtl8189fs`). Прошивка: `/lib/firmware/rtlwifi/rtl8188efw.bin` —
  в живой Lakka 11216 байт; наш `fw_nic.bin` = **18802 байта** (0x4972,
  из образа Lakka `ref/`). Размеры не обязаны совпадать — у нас автономная
  прошивка, сверка ведётся по поведению чипа, а не по байтам файла.
- Интерфейс: `wlan0`, MAC `02:81:38:27:88:cd` (локально-администрируемый).
- Режимы снятия: STA ассоциирован с `MTS_GPON_9290e0` (ch4, RSSI -55) и с
  `A05s пользователя М` (BSSID 9a:4d:56:7e:81:4f, ch1, WPA2-PSK).

## 2. SDIO-шина (ммс1, debugfs ios)

| Параметр | Значение |
|---|---|
| clock | **50000000 Hz** (50 МГц) |
| bus width | **4 bits** |
| timing spec | **2 (sd high-speed)** |
| vdd / power | 21 (3.3~3.4V) / on |
| signal voltage | 0 (3.30V) |

Тактирование (debugfs clk): `mmc1` rate=50 МГц, `mmc1_sample`=50 МГц
(clk_phase **120°**), `mmc1_output`=50 МГц (clk_phase **90°**), parent=200 МГц
(PLL_PERIPH0). Формула H3: `mmc_clk = parent / (N*M)`; 50 МГц = 200/4 (N=1, M=4),
источник `CLK_SRC_SEL=01`. **Фазы: output=90°, sample=120°** (в новом timing mode
CCU-фазы игнорируются — используются NTSR/SAMP_DL, см. HARDWARE §7).

## 3. Ключевые регистры приёма (живой чип, RX OK)

| Адрес | Имя | Живое значение (RX OK) |
|---|---|---|
| 0x100 | REG_CR | 0x000206FF |
| 0x200 | RQPN | 0x00E7020C |
| 0x280 | RXDMA_AGG_PG_TH | 0x2000010F |
| 0x284 | RXPKT_NUM | 0x00020000 (RXDMA_IDLE) |
| 0x290 | RXDMA_MODE | 0x0000000E |
| 0x608 | RCR | 0x700060CE |
| 0x116 | rxff_bndy | 0x3F7F (32-бит: 0x3f7f0ff8) |
| 0x400..0x41C | очереди TX | все 0x007F80FF; 0x41A=0x00F8 |
| 0x420 | FWHW_TXQ_CTRL | 0xFF311F00 (bit7 EN_AMPDU_RTY_NEW) |
| 0x10C | TRXDMA_CTRL | 0xF5B4 (map 0x3F5B0 + low 0x7 + RXDMA_AGG_EN) |
| 0x510 | — | 0x2800660F |
| 0x610.. | MACID/REG | MAC + BSSID |

RF (живой, ch4): RF 0x00=0x00035677 (наш fixed 0x33D95 отличался!), RF 0x18=
0x00000C04 (ch4,BW20), RF 0x0C=0x0, RF 0x30=0x30000, RF 0x52=0xE8231, RF 0x56=0x51CF1.
BB: RFMOD 0x800=0x83045700 (совпадает), 0xC50=0x69553432 (наш 0x69553420 — младший
байт IGI!), 0xA50=0x5B28B300 (TRSSI живой vs наш 0), 0xE30=0x01008C00.

## 4. CAM и REG_SECCFG (WPA2-PSK, A05s) — ВАЖНО для bare-metal

```
/proc/net/rtl8189fs/wlan0/sec_cam:
 id ctrl   addr              kid type MK GK valid
  4 0x8010 9a:4d:56:7e:81:4f   0 AES  0  0  1   <- pairwise (BSSID)
  5 0x8051 9a:4d:56:7e:81:4f   1 AES  0  1  1   <- group GTK (BSSID!, keyid=1)
```

- **pairwise** → CAM entry **4**, ctrl = BIT15|BIT6?0|AES<<2|keyid0, **addr = BSSID**.
- **GTK** → entry **5** (не 1!), ctrl=0x8051, **addr = BSSID** (НЕ null!).
- Остальные 0..15 — ctrl=0 (мусор/остатки, чип игнорирует).
- `reg_scr = 0x010C` = SCR_CHK_KEYID(BIT8)|SCR_RxDecEnable(BIT3)|SCR_TxEncEnable(BIT2).
  БЕЗ CHK_BMC, БЕЗ TXBCUSEDK/RXBCUSEDK (для WPA2-PSK 802.1X их нет).
- `sec_info`: hw_decrypted=1, aes_sw_dec_cnt=0 — RX-декрипт аппаратный.
- ap_info: SSID=A05s, aid=8, macid=0, raid=3, qos_en=1, ht_en=1, ampdu_enable=1.
- macid_info: id0 bmc=0 mac=BSSID C AP; id1 bmc=1 mac=ff:ff:ff:ff:ff:ff RSVD.
- current_tx_rate: MCS7 (0x13).

Вывод для bare-metal (W.537): `wpa_cam_write_key(4,bssid,ptk+32,0)` (pairwise),
`wpa_cam_write_key(5,bssid,gk,1)` (GTK в slot 5 c MAC=BSSID), `REG_SECCFG=0x010C`.
Прежняя запись GTK в entry 1 с null-MAC не находилась при RX — swdec=1.

## 5. TX-путь: живые замеры (W.555-W.556)

- **REG_FWHW_TXQ_CTRL (0x0420) = 0xFF311F00**, bit7 = EN_AMPDU_RTY_NEW —
  КОРЕНЬ зависания TX. Мы ставили BIT7 в 0x0818 (неверный адрес!). Пока бит не
  стоял, 3-страничные кадры (DHCP) висели в LOQ вечно.
- RQPN (0x200) = 0x00E7020C (HPQ=12, LPQ=2, PUBQ=231). DHCP (3 стр) у эталона идёт
  через PUBQ (FreePage[PageIdx] + FreePage[PUBLIC]).
- Интерфейсы TX-очередей: 0x400=0x017F80FF (Q0), 0x404-0x410=0x007F80FF,
  0x418=0x0FFF00F8 (BCNQ), 0x41C=0x000000F8 (CPU_MGQ), 0x41A=0x00F8 (TXPKT_EMPTY).
  Наша прошивка после фикса W.555 даёт идентичные значения.
- SDIO FREE_TXPG (0x20): 0x0002000C → [0]=HIQ=0x0C, [2]=MIDQ=0x02;
  0x24=0x00E70002 → [4]=LOWQ=0x02, [6]=PUBQ=0xE7. Формат HalQueryTxBufferStatus8188FSdio:
  чётные байты 8-байтного блока = HIQ[0], MIDQ[2], LOWQ[4], PUBQ[6] (16-бит LE).

## 6. Полный BB/RF/MAC-дамп живого чипа

Полные дампы живого чипа собраны в разделах выше и в git-истории (коммиты
W.316-W.641). При аудите/сверке использовать этот файл как primary source.
Ключевые расхождения нашего («мёртвого RX») vs живого:

1. **RF 0x00**: живой 0x35677 vs наш fixed 0x33D95 — LNA/RX-path (В.существенные итоги: после W.641 живой чип подтверждает RfAC из RF-таблицы, см. HARDWARE §7).
2. **RF 0x0C**: живой 0x0 vs табличный 0x8992B — аналоговый путь.
3. **BB 0xC50 (IGI/AGC)**: живой 0x69553432 vs наш 0x69553420 (мл. байт IGI).
4. **MAC 0x510**: живой 0x2800660F.
5. **BB 0xA50 (TRSSI)**: живой 0x5B28B300 vs наш 0 — отчёт, не причина.

## 7. Итоги для bare-metal

1. 50 МГц/4-bit/HS — эталон; фазы NTSR/SAMP_DL вместо CCU-phase (new timing mode).
2. RCR=0x700060CE (connected) / 0x7000604E (scan — снять CBSSID_BCN).
3. RQPN с PUBQ=231, LPQ=2 (у нас LPQ=8 — страховка под 3-стр кадры в LOQ, не конфликт).
4. REG_SECCFG=0x010C, CAM: pairwise slot 4, GTK slot 5 с BSSID.
5. TX: EN_AMPDU_RTY_NEW в 0x420, TX-дескриптор с checksum/AGG_BREAK/CCMP-IV,
   byte-mode CMD53 (не block-mode).
6. Хост SDIO H3 не тянет многоблок — 1 блок за CMD53 (ограничение нашего host-стекла,
   в Linux копается дальше mmc-api).