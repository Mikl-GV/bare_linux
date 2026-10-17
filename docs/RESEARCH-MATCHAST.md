# RTL8189FTV — доп. матчасть (research 2026-10-17)

Собрано по итогам параллельного веб-ресёрча. Цель — найти зацепки, почему у нас
probe пишется в TX FIFO, но FW не выдаёт кадр в эфир (`hiq_free` падает 12→0
навсегда), и спланировать целевую инфраструктуру (подключение к сети, приём
файлов с компьютера, запись на SD-карту).

Источники-первоисточники в конце. Все ключевые технические факты сверены с
реальными исходниками rtl8189fs (ap17 / v4.3.x) и продукт-страницами Realtek.

---

## 1. Как Linux/Windows WiFi-стек обращается к буферу RX/TX чипа

Транспорт SDIO у 8188F/8189F — целиком через **CMD53** (многословные
`sdio_memcpy_toio/fromio`) и **CMD52** (регистровые аксессоры).

- Уровень драйвера: `os_dep/linux/sdio_ops_linux.c`: `sd_write()/sd_read()` →
  для `cnt>2` это `sdio_memcpy_toio/fromio` (CMD53, один цельный доступ), для
  `cnt==1/2` — побайтово `sdio_writeb/readb` (CMD52). `sd_*8/16/32` и
  `sd_f0_*8` — отдельные регистровые аксессоры.
- Выбор "какой порт" через `ffaddr2deviceId(pdvobj, addr)` =
  `pdvobj->Queue2Pipe[addr]` (include/hal_sdio.h) — очередь (FH) отображаются
  на номер io-порта (device_id).
- **Схема адреса** (общепринятая у Realtek SDIO): `addr = (device_id << 13) | len`,
  где device_id: `0x01` = WLAN IOREG (регистры), `0x02/0x03/0x04` = TX
  LOQ/MIQ/HIQ, `0x05/0x06/0x07` = RX FIFO. Сдвиг/заворот счётчика в
  `rtw_sdio_write_port`/`rtw_sdio_read_port`.
- Требование выравнивания: данные CMD53 должны лежать в DMA-скратч-буфере
  (комментарий в `_sd_read/_sd_write`).
- **1-bit vs 4-bit**: CMD53 работает на том числе линий, которое согласовано при
  инициализации SDIO (RTL8189FTV-VQ0-CG — SDIO 1.0/1.1/2.0). TX корректно идёт и
  на 1-bit (скорость ниже, механика та же). Наша рабочая сетка 1-bit/50 МГц — валидна.

### Механика RX
- Хост читает RX-буфер `sd_read()` / `sdio_memcpy_fromio` (CMD53) с адресом
  `(WLAN_RX0FF_DEVICE_ID << 13) | len` (RX FIFO = 0x05).
- Триггер: SDIO-прерывание функции 1 (активно, если включены IENM/маска прерываний —
  у нас FBR INT_EN и CCCR IENM уже подняты).
- Разметка RX: `RX_DMA_SIZE = 0x4000` (16К), `RX_DMA_RESERVED_SIZE = 0x80`,
  `RX_DMA_BOUNDARY = 0x4000 - 0x80 - 1`; RX page = 8 Б.
- Кол-во RX-пакетов хост читает через RXDMA-регистры (RXPKT_NUM) и по ним решает,
  сколько выбрать `read_port`.

### Что драйвер делает ПОСЛЕ записи TX-пакета
**Никаких "запросов запустить передачу" нет.** TX запускает сама 8051, когда
видит записанные в TX FIFO страницы (при штатных RQPN/LLT и снятом TXPAUSE).
Хост обязан:
1. правильно разметить FIFO (регистр разметки очередей/границ +
   `rtl8188f_InitLLTTable`);
2. не держать очереди на паузе (`REG_TXPAUSE`, у живого байт0 = 0);
3. отдать FW запрос на выход из сна (H2C SET_PWR_MODE — у нас сделано).

**Вывод по TX-блоку**: если при подтверждённом активном режиме FW (H2C/CPWM
отвечают) probe не ушёл — проблема не в "хвосте" записи хоста, а в FW/физтракте
(неверная разметка/не снят pause, либо физика RF).

---

## 2. Механика TX — критичные требования, чтобы FW выдала кадр

