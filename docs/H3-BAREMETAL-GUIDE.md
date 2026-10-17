# MultiTool Bare-Metal — руководство по сборке, заливке и отладке (Allwinner H3)

> Оранж Пи Лайт (Orange Pi Lite), SoC Allwinner H3 (SUN8I 1680, Cortex-A7).

> **Версия:** 1.5
> **Дата:** 2026-10-13
> **Изменения (1.5):** WiFi-этап: RF-часть ожила — включение BB/RF по драйверу,
> загрузка полного BB-конфига (209 рег.), LSSI-доступ к RF-регистрам (канал
> читается/пишется) и калибровка LCK. Ранее (1.4): полная инициализация
> RTL8189FTV — PowerSeq, fwdl rc=0, eFuse пуст, программный MAC в REG_MACID.

---

## 0. Версионирование

| Версия | Дата | Изменения |
|---|---|---|
| 1.0 | 2026-10-11 | Первая версия после успешного запуска звука (FEL + SD). |
| 1.1 | 2026-10-11 | Уточнения по DMA-когерентности (clean/invalidate), выравнивание, честная секция `.coherent`, эталонный лог, путь DLL. |
| 1.2 | 2026-10-11 | Начало этапа WiFi: версия прошивки FW vW.000 (на UART и на экране внизу справа), базовый SDC1-драйвер, подготовка к чтению чипа RTL8189FTV. |
| 1.3 | 2026-10-11 | Этап WiFi (W.040+): CMD53-блок-запись заработала, CIS (Realtek 0x024C), 4-bit, power-on; добавлены `fw_blob.S` + `fw_nic.bin` и первый запуск загрузки прошивки. |
| 1.4 | 2026-10-13 | WiFi-этап: рабочая последовательность init чипа — PowerSeq до Fn1, fwdl rc=0, eFuse пуст, программный MAC в REG_MACID (mac_set=OK, readback совпал). CMD53: фикс хвостов (write/read) и noinc-доступ к 0x610/0x614. |
| 1.5 | 2026-10-13 | WiFi-этап: RF-часть ожила — включение BB/RF по `PHY_BBConfig8188F`, загрузка полного BB-конфига (209 рег.), LSSI-доступ к RF (канал читается/пишется), калибровка **LCK**. |

### Версия прошивки
- Определена в `include/version.h` (`FW_VERSION_STR`).
- Выводится: по UART при старте (`FW vW.xxx`) и на экран (нижний правый угол).
- Формат: `<этап>.<номер>`; `W` — этап WiFi; номер увеличивается с каждой ревизией.
- При каждом изменении кода обновлять `FW_VERSION_STR` и строку в таблице выше.

---

## 1. Результат — что умеет прошивка

- **Звук I2S0 → MAX98357A**: чистый ровный тест-тон 440/697/1209 Гц (кнопки меню).
- Работает в **обоих** режимах загрузки: **FEL (SPL)** и **SD + полный U-Boot**.
- Кнопки PCF8574 (I2C-битбанг, адрес 0x27), HDMI-меню на готовом FB U-Boot, LED PL10 (индикатор «жив»).
- **WiFi-этап**: полная инициализация RTL8189FTV —
  CMD5/CMD3/CMD7, чтение ID чипа (manf=0x024C card=0xF179), PowerSeq CARDEMU→ACTIVE,
  включение Fn1, **загрузка прошивки (fwdl rc=0, FW READY)**, чтение eFuse
  (на Orange Pi Lite пуст) и **программная установка MAC** в REG_MACID 0x610-0x615
  с подтверждением readback. **RF-часть работает**: включение BB/RF + полный
  BB-конфиг (LSSI-интерфейс), RF-регистры читаются/пишутся через LSSI, канал
  устанавливается, калибровка **LCK** выполнена.
  с подтверждением readback. Полный стек WiFi (802.11/WPA2/TCP/IP) — в разработке.

---

## 2. Требования к окружению

