# Bare_linux — bare-metal для Allwinner H3 (Orange Pi Lite)

Прошивка **с нуля, без ОС и без библиотек**, для одноплатника
**Orange Pi Lite** на SoC **Allwinner H3** (SUN8I 1680, 4× Cortex-A7).

Цель проекта — поднять на голом железе всю переферию платы и её модули:
сейчас работает **звук I2S0 → MAX98357A** и идёт разработка **WiFi-стека
RTL8189FTV (SDIO)** — от инициализации чипа до RF-калибровки.

> Рабочее состояние: RTL8189FTV полностью инициализирован
> (PowerSeq → загрузка FW → программный MAC), **RF-часть ожила**:
> LSSI-доступ к RF-регистрам работает, калибровка **LCK** выполнена,
> шина SDIO — 4-bit/6 МГц по эталонному dtb (образ Lakka 2.3.1).

---

## Что уже работает (проверено на стенде)

| Блок | Статус | Подробности |
|---|---|---|
| **Звук I2S0 → MAX98357A** | ✅ | Чистый тест-тон 440/697/1209 Гц, DMA (sunxi-DMA → I2S). Работает в FEL и SD+U-Boot. |
| **HDMI-меню 1024×600** | ✅ | Вывод на готовый FB U-Boot, кнопки PCF8574 (I2C-bitbang), LED PL10. |
| **WiFi RTL8189FTV (SDIO)** | 🚧 в разработке | SDC1 (MMC1), 4-bit, ~6 МГц (эталон dtb sun8i-h3-orangepi-lite); см. ниже «Этап WiFi». |
| JTAG-отладка (JTAGprobe/Pico, OpenOCD) | ✅ | Пины PA0-PA3 (function 3), H3 сам JTAG не включает. |

### Этап WiFi (RTL8189FTV) — статус

Подтверждено на стенде:

1. SDIO-инициализация: CMD5 → CMD3 → CMD7 → чтение ID чипа
   (`manf=0x024C card=0xF179` — RTL8189FTV).
2. PowerSeq CARDEMU→ACTIVE **до** включения Fn1 (иначе CMD53 срывается).
3. Загрузка прошивки чипа (fwdl rc=0), eFuse (на OPi Lite пуст),
   программный MAC в REG_MACID 0x610-0x615 (readback совпал).
4. **RF-часть**: включение BB/RF по `PHY_BBConfig8188F`, загрузка полного
   BB-конфига (209 рег.), **LSSI-доступ к RF-регистрам** (канал
   читается/пишется), калибровка **LCK**.

Дальше — IQK (калибровка I/Q) и RX-путь (скан эфира). Подробности и
следующие шаги: `NEXT-CHAT-PROMPT.md`, `HARDWARE-REF.md`.

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
- Артефакты: `h3_bare.elf`, `h3_bare.bin` — в корне проекта (размер ~10 КБ).

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