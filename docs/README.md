# Bare_linux — bare-metal для Allwinner H3 (Orange Pi Lite)

Прошивка **с нуля, без ОС и без библиотек**, для одноплатника
**Orange Pi Lite** на SoC **Allwinner H3** (SUN8I 1680, 4× Cortex-A7).

Цель проекта — поднять на голем железе всю периферию платы и её модули:
сейчас работают **звук I2S0 → MAX98357A**, **HDMI-меню**, **SDIO-доступ
к WiFi-чипу RTL8189FTV (приём эфира, сканирование beacon/SSID)**. В планах —
превратить плату в эмулятор ретроконсолей с сетевым доступом к ROM на SD.

> Рабочее состояние (W.391): **полная host-ассоциация к открытой сети A05s
> работает end-to-end** (AuthResp=0 → AssocReq → AssocResp=0 → MSR=STA, rc=0),
> к сети WPA2 (MTS_GPON_9290e0) assoc принят (cap=0x0011, RSN IE 22 байта) и
> AP инициировал 4-way — в RX приходят EAPOL-Key msg1 (etype=0x888E). В эфире
> подтверждён приём data-кадров (ARP/IPv6). Реализованы: скан 13 каналов
> (канал AP берётся из DS Parameter Set IE, а не из свипа), криптоядро WPA2
> host-supplicant (SHA-1/HMAC-SHA1/PBKDF2/PRF/MIC — проверено), фикса
> недетерминизма повторных join. Шина SDIO — 1-bit/50 МГц (4-bit ломает
> data-фазу CMD53). host не тянет многоблочный SDIO read (только 1 блок/CMD53).

---

## Что уже работает (проверено на стенде)

| Блок | Статус | Подробности |
|---|---|---|
| **Звук I2S0 → MAX98357A** | ✅ | Чистый тест-тон 440/697/1209 Гц, DMA (sunxi-DMA → I2S). Работает в FEL и SD+U-Boot. |
| **HDMI-меню 1024×600** | ✅ | Полный подъём DE2/TCON1/HDMI по u-boot (без U-Boot FB), кнопки PCF8574, LED PL10. |
| **WiFi RTL8189FTV (SDIO)** | 🚧 подключение | Полный 802.11 join к открытой A05s (rc=0, MSR=STA); assoc к WPA2 принят (EAPOL msg1 приходит); data-приём в эфире; дальше DHCP/dтатik IP + WPA2 handshake. |
| JTAG-отладка (JTAGprobe/Pico, OpenOCD) | ✅ | Пины PA0-PA3 (function 3), H3 сам JTAG не включает. |

### Этап WiFi (RTL8189FTV) — статус

Подтверждено на стенде:

1. SDIO-инициализация: CMD5 → CMD3 → CMD7 → чтение ID чипа
   (`manf=0x024C card=0xF179` — RTL8189FTV).
2. PowerSeq CARDEMU→ACTIVE **до** включения Fn1 (иначе CMD53 срывается).
3. Загрузка прошивки чипа (fwdl rc=0), eFuse (на OPi Lite пуст),
   программный MAC в REG_MACID 0x610-0x615 (readback совпал).
4. **RF-часть**: включение BB/RF по `PHY_BBConfig8188F`, загрузка полного
   BB-конфига (207 рег.), **LSSI-доступ к RF-регистрам**, калибров **LCK** и **IQK**.
5. **RX-приём** (W.329+): чип принимает эфир на всех 13 каналах, выдача из
   RX FIFO работает (по 1 блоку 512 б за CMD53 — host не тянет многоблок),
   **парсер извлекает beacon и SSID** (`BCN plen=0x10F ch=2 ssid=midea_b8_6515`).
6. **RPWM/CPWM/HIMR/HISR — через fn1** (SDIO-local, не fn0): с W.329 FW
   подтверждает ACTIVE по смене toggle (cpwm=0x80, tog_sw=1), HIMR=0x01.
   Эксперимент W.338: запись 0xCC в HRPWM1 даёт readback 0xCC, но CPWM
   state[3:0] всё равно 0 — ACK на связке идёт только через toggle.
7. **JOINBSS-движок** (W.332-W.338): host передаёт Auth/AssocReq кадры
   (не H2C 0x23 — это P2P_LPS_PARAM); захват BSSID+channel из скана,
   вызов из пункта меню Transfer с Retry (3 скана).

### Известные ограничения (W.338+)

- **host SDIO не тянет многоблочный read** — только 1 блок (512 б) за CMD53;
  попытка прочитать большими записями даёт `rc=-1` и зависание. Поэтому RX-FIFO
  выгребается по одному блоку в цикле.
- **`RX0_REQ_LEN` (0x1C) может показывать большой накопленный объём**, не равный
  размеру одного кадра; ориентиром для завершения служит сброс в 0 после выдачи.
- **Парсер берёт 802.11-кадр на смещении off+24** (RX-дескриптор 24 байта,
  drvinfo в этом build равен 0). SSID из IE с id=0.
- **C2H-ABSENT**: FW не отдаёт C2H-пакеты в scan/связанном состоянии по
  причине, показывающей зависимость от power-state (эксперимент W.338 снял
  эту версию — см. п.6). Ассоциация идёт host-кадрами Auth/Assoc через RX-путь,
  C2H понадобится позже (для JOINBSS-подтверждения/WPA).