- **Тулчейн**: `arm-none-eabi-gcc` (AArch32, `-mcpu=cortex-a7 -mfpu=neon -mfloat-abi=hard`).
  Путь по умолчанию в Makefile: `C:/ARM/gcc-arm-none-eabi-15.2.1/bin`.
- **make**: MSYS2 (`C:/msys64/usr/bin/make.exe`) — добавить в PATH.
- **FEL-инструмент** (собран, хранится в проекте, НЕ в temp!):
  - `tools/fel/src/sunxi-tools/sunxi-fel.exe`
  - требует **именно UCRT-версию** `libusb-1.0.dll` из `C:/msys64/ucrt64/bin` (собирался в MSYS2 UCRT).
    ⚠️ Если положить не ту DLL (mingw64/MSVCRT) — `sunxi-fel` падает с «DLL not found».
    Чек-лист: если `sunxi-fel version` не работает — добавить `C:/msys64/ucrt64/bin` в PATH
    и убедиться, что DLL именно оттуда.
- **SPL для FEL** (инициализация DRAM, U-Boot SPL 2025.01 H3):
  - `tools/fel/spl/spl.bin` (из Debian-пакета `u-boot-sunxi_2025.01-3+deb13u1_armhf.deb`,
    доска `orangepi_pc_plus`).
- **JTAG**: OpenOCD `C:/msys64/ucrt64/bin/openocd.exe` + адаптер JTAGprobe
  (CMSIS-DAP, Raspberry Pi Pico). Конфиг: `tools/jtag/h3_pico_jtag.cfg`.

**Важно про temp:** `C:/Users/PCB/AppData/Local/Temp/gigatool` периодически чистится
Windows. Всё нужное для проекта хранить в папке проекта — не в temp.

---

## 3. Сборка

```sh
export PATH="/c/msys64/usr/bin:/c/msys64/ucrt64/bin:$PATH"
cd E:/Giga_1/Bare_linux
make clean
make bin          # соберёт h3_bare.elf и h3_bare.bin (образ грузится на 0x40000000)
```

- Артефакты: `h3_bare.elf`, `h3_bare.bin` (в корне проекта).
- Размер бинаря ~10 КБ.

---

## 4. Заливка

### 4.1 Через FEL (SPL)

1. Перевести плату в FEL (держать кнопку FEL при включении).
2. Проверка: `sunxi-fel version` → `AWUSBFEX soc=00001680(H3)`.
3. Заливка (порядок обязателен):

```sh
FEL=tools/fel/src/sunxi-tools/sunxi-fel.exe
SPL=tools/fel/spl/spl.bin
BIN=h3_bare.bin

"$FEL" writel 0x01C20800 0x00003333   # 1) PA0-3 в режим JTAG (function 3)
                                      #    иначе Pico-адаптер не увидит TDI/TDO/TCK/TMS.
                                      #    Делать ДО spl: SPL переинициализирует порт.
"$FEL" spl "$SPL"                     # 2) загрузить и выполнить SPL (инит DRAM)
"$FEL" write 0x40000000 "$BIN"        # 3) записать нашу прошивку в DRAM
"$FEL" exe 0x40000000                 # 4) запустить
```

### 4.2 Через SD + U-Boot

- Файл `h3_bare.bin` кладётся на FAT-раздел карты; `/boot.scr` грузит его на
  `0x40000000` и делает `go 0x40000000`.
- Лог: `## Starting application at 0x40000000`.

---

## 5. ЗВУК: как это работает и почему (итог отладки)

### 5.1 Параметры (эталон, проверены на стенде)

