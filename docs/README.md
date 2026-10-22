# Bare_linux — bare-metal для Allwinner H3 (Orange Pi Lite)

Прошивка **с нуля, без ОС и без библиотек**, для одноплатника
**Orange Pi Lite** на SoC **Allwinner H3** (SUN8I 1680, 4× Cortex-A7).

Цель проекта — поднять на голем железе всю периферию платы и её модули:
работают **звук I2S0 → MAX98357A**, **HDMI-меню**, **SDIO-доступ
к WiFi-чипу RTL8189FTV** (ассоциация, WPA2 4-way handshake, полный DHCP,
ответы на ping с ПК), **ретро-меню систем + чтение ромов со SD (FAT32)**.

> Рабочее состояние (W.821): **полная host-ассоциация + WPA2 4-way handshake
> + полный DHCP-цикл DISCOVER→OFFER→REQUEST→ACK** (корни найдены — seq в
> TX-дескрипторе, W.770; MSR=0x02 STA вместо 0x01 ADHOC, W.778) **+ пинг с ПК
> работает (0% потерь, ~14 мс)**. Запущены **все 4 ядра Cortex-A7 (SMP)**.
> Работает **ретро-меню**: список систем (из emulation_h3_v2.xlsx) →
> браузер ромов системы из `/roms/<система>` на SD → заглушка эмулятора;
> добавлен **SD-драйвер (SDC0) + FAT32 (чтение)** с поиском целевого
> раздела (с `/roms`) из MBR. К любой сети из скана проходит ассоциация
> (AuthResp=0 → AssocReq → AssocResp=0 → MSR=STA, rc=0): к открытым и WPA2-сетям
> (A05s, MTS_GPON_9290e0). Криптоядро host-supplicant
> (SHA-1/HMAC-SHA1/PBKDF2-PRF/MIC, AES-128, RFC3394 unwrap — проверено на хосте).
> OFFER и ACK принимаются и распознаются (`net: DHCP OFFER/ACK ip=...`),
> REQUEST уходит broadcast с опциями 50/54 (36-байт, без client-id).
> IP сохраняется в global join_target; ARP- и ICMP-ответы работают
> (ping с ПК: ARP request→reply, ICMP echo→reply).
> Шина SDIO — 50 МГц / 4-bit / HS (W.641), host не тянет многоблочный SDIO read
> (только 1 блок/CMD53). Повторные сканы (rescan) работают (W.664).

---

## Что уже работает (проверено на стенде)

| Блок | Статус | Подробности |
|---|---|---|
| **Звук I2S0 → MAX98357A** | ✅ | Чистый тест-тон 440/697/1209 Гц, DMA (sunxi-DMA → I2S). Работает в FEL и SD+U-Boot. |
| **HDMI-меню 1024×600** | ✅ | Ретро-меню систем (systems.h) + браузер ромов с SD + Settings/Help/About. DE2/TCON1/HDMI по u-boot, кнопки PCF8574, LED PL10. |
| **WiFi RTL8189FTV (SDIO)** | ✅ | Полный 802.11 join + WPA2 4-way + TX 3-стр кадров + полный DHCP (DISCOVER→OFFER→REQUEST→ACK, W.770) + **пинг с ПК (0% потерь, W.778)**. Экран Transfer (скан/connect) — внутри Settings. |
| **SD-карта (SDC0) + FAT32** | ✅ | Двухфазный init (Фаза A: наследование U-Boot без CMD0; Фаза B: полный подъём CMD0/8/ACMD41/2/3/7/9, 4-bit), чтение секторов PIO; FAT32-ридер (MBR→раздел с /roms, BPB, каталоги 8.3+LFN, цепочки кластеров) + fat_mkdir (посев папок, не вызывается). |
| **SMP (4 ядра Cortex-A7)** | ✅ | Подъём CPU1..CPU3 при старте (трамплин SRAM A1, R_CPUCFG / PSCI); ядра в idle-цикле (задел под эмуляторы). |
| JTAG-отладка (JTAGprobe/Pico, OpenOCD) | ✅ | Пины PA0-PA3 (function 3), H3 сам JTAG не включает. |