### Структура TX-дескриптора 8188F (40 байт, DWORD-сетка)
- **DW0**: `PKT_SIZE` [0:15], `OFFSET` [16:23] (=TXDESC_SIZE/16=40), `BMC` [24],
  `HTC` [25], `LAST_SEG` [26], `FIRST_SEG` [27], **`OWN` [31]** —
  **OWN=1 обязателен** (бит готовности для DMA).
- **DW1**: `MACID` [0:7], **`QUEUE_SEL` [8:12]** — выбор очереди TX, `RATE_ID`
  [16:20], `SEC_TYPE` [22:23], `PKT_OFFSET` [24:28].
- **DW4**: `TX_RATE` [0:7], `RETRY_LIMIT_ENABLE` [17], `DATA_RETRY_LIMIT` [18:23].
- **DW7**: для SDIO `SDIO_TXSEQ` [16:23] (при агрегации). Для неагрегированного
  кадра достаточно OWN/FIRST/LAST/PKT_SIZE/OFFSET/QUEUE/MACID/RATE.

**Ключевое требование**: `QSEL` в дескрипторе должен совпадать с DeviceID
FIFO-порта, в который пишете. device 4 = HIQ. (драйвер через
`SET_TX_DESC_QUEUE_SEL_8188F(desc, pattrib->qsel)` + `ffaddr2deviceId`.)

### Триггер выдачи — самый вероятный пропущенный шаг
Отдельного регистра-защёлки TX DMA нет. Из `sdio_write_port` (sdio_ops.c):

```
cnt = _RND4(cnt);
HalSdioGetCmdAddr8188FSdio(padapter, addr, cnt >> 2, &addr);  // <- len в СЛОВАХ
err = sd_write(pintfhdl, addr, cnt, xmitbuf->pdata);
```

`HalSdioGetCmdAddr8188FSdio` формирует `(deviceId << 13) | (cnt >> 2)`.
**Младшие 13 бит TX-адреса = число 4-байтных слов, а не байт.**

> НАШЕ СОСТОЯНИЕ: у нас адрес уже считается как `(4<<13) | (total/4)` (h3_sdc.c:2978-2979)
> — ок, зацепка соблюдена.

Для обычного TX в HIQ **достаточно записи в FIFO**, никакого H2C для данных.
`REG_TXDMA_START`/`REG_TXPKT_NUM` в этом драйвере для старта не используются.

### Регистры, выставляемые ДО MACTXEN (порядок rtl8188fs_hal_init)

| Регистр | Назначение | Значение (8188F) | Наш статус |
|---|---|---|---|
| `REG_RQPN_NPQ` (0x40F) | RQPN NPQ | 0x02 | ✔ |
| `REG_RQPN` (0x40C) | `_HPQ\|_LPQ\|_PUBQ\|LD_RQPN` (бит **LD_RQPN** обязателен) | 0x80E7020C | ✔ |
| `_InitTxBufferBoundary` | границы TX, **5 регистров** | см. ниже | ⚠ ПРОПУЩЕН `REG_TDECTRL+1` |
| `rtl8188f_InitLLTTable` | LLT | после boundary | ✔ |
| `_InitNormalChipRegPriority` | `REG_TRXDMA_CTRL` (0x70C) карта оч.→HIQ/MIQ/LOQ | | ? |
| `_InitPageBoundary` | `RX_DMA_BOUNDARY` в `REG_TRXFF_BNDY+2` | | ✔ (0x116) |
| `_InitTransferPageSize` | `REG_PBP` (0x104) page 128 | | ? |
| `REG_CR` | `MACTXEN\|MACRXEN` (16/32-бит read-modify) | | ✔ |
| `SDIO_REG_TX_CTRL` (local 0x00) | чистка [2:0] `&=0xFFFFF8` | | ✔ (W.248, no-op) |

### `_InitTxBufferBoundary` (sdio_halinit.c:304-325) — ВАЖНО
```
txpktbuf_bndy = TX_PAGE_BOUNDARY_8188F;  // 0xF8
rtw_write8(REG_TXPKTBUF_BCNQ_BDNY_8188F, txpktbuf_bndy);  // 0x424
rtw_write8(REG_TXPKTBUF_MGQ_BDNY_8188F,  txpktbuf_bndy);  // 0x425
rtw_write8(REG_TXPKTBUF_WMAC_LBK_BF_HD_8188F, txpktbuf_bndy); // 0x45D
rtw_write8(REG_TRXFF_BNDY, txpktbuf_bndy);               // 0x114
rtw_write8(REG_TDECTRL + 1, txpktbuf_bndy);              // 0x209
```
`REG_TDECTRL = 0x0208` (hal_com_reg.h:174). Значит `REG_TDECTRL+1 = **0x209**`
(НЕ 0x461 — адрес 0x0460 в hal_com_reg.h занят REG_FAST_EDCA_CTRL).