| Параметр | Значение | Где проверить |
|---|---|---|
| Формат FIFO | 32-бит слово, пара `[L<<16][R<<16]`, TXIM=0 (данные в MSB) | I2S0_BASE+0x14 (FCTL), +0x20 (TXFIFO) |
| BCLK / LRCK | 3.072 МГц / 48 кГц (стерео 16 бит) | — |
| I2S CTL | `0x00060115` (BCLK_OUT, LRCK_OUT, SDO0, MODE_LEFT, TX_EN, GL_EN) | I2S0_BASE+0x00 |
| I2S FMT0 | `0x00001F37` (LRCKPER=32, SR=16 бит, SW=32 бита) | I2S0_BASE+0x04 |
| I2S CLKD | `0x00000152` (MCLKO_EN, BCLKDIV=5 (/8), MCLKDIV=2 (/2)) | I2S0_BASE+0x24 |
| PLL_AUDIO_CTRL | `0x91000D0D` (EN, SDM, LOCK, N=14, M=14, PAT=0xC000AC02) | CCU_BASE+0x08 |
| I2SPCM0_CLK | `0x80030000` (bit31=on, mux 11 = PLL_AUDIO 1Х) | CCU_BASE+0xB0 |
| DMA | канал 5–11 (смарт-старт), SEC=0xFFFF, AUTOGATE=0x4 | DMAC_BASE+0x20 / +0x28 |

### 5.2 Корень проблемы (весь путь отладки)

1. **Неправильный mux I2SPCM0_CLK** — нужен `11` (PLL_AUDIO 1X=24.576М), не `00` (8X).
2. **DMA-канал 4 занят SPL** — оказалось недетерминированно (то 4, то 5).
3. **U-Boot прячет таблицу MMU** (TTBR0=0x5FFF4000, L1[0x400..0x402]=0xFFFFFFFF) —
   пометить uncached через её нельзя.
4. ⭐ **ГЛАВНОЕ: H3 не кэш-когерентен для DMA.** CPU пишет LLI/буфер в D-cache,
   а DMAC читает по физическому адресу мимо кэша → получает мусор.
   Симптом в JTAG: по 0x40100000 `CFG=0xea000016`, `CUR_SRC=0x4e4f4765` ("eGON").
5. **Решение — clean D-cache перед стартом DMA** (НЕ перестройка MMU!).

### 5.3 Что НЕ работает (жёсткие уроки)

- ❌ **Нельзя перестраивать TTBR0 на свою таблицу на лету** — CPU ловит Data Abort
  (код уже исполняется из DRAM; кэш/конвейер сбиваются). Ошибка поспешного
  «полного контроля над MMU».
- ❌ **Нельзя отключать MMU на лету** — Undefined Instruction (PC=0x40000004).
- ✅ Единственный стабильный путь — **clean D-cache** для LLI и буфера.

### 5.4 Ключевые функции

- `dcc_clean_range(addr,len)` (src/h3_mmu.c):
  `DCCMVAC (mcr p15,0,c7,c10,1)` по 32-байтным линиям + DSB.
- Вызывается: для `g_lli` в `h3_dma_audio_init` (после заполнения LLI) и для всего
  `_dma_buf` в `i2s_play_freq` (после `synth`).
- `h3_dma_audio_start()` = смарт-старт: остановить канал → дождаться busy=0 →
  чистить pending → DESC_ADDR → EN=1 → проверить `DESC==want` и `busy==1`;
  иначе пробует следующий свободный канал (5..11, затем 0..4).

#### Направление DMA → какая операция нужна

| Направление | CPU до DMA | CPU после DMA | Инструкция |
|---|---|---|---|
| **DMA читает** (playback — наш случай) | **clean** (write-back) | — | `DCCMVAC` (c7,c10,1) |
| **DMA пишет** (capture) | — | **invalidate** | `DCIMVAC` (c7,c6,1) |
| **DMA читает и пишет** (двунаправленно) | clean | invalidate | `DCCIMVAC` (c7,c14,1) |

У нас playback → `DCCMVAC` корректен. Если добавите запись с микрофона — потребуется
`dcc_invalidate_range()` (аналог clean, но `c7,c6,1`) после завершения DMA, иначе
CPU прочитает старые закэшированные сэмплы.

#### Выравнивание по кэш-линии (важно для H3)