### Этап WiFi (RTL8189FTV) — статус

Подтверждено на стенде:

1. SDIO-инициализация: CMD5 → CMD3 → CMD7 → чтение ID чипа
   (`manf=0x024C card=0xF179` — RTL8189FTV).
2. PowerSeq CARDEMU→ACTIVE **до** включения Fn1 (иначе CMD53 срывается).
3. Загрузка прошивки чипа (fwdl rc=0), eFuse (на OPi Lite пуст),
   программный MAC в REG_MACID 0x610-0x615 (readback совпал).
4. **RF-часть**: включение BB/RF по `PHY_BBConfig8188F`, загрузка полного
   BB-конфига (209 рег.), **LSSI-доступ к RF-регистрам**, калибров **LCK** и **IQK**.
5. **RX-приём** (W.329+): чип принимает эфир на всех 13 каналах, выдача из
   RX FIFO работает (по 1 блоку 512 б за CMD53 — host не тянет многоблок),
   **парсер извлекает beacon и SSID** (`BCN plen=0x10F ch=2 ssid=midea_b8_6515`).
6. **RPWM/CPWM/HIMR/HISR — через fn1** (SDIO-local, не fn0): с W.329 FW
   подтверждает ACTIVE по смене toggle (cpwm=0x80, tog_sw=1), HIMR=0x01.
7. **JOINBSS-движок** (W.332-W.413): host передаёт Auth/AssocReq кадры
   (не H2C 0x23 — это P2P_LPS_PARAM); захват BSSID+channel из скана,
   вызов из пункта меню Transfer. Полная ассоциация (rc=0, MSR=STA)
   к открытым и WPA2-сетям; при смене BSSID шлётся deauth и выполняется
   drain RX FIFO перед новым auth (фикс flaky rc=-4).
8. **DHCP (W.395-W.770)**: после assoc отправляется DHCP DISCOVER
   (IP proto=UDP, broadcast); приём data-кадров после assoc (ARP/IPv6)
   подтверждён. При WPA2 — пересылается после handshake (зашифрованный CCMP).
   OFFER принимается (`net: DHCP OFFER ip=...`), REQUEST (broadcast, опции
   50/54, без client-id) отправлен + ретраи +3с/+6с; **ACK принимается
   (W.770 — корень: seq в TX-дескрипторе)**. IP сохраняется в global
   join_target (W.595). Длина кадра с 8-байтным CCMP IV ≈ 320 Б (3 страницы)
   — данные идут через LOQ (W.705); приём OFFER/ACK распознаётся
   (`net: dhcp 67->68 op=2 msg=2/5`, фикс портов W.590).
9. **WPA2 (W.412-W.597)**: криптоядро host-supplicant готово; против WPA2-AP
   проходит полный 4-way handshake: EAPOL msg1 → msg2 (принят AP, nonce min/max
   фикс) → msg3 (GTK unwrap через RFC3394) → msg4 (key_info 0x030A, secure bit)
   → PTK/GTK в CAM чипа. TX-дескриптор данных: AGG_BREAK, checksum (байты 28-29),
   8-байт CCMP IV, OQT-слот. DHCP-состояние: `dhcp_offer_seen` не сбрасывается
   в DISCOVER, сброс на старте join (W.594), после OFFER DISCOVER не повторяем.

### Известные ограничения (W.338+)

- **host SDIO не тянет многоблочный read** — только 1 блок (512 б) за CMD53;
  попытка прочитать большими записями даёт `rc=-1` и зависание. Поэтому RX-FIFO
  выгребается по одному блоку в цикле.
- **`RX0_REQ_LEN` (0x1C) может показывать большой накопленный объём**, не равный
  размеру одного кадра; ориентиром для завершения служит сброс в 0 после выдачи.