> НАШЕ СОСТОЯНИЕ: 0x424/0x425/0x45D/0x114 ставим все 0xF8 (h3_sdc.c:1792-1795).
> **0x209 (REG_TDECTRL+1) НЕ пишем.** Research прямо указывает: "если TDECTRL
> boundary нулевой/другой, PDMA не знает границу и не начнёт передачу".
> Это НОВАЯ зацепка W.252. Проверить регистры 0x70C (TRXDMA_CTRL) и 0x104 (PBP).

---

## 3. Схожие чипы и драйверы Windows/Linux

RTL8188F/RTL8189F — фактически один radio (die `RTL8188F`, 1T1R 802.11bgn) в
разных корпусах/интерфейсах; суффикс кодирует транспорт, драйвер общий.

| Чип | Транспорт | Примечание |
|---|---|---|
| RTL8188EU/ETV | USB 2.0 | RTL8188E die |
| RTL8188FTV (VC-CG) | USB 2.0 | RTL8188F die |
| RTL8188FTV (VQ0-CG) | SDIO | тот же die — близнец 8189FTV |
| **RTL8189FTV** | **SDIO** | наш чип |
| RTL8189ES | SDIO 1.1/2.0/3.0 | RTL8188E die, старший брат |
| RTL8189ETV | SDIO | E-die |
| RTL8189EM | SDIO | single-chip 1T1R |
| RTL8723BS/DS | SDIO | WiFi+BT combo |

Набор H2C/CPWM/регистров у всего семейства одинаков (мы подтвердили на стенде).

### Драйверы
- **Linux**: `rtl8189fs` (SDIO, ap17/jwrdegoede — тот же v4.3.x, что наш эталон);
  `rtl8188fu` (USB, kelebek333, segv, i2som); `rtl8188eus` (USB, aircrack-ng);
  mainline `rtl8xxxu`.
- **Windows**: RTL8189FTV SDIO — Realtek WLAN SDIO driver 3008.28.201.2016
  (Win10), см. Softpedia. RTL8188FTV USB — driver.ru v1030.25.0701.2017.
  Официальный источник — realtek.com/downloads.
- Взаимозаменяемость прошивок 8188F/8189F на уровне команд FW (H2C) — полная;
  отдельный блоб для SDIO драйвер держит в себе (в LibreELEC `wifi_efuse_8189fs.map`
  не нужен → дефолт драйвера).

### Bare-metal заготовки (без ОС)
- `let-me-go/rtl8189` — настоящий non-OS SDIO-драйвер RTL8189 с `sdc_test`
  (прямая работа с SDIO-регистрами без ОС). Наиболее близок к нашей задаче.
- `zeoneo/rpi-3b-wifi` — bare-metal WiFi для Pi3B без ОС.
- `Infineon/wifi-host-driver` (WHD) — переносимый host-драйвер Cypress (работает
  bare-metal + lwIP), как схема netif ↔ SDIO-WiFi.
- `esp-hosted-mcu` — ESP32 как WiFi-сопроцессор через SDIO/SPI/UART.
- **Зрелого bare-metal драйвера RTL8188F с полным 802.11-стеком в открытом доступе
  НЕТ** — ниша закрыта (чип без публичного SoC-рег. дока). Жизнь bare-metal —
  перенос кода rtl8189fs/rtl8188fu (что мы и делаем).

---

## 4. Граница «хост делает» vs «FW делает» для RTL8189F (802.11i / безопасность)

RTL8188F/8189F — FullMAC-гибрид.

**Чип (8051-FW + хардварный криптодвижок) делает сам:**
- MLME (scan/auth/assoc по RF), beaconing (в AP), управление питанием (LPS/PSM),
- обработка данных и **шифрование/дешифрование data-кадров** (WEP/TKIP/AES-CCMP
  в железе).

**Хост обязан сделать сам:**
- весь 802.11i суппликант для STA: WPA2/WPA3 4-way handshake (и SAE для WPA3),
  PBKDF2/PRF, вычисление **PTK/GTK**, проверку MIC, EAPOL-обмен;