- Кэш-линия D-cache Cortex-A7 = **32 байта**, L2 = **64 байта**.
- `DCCMVAC` работает по адресу, а не по диапазону. `dcc_clean_range()` обязан
  выравнивать: `start = addr & ~31`, `end = (addr+len+31) & ~31` (реализовано).
- Проверить, что `sizeof(struct dma_lli)` кратен 32 — сейчас 32 байта (добавлен
  `pad[2]` в h3_dma.c). Секция `.coherent` выровнена через `ALIGN` в linker.ld.
- Если структура LLI не кратна кэш-линии — clean может не покрыть хвост. Это
  источник «плавающих» багов.

---

## 6. Такты/сбросы/защиты (CCU, SMTA, GIC)

- **I2S0**: `GATING2` bit12=1, `RST3` bit12=1 (deassert), `I2SPCM0_CLK` mux=11 + bit31.
- **DMA**: `GATING0` bit6=1, `RST0` bit6=1 (в FEL может быть в reset — условный deassert),
  `SEC_REG (0x20)=0xFFFF`, `AUTO_GATE (0x28)=0x4`.
- **PLL_AUDIO**: `PAT=0xC000AC02`, `CTRL=(1<<31)|(1<<24)|(13<<8)|13`, ждать LOCK (bit28).
- **SMTA** (0x01C23400) — по даташиту это НЕ «TZPC»: регистры SET/CLR (WO).
  DEC1_STA=0xFF, DEC2_STA=0x7F — защит нет.
- **GIC**: DIST_CTL=0x3, CPUIF_CTL=0x3, PMR=0xF8.

---

## 7. Память / линкер

- Код/стек: `0x40000000` (DRAM 512 МБ: `0x40000000..0x5FFFFFFF`).
- `.coherent` — LLI-дескрипторы DMA: `0x40100000` (название условное, см. ниже).
- `.dma_buf` — DMA-буфер 64КБ: `0x40200000`.
- `.fb_hdmi` — резервный кадровый буфер 2.4 МБ: `0x40300000` (в FEL не подключён к DE).
- Проверка адресов: `arm-none-eabi-nm -n h3_bare.elf | grep -E "_coherent|_dma_buf|_fb_hdmi"`.

> ⚠️ **Важно про `.coherent`:** секция НАЗЫВАЕТСЯ так по традиции, но **фактически она
> маппится как cacheable** — U-Boot прячет свою таблицу (TTBR0=0x5FFF4000,
> L1[0x400..0x402]=0xFFFFFFFF) и не даёт переопределить её в uncached. Когерентность
> обеспечивается **вручную** через `dcc_clean_range()` перед каждым стартом DMA.
> Не полагаться на имя секции! Если убрать clean — звук «сломается» (DMAC прочитает
> старые данные).

---

## 8. Диагностика

- По UART (115200): при старте печатаются `=== MultiTool H3 bare-metal ===`,
  `i2c init/ok`, `i2s init/ok` (или `I2S FAIL`), `display init/ok`.
  При нажатии A — `test freq=<число>`; B — `stop`. Это весь вывод: подробные
  регистровые дампы и печать DMA-каналов намеренно убраны из кода (чистота).
- Для диагностики аппаратуры используйте JTAG (см. ниже).

### Эталонный лог успешного запуска (для сравнения)

```
=== MultiTool H3 bare-metal ===
i2c init...
i2c ok
i2s init...
i2s ok
display init...
display ok
test freq=440
```

### JTAG-диагностика

- Скрипты OpenOCD: положил в проект `tools/jtag/` (не в temp, который чистится):
  - `start_ocd.ps1` — запуск openocd с `h3_pico_jtag.cfg`.
  - `telnet_probe.ps1` — чтение DMA-канала и запись тестового дескриптора.
  - `dump_resume.cfg` — одноразовый дамп регистров с `resume`+`shutdown`.
- ⚠️ После снятия снимков ОБЯЗАТЕЛЬНО делать `resume` перед `shutdown`, иначе
  плата остаётся в halt (выглядит как «зависла»).