- **Парсер берёт 802.11-кадр на смещении off+24** (RX-дескриптор 24 байта,
  drvinfo в этом build равен 0). SSID из IE с id=0.
- **IPv4/arp приём** — ethertype читается big-endian (0x0800/0x86DD/0x888E),
  что соответствует порядку байт в проводе.
- **RX-агрегация**: чип может выдавать несколько кадров в одном окне
  RX FIFO; парсер проходит по цепочке кадров (24+drvsz+plen, round 8).
- **C2H-ABSENT**: FW не отдаёт C2H-пакеты в scan/связанном состоянии по
  причине, зависящей от power-state. Ассоциация идёт host-кадрами
  Auth/Assoc через RX-путь.

Ближайший шаг: **первый эмулятор + запуск рома с SD** (меню и браузер готовы,
заглушка диспетчера в retro_menu.c); затем WebDAV/HTTP-сервер для доступа
к SD из проводника Windows (связность с ПК подтверждена: ping — 0% потерь, W.778).
Цель — «эмулятор + сетевой доступ к ROM на SD».
Подробности: `NEXT-QUESTIONS.md`, `HARDWARE.md`.

## Системы и папки на SD

Структура раздела с ромами — **как в корне проекта** (`roms/<система>/`; системный
раздел с `boot.scr` не затрагивается). Папки систем созданы в корне проекта
`roms/`; при загрузке меню проверяет наличие каждой папки на SD и ставит `[OK]`
рядом с пунктом (папка не создаётся автоматически). Ромам — лежать в соответствующих
папках. Таблица «система → папка» (папка = `dir` или `id`, по `include/systems.h`):

| Система (пункт меню) | Папка в roms/ | Группа | Статус | Builtin |
|---|---|---|---|---|
| Game Boy / Color | `gameboy` | Portable | READY | — |
| Game Boy Advance | `gba` | Portable | READY | — |
| Sega Game Gear | `gamegear` | Portable | READY | — |
| Atari Lynx | `lynx` | Portable | READY | — |
| Neo Geo Pocket | `ngp` | Portable | READY | — |
| WonderSwan | `wonderswan` | Portable | READY | — |
| Atari 2600 | `a2600` | Console | READY | — |
| Atari 5200 | `a5200` | Console | READY | — |
| Atari 7800 | `a7800` | Console | READY | — |
| NES / Famicom | `nes` | Console | READY | — |
| SG-1000 | `sg1000` | Console | READY | — |
| ColecoVision | `coleco` | Console | READY | — |
| Vectrex | `vectrex` | Console | READY | — |
| Sega Master System | `sms` | Console | READY | — |
| Mega Drive / Genesis | `megadrive` | Console | READY | — |
| PC Engine / TGFX-16 | `pce` | Console | READY | — |
| Super Nintendo | `snes` | Console | PLANNED | — |
| Neo Geo | `neogeo` | Console | PLANNED | — |
| 3DO | `3do` | Console | PLANNED | — |
| PlayStation | `psx` | Console | PLANNED | — |
| Saturn | `saturn` | Console | PLANNED | — |
| N64 | `n64` | Console | PLANNED | — |
| Space Invaders | `invaders` | Arcade | READY | — |
| Pac-Man | `pacman` | Arcade | READY | — |
| Galaxian / Galaga | `galaga` | Arcade | READY | — |
| Donkey Kong | `dkong` | Arcade | READY | — |
| Sega System 1/2 | `segasys1` | Arcade | READY | — |
| Toaplan / Cave (shmup) | `toaplan` | Arcade | READY | — |
| Capcom CPS1 | `cps1` | Arcade | READY | — |
| Sega System 16 | `segasys` | Arcade | READY | — |
| Capcom CPS2 | `cps2` | Arcade | PLANNED | — |
| FB Neo (Arcade) | `fbneo` | Arcade | PLANNED | — |
| Taito F3 | `taito_f3` | Arcade | PLANNED | — |
| Sega Model 1/2 | `model1` | Arcade | PLANNED | — |
| Atari 8-bit (400/800/XL/XE) | `atari800` | Computer | READY | — |
| Apple II / II+ / IIe | `apple2` | Computer | READY | — |
| BBC Micro / Master | `bbcmicro` | Computer | READY | — |
| Enterprise 64/128 | `enterprise` | Computer | READY | — |
| ZX Spectrum | `zxspectrum` | Computer | READY | ✅ builtin |
| MSX / MSX2 | `msx` | Computer | READY | ✅ builtin |
| Amstrad CPC | `cpc` | Computer | READY | — |
| Sinclair QL | `ql` | Computer | READY | — |
| Amstrad PCW | `pcw` | Computer | READY | — |
| Atari ST | `atarist` | Computer | READY | — |
| Amiga 500/600 | `amiga` | Computer | READY | — |
| Macintosh Plus | `macplus` | Computer | READY | — |
| Sharp X68000 | `x68000` | Computer | READY | — |
| Radio-86RK / Specialist | `radio86rk` | Computer | READY | — |
| Agat | `agat` | Computer | READY | — |
| PC XT / PCjr / Tandy | `pcxt` | Computer | READY | — |
| BK-0010 / BK-0011M | `bk0010` | Computer | READY | ✅ builtin |
| UKNC / Korvet | `uknc` | Computer | READY | — |
| NEC PC-8801 | `pc88` | Computer | PLANNED | — |
| Apple IIGS | `iigs` | Computer | READY | — |
| FM Towns | `fmtowns` | Computer | PLANNED | — |
| Commodore 64 | `c64` | Computer | READY | — |
| Osborne 1 / Kaypro | `osborne` | Laptop | READY | — |
| Tandy Model 100 | `tandy100` | Laptop | PLANNED | — |
| Toshiba T1100 | `t1100` | Laptop | READY | — |
| Electronica MS 1504 | `ms1504` | Laptop | READY | ✅ builtin |
| HP 200LX / Poqet PC | `hp200lx` | Laptop | READY | — |
| Atari Portfolio | `portfolio` | Laptop | PLANNED | — |
| TI-83 calculators | `ti83` | Laptop | READY | — |