Дальше — DHCP/статический IP на открытой A05s (IP авто `x.x.x.199` для пинга
с компа), затем WPA2 4-way handshake к MTS_GPON_9290e0 (криптоядро готово).
Цель — «эмулятор + сетевой доступ к ROM на SD». Подробности:
`NEXT-CHAT-PROMPT.md`, `HARDWARE-WIFI.md`.

---

## Требования к окружению

- **Тулчейн**: `arm-none-eabi-gcc` (AArch32, `-mcpu=cortex-a7 -mfpu=neon -mfloat-abi=hard`).
  Путь по умолчанию в Makefile: `C:/ARM/gcc-arm-none-eabi-15.2.1/bin`.
- **make**: MSYS2 (`C:/msys64/usr/bin/make.exe`) — добавить в PATH.
- **FEL**: `tools/fel/src/sunxi-tools/sunxi-fel.exe` (собран в проекте;
  нужна UCRT-версия `libusb-1.0.dll` из `C:/msys64/ucrt64/bin`).
- **SPL для FEL**: `tools/fel/spl/spl.bin` (U-Boot SPL 2025.01 H3, init DRAM).
- **JTAG** (опционально): OpenOCD `C:/msys64/ucrt64/bin/openocd.exe` +
  адаптер JTAGprobe (CMSIS-DAP, Raspberry Pi Pico).

---

## Сборка

```sh
export PATH="/c/msys64/usr/bin:/c/msys64/ucrt64/bin:$PATH"
cd E:/Giga_1/Bare_linux
make clean
make bin          # соберёт h3_bare.elf и h3_bare.bin (образ на 0x40000000)
```

- Сборка многопоточная при желании: `make -j8 bin`.
- Версия прошивки (`FW vW.xxx`) авто-инкрементируется при сборке
  (`tools/scripts/bump_version.sh`), выводится по UART и на экран.
- Артефакты: `h3_bare.elf`, `h3_bare.bin` — в корне проекта (bin ~67 КБ).

## Заливка

### Через FEL (SPL) — основная для разработки

```sh
FEL=tools/fel/src/sunxi-tools/sunxi-fel.exe
SPL=tools/fel/spl/spl.bin
BIN=h3_bare.bin

"$FEL" writel 0x01C20800 0x00003333   # PA0-3 в JTAG (function 3), ДО spl
"$FEL" spl "$SPL"                     # init DRAM
"$FEL" write 0x40000000 "$BIN"        # записать прошивку
"$FEL" exe 0x40000000                 # запустить
```

### Через SD + U-Boot

`h3_bare.bin` на FAT-разделе карты; `/boot.scr` грузит его на `0x40000000`
и выполняет `go 0x40000000`.

### Отладка

- **JTAG**: конфиг `tools/jtag/h3_pico_jtag.cfg` (IDCODE DAP 0x5ba00477),
  полная инструкция — `tools/jtag/JTAG-SETUP.md`.
- **UART**: UART0 (PA4=TX, PA5=RX), 115200 8N1 — основной вывод отладки.

---

## Структура проекта

```
src/           исходники (main, uart, sys, h3_mmu, h3_dma, i2s, h3_sdc, ...)
include/       заголовки; bb_phy_reg.h / rf_radioa.h — таблицы BB/RF (8188F)
tools/         sunxi-fel (SPL+инструмент), JTAG (config+прошивка Pico),
               wifi/ (референсный Linux-драйвер rtl8189fs + справка),
               scripts/ (bump_version.sh)
ref/           эталон: распакованный образ Lakka 2.3.1 (ref/Lakka-...image,
               НЕ коммитится)
boot.s         стартовый код (векторы, MMU, L1)
linker.ld      линкер-скрипт (код на 0x40000000)
fw_nic.bin     прошивка RTL8189FTV (инкапсулирована в образ)
Makefile       сборка (MSYS2 make, arm-none-eabi-gcc)
```

## Документация

| Файл | Содержание |
|---|---|
| `H3-BAREMETAL-GUIDE.md` | Руководство: сборка, заливка, версии, разбор этапов. |
| `HARDWARE-REF.md` | Аппаратная справка: регистры H3, звук, DMA, JTAG, WiFi/SDIO/RF. |
| `KNOWLEDGE.md` | База знаний: проверенные факты, источники, уроки отладки. |
| `REGISTER-MAP.md` | Снимки значений регистров со стенда (эталон). |
| `NEXT-CHAT-PROMPT.md` | Контекст для продолжения разработки (WiFi: шина SDIO, RF-калибровки, RX). |
| `HARDWARE-WIFI.md` | Справка по чипу RTL8189FTV и драйверу. |

## Git / точки восстановления

Каждая рабочая точка фиксируется тегом:

```
restore-W099-fw-ok     звук + базовый каркас (после чистки репо)
w119-wifi-init         полная инициализация чипа (FW, MAC)
w139-rf-lck            RF-часть: BB-конфиг, LSSI, калибровка LCK
```

---

*Bare-metal драйверы пишутся с нуля по даташитам и эталонному Linux-коду;
проект самодостаточен — собирается на чистой машине с MSYS2 + тулчейном
arm-none-eabi.*