- Ключевые точки при отладке звука: `DMA_STA` (busy-канал), `DESC_ADDR` канала
  (должен быть `0x40100000`, а не мусор), значение по журналу `L1[0x400..0x402]`
  (в U-Boot = `0xFFFFFFFF`).

---

## 9. WiFi-этап: SDC1 и чип RTL8189FTV

> Этап начат в v1.2 (W.000) и продолжается (W.040+). Подробности по регистрам —
> в `HARDWARE-REF.md`, логика — в `src/h3_sdc.c` и `include/h3_sdc.h`.

### Цепочка инициализации (по коду main.c)

1. `h3_sdc1_init()` — WIFI_EN (PL7), такт MMC1 (OSC24M, OLD mode), пины PG0-5
   (+ drive-strength/pull-up по эталону dtb), сброс контроллера, CKC делитель
   (рабочий такт 6 МГц), SD_FUNS=0xceAA.
2. `h3_sdio_init_chip()` — CMD5 → CMD3 → CMD7.
3. `h3_sdio_read_cis()` — адрес CIS из CCCR 0x09-0x0B, разбор тюплов через CMD52.
4. `h3_sdio_set_bus_width(1)` — 4-bit режим.
5. `h3_wifi_power_on_chip()` — PowerSeq (CARDEMU→ACTIVE).
6. `h3_wifi_download_fw()` — загрузка прошивки (MCUFWDL протокол).

### Ключевые открытия (проверено на стенде)

- **Для data-команд контроллер SDC1 НЕ взводит CMD_DONE** — завершение ждём по
  DATA_DONE (TRANS_COMPLETE). Иначе блочная операция возвращает -2.
- **Шина MMC1 — OLD mode** (бит30=0 в SDMMC1_CLK_REG); MODE_NEW не даёт ответа.
- **SDIO-адресация**: SDIO-local регистры чипа — device id 0, WLAN IOREG — device id 8.
  `ftaddr = (device_id << 13) | (offset & mask)`.
- **Прошивка чипа**: массив NIC-FW извлечён в `fw_nic.bin` (18802 байта),
  встраивается в бинарь через `src/fw_blob.S` (`.incbin`). Загружается по
  протоколу MCUFWDL (REG_MCUFWDL=0x80, стартовый адрес 0x1000).

---

## 10. Горячие клавиши меню

- **A** — тест выбранной частоты (440/697/1209).
- **B** — остановить звук.
- **Up/Down** — выбор частоты.
- **PL10** мигает раз в 500 мс — индикатор «жив».

---

## 11. Чек-лист «что проверить, если звук снова тихий»

1. `I2SPCM0_CLK` == `0x80030000` (mux 11) — иначе нет такта.
2. `GATING0/RST0` bit6=1 (DMA не в reset) — иначе регистры DMA не отвечают.
3. Через JTAG: `DESC_ADDR` канала == `0x40100000` (а не мусор) — иначе LLI не виден DMAC.
4. Через JTAG: `FSTA` наполненный (~0x006A) — иначе DMA не пишет в FIFO.
5. PA10 = HIGH (усилитель включён), питание 5 В у MAX98357A.

## 12. Чек-лист WiFi (этап отладки)

1. После init UART показывает: `sdio_init rc=0`, `ccid0=0x32`, `cis addr=0x1000
   rc=0 manf=0x024C`, `bus4 rc=0`, `pwon rc=0` — базовый SDIO-обмен в порядке.
2. Если `cmd52` к function 1 не отвечает — проверь, что включена функция 1
   (CCCR 0x02 = 0x02) и что WIFI_EN (PL7) поднят.
3. Если `cmd53` блок-запись даёт `-2` — для data-команд нужно ждать DATA_DONE,
   а не CMD_DONE (см. §9).
4. `syscfg` (REG_SYS_CFG 0xF0) читается как `0xEAEAEA`, пока прошивка не
   загружена — это нормально до успешного запуска FW.
5. `sdio100` (SDIO-local 0x100): 0x07 = чип проснулся; 0xEA — в suspend (power-on
   не отработал).