- после handshake — **записать готовые ключи в криптодвижок чипа** (команда
  write key / update key).
- FW НЕ ведёт supplicant для STA (внутренний отключён, аналог Broadcom `sup_wpa=0`).

**Вывод:** для открытой (open) сети data работает на уровне L2 без всякого crypto —
криптодвижок не участвует. Для шифрованной сети нужен мини-суппликант
(аналог wpa_supplicant). Рекомендация: сначала стабильный RX на open-точке,
потом 802.11i.

> Наш стенд: H2C/CPWM отвечает после загрузки FW → MLME-ядро прошивки ЖИВО.
> Мёртвый RX при живом RF-эталоне (Lakka) — это вопрос физического RF/BB/AGC
> тракта, а не host-стороны 802.11.

---

## 5. Bare-metal 802.11 / TCP-IP / сетевое хранилище

- **Zerowi** (Jeremy Bentham, iosoft.blog) — полный bare-metal драйвер CYW43438
  по SDIO с нуля (bit-bang SDIO, CMD52/53, CRC, загрузка FW). Лучший референс
  по структуре.
- **lwIP** на голом железе: режим `NO_SYS` (raw/polled, без RTOS). Интеграция:
  структура `netif` (init/input/link_output), драйвер кормит `netif->input()`.
  Связка netif↔SDIO-WiFi — по образцу Infineon lwip-network-interface-integration.
- **Сетевое хранилище**: для bare-metal реалистичнее **FTP или HTTP** (а не SMB —
  SMB2 на голом железе крайне редок и тяжёл; SMB1 груб). Референсы: NXP AN3931
  (FTP-server на lwIP+FreeRTOS), SEGGER emFTP (структура), STM32H7-LwIP-Examples.

---

## Выводы и зацепки для нашего TX/RX

1. **NEW (W.252)**: пропущен `REG_TDECTRL+1` = **0x209** = 0xF8 в TX-boundary
   (5-й регистр). Research: если не задан, PDMA может не знать границу TX и
   не начать передачу. — кандидат №1 под исправление.
2. Проверить, выставлены ли `REG_TRXDMA_CTRL` (0x70C, карта очередей) и
   `REG_PBP` (0x104, размер TX-страницы 128).
3. TX-адрес `(4<<13)|(total/4)` уже корректен (HIC). QSEL=0x12 (QSLT_MGNT)
   в дескрипторе соответствует mgmt.
4. FW не ждёт H2C для данных — только запись в FIFO при снятом TXPAUSE и
   корректной разметке.

---

## Источники

- rtl8189fs: `hal/rtl8188f/sdio/sdio_ops.c`, `sdio_halinit.c`, `rtl8189fs_xmit.c`
  — ap17/rtl8189fs (github.com/ap17/rtl8189fs)
- rtl8189fs: `include/hal_sdio.h`, `include/rtl8188f_hal.h`, `include/rtl8188f_xmit.h`,
  `include/rtl8188f_spec.h`, `include/hal_com_reg.h`
- Realtek: RTL8189FTV https://www.realtek.com/Product/Index?id=600&cate_id=194
  RTL8189ES https://www.realtek.com/Product/Index?id=594&cate_id=194
  RTL8188FTV https://www.realtek.com/Product/Index?id=595&cate_id=194
- Windows-драйверы: Softpedia (RTL8189FTV 3008.28.201.2016), driver.ru (8188FTV),
  realtek.com/downloads
- Bare-metal: iosoft.blog/zerowi, github.com/let-me-go/rtl8189,
  github.com/zeoneo/rpi-3b-wifi, github.com/Infineon/wifi-host-driver,
  github.com/espressif/esp-hosted-mcu
- FullMAC-анализ (граница хост/FW, sup_wpa=0): github.com/nikhildatta/raspberrypi4-windows-wifi
  blob/master/FULLMAC_RESEARCH.md
- lwIP: sics.se/lwip, lwip-tcpip хостинг, TI/ST/NXP AN3931/AN13799/UM1713

---

## 6. Новые источники (добавлено 2026-10-17)

