# Промпт для следующего чата (продолжение WiFi-этапа: DHCP после WPA2)

> Вставьте этот текст в начало нового чата с MultiTool, чтобы восстановить контекст.
> Дата: 2026-10-20. Проект: `E:\Giga_1\Bare_linux` (Allwinner H3, Orange Pi Lite, RTL8189FTV по SDIO).
> Точка стенда: **W.556** (собран, прошит, лог W.555/W.556 снят). Рабочая точка git: коммит `16e6b8e` (пушнут); правки W.553-W.556 в рабочем дереве.

---

Продолжаем работу над bare-metal WiFi (RTL8189FTV) в проекте `E:\Giga_1\Bare_linux`.
Перед началом прочитай память проекта (сохранённые заметки), `NEXT-CHAT-PROMPT.md`,
`KNOWLEDGE.md`, `HARDWARE-REF.md`, `REGISTER-MAP.md`, `LAKKA-ETALON-ZAMERY.md`.
ИСТОЧНИК ИСТИНЫ по чипу — Linux-драйвер rtl8189fs в `tools/wifi/rtl8189fs/`
(полный, с поддержкой 8188F; версия v4.3.24_15589.20151023 — та же, что в
эталонном образе Lakka 2.3.1, ядро 4.14.18). Сравнивай ТОЛЬКО с ним.
ЖИВОЙ эталон — Lakka 2.3.1 на той же плате по SSH: `root@10.72.75.55` (пароль root,
plink `D:\Program Files\PuTTY\plink.exe`).

## Текущее состояние (подтверждено на стенде, W.555/W.556)

**WPA2 4-way handshake + TX больших (3-страничных) кадров РАБОТАЮТ. Осталось: DHCP OFFER.**

- **W.553**: добавлен 8-байтный CCMP header (IV/PN) для Protected кадров
  (эталон rtw_xmit.c:2198 `last_txcmdsz = hdrlen + iv_len + llc_sz + mem_sz`;
  IV физически в кадре между 802.11 hdr и LLC). Длина DHCP стала 320 Б.
- **W.554**: блочный CMD53 для кадров >= 3 страниц — НЕ подтвердился как корень, оставлен.
- **W.555 (лог W.555)**: **корневой фикс зависания TX** — `EN_AMPDU_RTY_NEW`
  (BIT7) регистра **0x0420** (`REG_FWHW_TXQ_CTRL`) не ставился (писали в неверный
  0x0818). После фикса: `loq=8` (было 5), `txempty=0x00F80FFF` (было 0xFE),
  `q0..7 = 017F80FF 007F80FF ... 0FFF00F8 000000F8` — **ИДЕНТИЧНО живой Lakka**.
  Кадр уходит из очереди в эфир. Сравнением с живой Lakka подтверждено:
  0x420 у неё = 0xff311f00 (BIT7=1).
- **W.556**: после W.555 кадр уходит, но `txrpt got=0` (нет ACK от AP на unicast
  DISCOVER) и OFFER нет. Гипотеза: скорость. Мы слали DHCP на 1M (TX_RATE=0),
  эталон Lakka — на **MCS7 (0x13)** (`current_tx_rate=MCS7`). Для DHCP (0x0800)
  поставлен TX_RATE=0x13 (MCS7), EAPOL остался 1M. **Лог W.556 не получен —
  это следующий шаг.**

## Симптом (что осталось неработающим)

После `cam keys loaded` → `msg4` → probe/bcast (2 стр) проходят, DISCOVER (3 стр)
раньше висел в LOQ (loq=5, txempty bit0=0). **W.555 это вылечил (0x420)** — кадр
уходит. Но `txrpt got=0` (нет ACK) и `rx etype=0x0800 plen=300+` (DHCP OFFER)
НЕ приходит; видны только фоновые `etype=0x0800 plen=88/113` (ARP/IPv6 от AP).
Ждём лог W.556 (MCS7): если `txrpt got=1` + OFFER — закрыто; если нет — копать
содержимое DHCP-пакета/RX-фильтр unicast.

## Диагностика в логе (уже есть)

- `tx camx dst=4/5 fc=0x4188 sec=3 len=320 rate=0x13 use=1 qos=0` — дескриптор;
- `join: dhcp_tx rc=0 loq=8 hiq=12` — страницы вернулись (кадр ушёл);
- `join: txempty=0x00F80FFF(...)` — LOQ пуст;
- `join: q0..7=017F80FF ...` — очереди Q0..Q7/MG/HI (0x400-0x41C) — сравнение с Lakka;
- `join: txrpt got=0 c2h=0x00000000` — нет C2H TX-отчёта (ACK);
- `join: rx etype=0x00000800 plen=88/113 fc=0x00004208 swdec=0` — фоновый RX.

## Ключевые найденные корни (не наступить дважды)

1. **Зависание TX 3-страничных кадров в LOQ** → `EN_AMPDU_RTY_NEW` (BIT7) в
   **0x0420** (REG_FWHW_TXQ_CTRL). Регистр 0x420 = 0xff311f00 у живой Lakka.
2. **Чеcksum TX-дескриптора** (rtl8188f_cal_txdesc_chksum, XOR 16 слов → байты 28-29)
   — обязателен для SDIO, но НЕ корень зависания (W.556 пометил).
3. **CCMP IV (8 байт)** в кадре для Protected — обязателен, иначе длина кадра
   не совпадает с ожидаемой FW.
4. **CAM раскладка** (W.537, живая Lakka): pairwise = entry 4 (0x8010, BSSID),
   GTK = entry 5 (0x8051, BSSID, keyid=1), REG_SECCFG=0x010C.
5. **LPQ=2 у эталона** (живая Lakka 0x200=0x00E7020C: HPQ=12, NPQ=2, LPQ=2, PUBQ=231);
   мы ставим LPQ=8 — отклонение НЕ критичное (страховка), но при сверке помнить:
   у эталона DHCP идёт на 3 стр через **PUBQ** (свободно LOWQ+PUBQ).

## Логи прогонов (свежие)

- W.552 (лог): DHCP len=320, всё ещё висел (loq=5) — до 0x420-фикса.
- W.555 (лог): после 0x420-фикса loq=8, txempty=0xFF, q0..7=как Lakka, OFFER нет.
- W.556: собрана (MCS7 для DHCP), лог НЕ снят — следующий шаг.

## Git

Рабочая точка: `16e6b8e` (W.550, пушнут). Правки W.553-W.556 — в рабочем дереве
(включая исправления комментариев). После успеха — коммит+пуш.

## Инструменты Lakka (живой эталон)

- `plink -ssh -batch -pw root -hostkey "SHA256:VRQSUQbd6C8RxitLiF+HpzM4UyYJopILgS7LBNMVb7E" root@10.72.75.55 "<cmd>"`
  (hostkey зафиксирован; plink: `D:\Program Files\PuTTY\plink.exe`).
- Важные proc-файлы: `mac_reg_dump`, `sdio_local_reg_dump` (FREE_TXPG 0x20:
  [0]=HIQ, [2]=MIDQ, [4]=LOWQ, [6]=PUBQ), `mac_qinfo`, `sec_cam`, `macid_info`,
  `current_tx_rate`, `ap_info`, `hal_spec`.
- Скрипты снятия: `tools/lakka_tx1.sh`, `tools/lakka_live2.sh`, `tools/lakka_tx_probe.sh`.
- Lakka уводит UART на HDMI — логи только по SSH.