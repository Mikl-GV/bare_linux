# Промпт для следующего чата (продолжение WiFi-этапа: RX)

> Вставьте этот текст в начало нового чата с MultiTool, чтобы восстановить контекст.
> Дата: 2026-10-18. Проект: `E:\Giga_1\Bare_linux` (Allwinner H3, Orange Pi Lite, RTL8189FTV по SDIO).
> Точка стенда: **W.316**. RAбочая точка git: коммит `22dbb93` (пушнут), тег — см. git/внизу.

---

Продолжаем работу над bare-metal WiFi (RTL8189FTV) в проекте `E:\Giga_1\Bare_linux`.
Перед началом прочитай память проекта (сохранённые заметки), `NEXT-CHAT-PROMPT.md`,
`KNOWLEDGE.md`, `HARDWARE-REF.md`, `REGISTER-MAP.md`, `rtl8189fs-lakka-live-rx-dump-w7.md`.
ИСТОЧНИК ИСТИНЫ по чипу — Linux-драйвер rtl8189fs в `tools/wifi/rtl8189fs/`
(полный, с поддержкой 8188F; версия v4.3.24_15589.20151023 — та же, что в
эталонном образе Lakka 2.3.1, ядро 4.14.18). Сравнивай ТОЛЬКО с ним.

## Текущее состояние (подтверждено на стенде, W.316)

**RX-приём РАБОТАЕТ. Парсер извлекает beacon и SSID из реального эфира:**

```
rx: t=0 ch=1 rxlen=0x127 blk rc=0 blk rc=0 fifo_rd rc=0 read=0x400
    BCN plen=0x10F ch=1 ssid=midea_b8_6515
```

Полная цепочка инициализации работает 1:1 с драйвером:
1. SDIO (1-bit/50 МГц — рабочая шина стенда; 4-bit ломает CMD53): CMD5→CMD3→
   CMD7, chip_id (manf=0x024C, card=0xF179), PowerSeq CARDEMU→ACTIVE, Fn1,
   загрузка FW (fwdl rc=0, MCUFWDL=0xC6 штатное), eFuse/программный MAC.
2. Конфигурация: MAC_REG (101) + BB-конфиг (207) + AGC (66) + RF RadioA (136,
   пропуск записи 0x18) + LSSI + LCK + RFMOD(0x800)=0x83045700 (bCCKEn=bit24,
   bOFDMEn=bit25) + IQK (Tx OK/Rx OK).
3. RX-конфиг: RQPN/NPQ → TX-границы → LLT → rx_bndy → BWOPMODE → SIFS/RL/EDCA →
   PBP/DRVINFO → NETTYPE_AP → RCR=0x7000604E → RRSR → RXFLTMAP → MAR → RXDMA agg →
   REG_CR=0x206FF → HSIMR/C2H clear → HIMR → HISR clear → H2C SET_PWR_MODE →
   CCCR/FBR INT → antenna_sel (LEDCFG2 |= BIT7).

## Симптом (что осталось неработающим)

```
rx: c2h_ping 0x1A0=0 payload=0 hisr=0x21 rxpk=... C2H-ABSENT  <- C2H не идёт
pwon: power_state=3   rx: rpwm=0xC0 cpwm=0 (tog_sw=0)          <- FW не подтверждает
                                                                  ACTIVE по RPWM/CPWM
```

Приём это НЕ блокирует (SSID получен), но **TX-ack и H2C/C2H-обмен отсутствуют**.
Вероятный корень — `power_state=3`/`cpwm=0` (FW ушла/не в активном режиме).
Следующий шаг после успешного RX — подключение к AP (H2C JOIN/CONNECT, C2H),
для чего нужен рабочий RPWM/CPWM.

## Критические предостережения (не наступить дважды)

- `MCUFWDL=0xC6` — ШТАТНОЕ, не сбрасывать биты.
- `REG_CR` — 32-бит RMW (bit17/8/9 теряются при 16-бит).
- HIMR (0x14) — писать 4-байтно CMD53 (CMD52-байт не прилипал: himr_lo=0x07).
- H2C многословный: box0 0x1D0 (id+b0..b2) + EX 0x1F0 (остальные),
  НЕ 0x1D4/0x1D6. **НЕ ПИСАТЬ 0x00 в 0x1CC (HMETFR)** — сбивает valid.
- **H2C 0x6D (BT_WLAN_CALIBRATION) и 0x42 (RSSI_SETTING) НЕ отправлять** — FW
  вешает box0 (valid не снимает), калибрует вхолостую, ломает последующие H2C.
- RPWM: активный запрос = S4 (0x0C)|toggle(0x80) с маской 0xC1 (+0x40 при
  переходе). CPWM поллить по СМЕНЕ bit7, не по cpwm==0.
- Каналы в работе: RF 0x18 = (BW20 0xC00) | ch в [7:0], маска канала 0x3FF.
  НЕ 0x8C01 (bit15 — режим калибровки, ломает приём).
- RFMOD: bCCKEn=BIT24, bOFDMEn=BIT25 (НЕ 16/17!).
- **RX-FIFO выгребается по 1 блоку (512 б) за CMD53** — host SDIO H3 НЕ тянет
  многоблочный read (30 блоков → `rc=-1` + зависание). seq/окно фиксируется на
  пакет, `seq++` только когда RX0_REQ_LEN подтвердил 0.
- `RX0_REQ_LEN` (0x1C) может показывать накопленный объём (не размер одного
  кадра); ориентир завершения пакета — сброс 0x1C в 0 после выдачи.
- **Парсер: 802.11-кадр на смещении off+24** (RX-дескриптор 24 байта, drvinfo
  в этом build = 0). SSID = IE id=0.
- Notch-фильтр: ставить для каналов 5-8 (маски из rtl8188f_phycfg.c:1118-1155),
  для остальных — сбрасывать (`C40 bit9=0`, `D2C bit28=0`).
- CrystalCap (0x24): маска `~0xFFF800`, `cc=(cs<<6)|cs`, `(cc<<11)`.
- Шина стенда: 1-bit/50 МГц (4-bit ломает CMD53 data-фазу).
- Сборка: **`make` без `-j`** (иначе bump_version гоняется параллельно с
  компиляцией main.o и версия в bin не совпадает с version.h). После сборки
  проверять `strings h3_bare.bin | grep 'W\.[0-9]'`.
- **Бэкапы (`*.bak`) хранить в `backups/` в корне**, не в src/.
- Методика: один шаг → лог → анализ; после успеха — комментарии+доки+git+тег.

## Git

Рабочая точка: `22dbb93` (W.316, пушнут в origin/main). Текущая разработка —
W.317 (перечитывать RX0_REQ_LEN снимком, агрегат 0x0101) — в рабочем дереве.