### Подтверждение SDIO ID 024c:F179 (внешний источник — B860AV1.1T/Armbian)
github.com/maomomo-eth/b860av1.1t-armbian — ZTE B860AV1.1T (Amlogic S905L) под
Armbian Linux 6.12, драйвер того же RTL8189FTV по SDIO. Прямо подтверждает наш
`chip_id`: vendor **`024c`**, device **`F179`** (у нас в логе:
`chip_id rc=0 manf=0x0000024C card=0x0000F179`).

```sh
cat /sys/bus/sdio/devices/mmc0:0001:1/vendor   # 024c
cat /sys/bus/sdio/devices/mmc0:0001:1/device   # F179
```

Ценность для нас:
- у jwrdegoede/rtl8189ES_linux **master-ветка удалила RTL8188F поддержку**; нужно
  фиксировать commit `75a566a830037c7d1309c5a9fe411562772a1cf2` (там RTL8188F
  живёт, `CONFIG_RTL8188F=y`) — подтверждает, что наш эталонный драйвер должен
  собираться с этим commit'ом, не с master;
- перед загрузкой драйвера DTB должен правильно инициализировать SDIO, иначе
  шина не увидит `024c:F179`; для нашей платформы (H3 OPi Lite) это уже сделано;
- это третий независимый пример, что RTL8189FTV (die RTL8188F) реально поднимается,
  подтверждая применимость нашей схемы.

---

### Marvell 88W8801 — работающий bare-metal SDIO WiFi + lwIP на STM32F407 без ОС
github.com/glifeather/marvell_wlan_host_driver — «basic 88W8801 WLAN host driver,
via SDIO interface on STM32F407». Прямое доказательство, что наша схема
(голое железо + SDIO WiFi + lwIP + DHCPS) реализуема:

- Архитектура: **SDIO** (DMA-транзакции к WiFi-модулю), **SPI1** (внешний Flash
  под хранение прошивки чипа), драйвер на **коллбэках, без ОС**.
- Встроен **lwIP 2.1.3** (sics, RT-Thread NAT) + добавлены **DHCP-server и NAT** —
  именно то, что нам нужно под «принимать файлы, представляться хранилищем».
- Чип 88W8801 — 2.4 ГГц 1×1 802.11n SoC, SDIO 2.0 / USB (NXP SDS: 
  docs.ampnuts.ru/nxp.com.datasheet/88W8801-SDS.pdf). Топология похожа на
  RTL8189FTV (SDIO WiFi 1×1).
- Родственный референс: sj15712795029/stm32_sdio_wifi_marvell8801_wifi
  (та же обвязка, LWIP DHCP server, логика Marvell).

**Вывод:** переносимый bare-metal WiFi-хост с SDIO + lwIP не только возможен, но
и реализован публично. Наша структура (netif ↔ SDIO-WiFi ↔ lwIP ↔ SD-хранилище)
полностью ложится на этот паттерн.

### CanMV (Kendryte K210/K230) использует RTL8189FTV — подтверждение низкоуровневой работы
- CanMV K230 SDK (_WLAN driver_) поддерживает **RTL8189FTV/RTL8733BS** и Broadcom
  brcmfmac по SDIO, RW007 по SPI, AIC8800 USB/SDIO, ESP-Hosted SPI/SDIO — включается
  конфигурацией прошивки (kendryte.com/k230_canmv: example/network/wlan.html).
- Наличие опции `REALTEK_SDIO_RTL8189FTV` в средах, близких к bare-metal/RTOS —
  ещё одно доказательство, что этот чип реально поднимается на низком уровне, а
  не только под Linux.
- (eet-china.com/mp/a340357.html на момент обращения не открылся/таймаут, тема —
  встраиваемый WiFi-стек; факт по CanMV подтверждён независимо через kendryte.com)

---

## 7. План-зацепка W.252 (проверить в коде)

Подтверждённая research-зацепка: в `_InitTxBufferBoundary` драйвер пишет TX-boundary
в **5 регистров**, мы — только в 4. Не хватает `REG_TDECTRL + 1` = **0x209** = 0xF8.

- `REG_TDECTRL = 0x0208` (hal_com_reg.h:174); `REG_TDECTRL+1 = 0x209`.
- Research: если TX DMA page boundary (TDECTRL) нулевой/другой, PDMA не знает
  границу и не начинает передачу — прямо соотносится с нашим симптомом.
- Также проверить: `REG_TRXDMA_CTRL` (0x70C, карта очередей HIQ/MIQ/LOQ) и
  `REG_PBP` (0x104, размер TX-страницы = 128) — есть ли они в нашем init.