Пункт меню становится **белым** только когда: папка есть на SD **и** эмулятор
реализован (пока все эмуляторы не написаны — пункты серые). Описание системы
(проц / ОЗУ / частота / год) выводится внизу экрана при выборе.

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
- Артефакты: `h3_bare.elf`, `h3_bare.bin` — в корне проекта (bin ~113 КБ).

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
| `README.md` | Канон: статус проекта, сборка, заливка, структура (этот файл). |
| `HARDWARE.md` | Аппаратная справка H3/OPi Lite: регистры, звук I2S/DMA, CCU/SMTA/GIC, WiFi/SDIO/RF, уроки. |
| `DEVELOPER-GUIDE.md` | Руководство разработчика: сборка, заливка, отладка, архитектура модулей, чек-листы. |
| `LAKKA-ETALON.md` | Эталонные замеры живой Lakka 2.3.1 (шина, регистры, CAM, TX) для сверки. |
| `NEXT-QUESTIONS.md` | Актуальное состояние и ближайшие шаги (DHCP ACK, WebDAV, эмуляторы). |
| `HISTORY.md` | Хроника этапов проекта и ключевых вех. |

## Git / точки восстановления

Точка восстановления — **тег `restore-2026-10-21`** (FW vW.669, после аудита кода
и ревизии документации). Старые теги (`restore-W099-*`, `w1xx-*`, `w3xx-*`) удалены
как устаревшие; история этапов — в `HISTORY.md`.

```
git log --oneline -15
```

---

*Bare-metal драйверы пишутся с нуля по даташитам и эталонному Linux-коду;
проект самодостаточен — собирается на чистой машине с MSYS2 + тулчейном
arm-none-eabi.*