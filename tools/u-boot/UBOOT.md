# U-Boot для Orange Pi Lite (H3)

Документ описывает загрузчик, которым прошивка `h3_bare.bin` запускается на
Orange Pi Lite: состав, сборку, способы загрузки и ограничения.

## 1. Назначение

- Загружается **через FEL** (`sunxi-fel uboot`) или **с SD-карты** (offset 8 КиБ).
- Поднимает максимум периферии H3, доступной на этой плате.
- Автоматически находит `h3_bare.bin` на USB-флешке или SD и запускает её
  по адресу `0x40000000`.
- Даёт консоль на UART0 и на HDMI, чтобы можно было прервать автозагрузку
  и выполнить команды вручную.

## 2. Артефакты

Каталог `build/u-boot/` (в git не попадает — `*.bin` в `.gitignore`,
пересобирается скриптом):

| Файл | Размер | Что это и когда использовать |
|------|--------|------------------------------|
| `u-boot-sunxi-with-spl.bin` | 586 776 Б | **Единственный файл для загрузки.** eGON-заголовок + SPL (24 КиБ) + U-Boot proper. FEL: `sunxi-fel uboot ...`; SD: `dd ... seek=8`. |
| `sunxi-spl.bin` | 24 576 Б | Только SPL (eGON): инициализация DRAM и передача управления. Нужен для `sunxi-fel spl` в паре с ручным `write`/`exe` (так делает `tools/fel/load_fel.sh`). USB, консоли и автозагрузки в нём нет. |
| `u-boot.bin` | 553 944 Б | Только U-Boot proper, без SPL и eGON-заголовка. **Не загружается** ни через FEL, ни с SD. Для анализа: `nm`, `objdump`, `strings`, сравнение сборок. |
| `u-boot.config` | 57 217 Б | Не бинарник: итоговый `.config` сборки (аудит и воспроизведение). |

Коротко: **для FEL и SD — `u-boot-sunxi-with-spl.bin`; для ручной FEL-заливки
через SPL — `sunxi-spl.bin`; `u-boot.bin` — только для отладки и анализа.**

md5 `u-boot-sunxi-with-spl.bin`: `8a844d4febea9fa1d1c7e8d39bd65e31`.

## 3. Версия и сборка

- U-Boot **v2025.01** (тег `v2025.01`, коммит `6d41f0a3`), источник —
  `https://source.denx.de/u-boot/u-boot.git`.
- Конфигурация: `orangepi_lite_defconfig` (DTS `sun8i-h3-orangepi-lite`)
  + фрагмент [`tools/uboot/orangepi_lite_periph.config`](../tools/uboot/orangepi_lite_periph.config).
- Кросс-компилятор: `arm-linux-gnueabihf-gcc`.
- Воспроизводимая сборка:

```sh
tools/uboot/build.sh
# результат: build/u-boot/u-boot-sunxi-with-spl.bin
```

Скрипт клонирует исходники в `/tmp/gigatool/u-boot` (можно задать `UBOOT_SRC`),
накладывает фрагмент через `scripts/kconfig/merge_config.sh`, собирает и
копирует артефакты в `build/u-boot/`.

## 4. Что включено

### 4.1. Уже в `orangepi_lite_defconfig`

| Блок | Опции / драйверы |
|------|------------------|
| USB host | `USB`, `DM_USB`, `USB_EHCI_HCD` + `USB_EHCI_GENERIC` (порты 1 и 2), `USB_OHCI_HCD` + `USB_OHCI_GENERIC` (компаньон для USB 1.1), `USB_STORAGE`, `USB_KEYBOARD`, поддержка хабов |
| SD/MMC | `MMC`, `DM_MMC`, `MMC_SUNXI`, `CMD_MMC`, `MMC_WRITE` |
| GPIO | `DM_GPIO`, `SUNXI_GPIO`, `CMD_GPIO` |
| I2C | `I2C`, `DM_I2C`, `I2C_EDID` (DDC/EDID для HDMI) |
| Видео | `VIDEO`, `VIDEO_DE2` (Display Engine 2), `VIDEO_DW_HDMI`, `VIDEO_DT_SIMPLEFB`, логотип |
| Консоль | `CONSOLE_MUX`, `SYS_CONSOLE_IS_IN_ENV` — можно переключать `stdin/stdout/stderr` |
| Регуляторы | `DM_REGULATOR`, `DM_REGULATOR_FIXED` |
| Watchdog | `WDT`, `WDT_SUNXI`, `SYSRESET_WATCHDOG` |
| Файловые системы | `CMD_FAT`, `FAT_WRITE`, distro boot (`BOOTSTD`) |

Питание: `SUNXI_NO_PMIC=y` — PMIC-драйвер не используется, питание фиксированное
(как в штатном defconfig платы).

### 4.2. Добавлено фрагментом

| Блок | Опции | Зачем |
|------|-------|-------|
| Автозагрузка | `BOOTDELAY=3`, `PREBOOT`, `BOOTCOMMAND` | `usb start`, `mmc rescan`, консоли, поиск и запуск `h3_bare.bin` |
| SPI | `SPI`, `DM_SPI`, `SPI_SUNXI`, `CMD_SPI` | Драйвер собран; узел `spi0` в DTS платы `disabled`, шина не создаётся |
| PWM | `DM_PWM`, `PWM`, `PWM_SUNXI`, `CMD_PWM` | Драйвер собран; узел `pwm` в DTS `disabled` |
| LED | `LED`, `LED_GPIO`, `CMD_LED` | `pwr`/`status` светодиоды из DTS; пины настраиваются только командой `led` |
| Диагностика | `CMD_I2C`, `CMD_REGULATOR`, `CMD_CLK`, `CMD_DM`, `CMD_PART`, `CMD_EXT4`, `CMD_FS_GENERIC` | Осмотр устройств; `i2c bus` пуст, узел `i2c0` в DTS `disabled` |
| USB OTG | `USB_MUSB_GADGET`, `USB_MUSB_SUNXI`, `USB_GADGET_DOWNLOAD`, `CMD_USB_MASS_STORAGE` | Драйверы OTG/`ums` собраны, но узел `usb_otg` в DTS платы `disabled` — в этом образе gadget не активируется (см. раздел 14) |

Важно: драйверы I2C/SPI/PWM собраны, но узлы `i2c0`, `spi0`, `spi1`, `pwm`
в DTS платы стоят `disabled`. U-Boot их не создаёт и не настраивает pinmux;
`spi list` и `i2c bus` в этом образе пусты. Чтобы шины появились, нужно
включить узлы в DTS и пересобрать.

## 5. Чего нет и почему

| Периферия | Причина |
|-----------|---------|
| Ethernet | На Orange Pi Lite нет PHY; узел `emac` в DTS H3 есть, но в DTS платы не включён (`disabled`) |
| WiFi RTL8189FS | SDIO-чип; драйвера RTL8189 в U-Boot нет. Узел `mmc1` включён, но SDIO-устройство не драйвится: MMC-инициализация на нём не проходит |
| Термодатчик (thermal) | В U-Boot 2025.01 нет sunxi-драйвера thermal |
| IR-приёмник | В U-Boot 2025.01 нет sunxi-драйвера IR |
| ADC/GPADC | Драйвера нет |
| PMIC (AXP) | Плата без AXP в этой конфигурации, `SUNXI_NO_PMIC=y` |

## 6. Загрузка

### 6.1. Через FEL (без SD и без USB-флешки)

Плата переводится в FEL (кнопка FEL при включении), затем одной командой
загружаются и U-Boot, и прошивка:

```sh
sudo sunxi-fel uboot build/u-boot/u-boot-sunxi-with-spl.bin \
                 write 0x40000000 h3_bare.bin
```

Порядок важен: `uboot` инициализирует DRAM и переносит U-Boot proper,
следующий `write` кладёт `h3_bare.bin` в DRAM по 0x40000000. Когда
`sunxi-fel` завершается, стартует U-Boot: `misc_init_r` видит FEL-загрузку
(`fel_booted=1`), `preboot` поднимает USB и MMC, а `bootcmd` первой веткой
делает `go 0x40000000`. Карту и флешку можно не трогать; SD, если вставлена,
остаётся для ROM-ов. Сценарий собран и сверен по коду U-Boot, но на живом
стенде пока не прогонялся.

Только U-Boot, без прошивки (дальше — вручную в консоли):

```sh
sudo sunxi-fel uboot build/u-boot/u-boot-sunxi-with-spl.bin
```

`sunxi-fel uboot` загружает SPL (24 КиБ) в SRAM, инициализирует DRAM,
переносит U-Boot proper в DRAM и запускает его. После этого работает
автозагрузка (раздел 7). Для запуска только нашей прошивки без U-Boot
по-прежнему можно использовать `tools/fel/load_fel.sh`
(`spl` + `write 0x40000000 h3_bare.bin` + `exe`), но тогда MMC0 не
инициализирован и SD/ROM-ы недоступны (раздел 10.3).

### 6.2. С SD-карты

```sh
make sd
# либо только загрузчик:
sudo dd if=build/u-boot/u-boot-sunxi-with-spl.bin of=/dev/sdX bs=1k seek=8 conv=notrunc
```

Файл `h3_bare.bin` и `boot.scr` кладутся на FAT-раздел (это делает `make sd`).

## 7. Автозагрузка

`preboot` (выполняется перед `bootcmd`):

```
usb start; mmc rescan; setenv stdin serial,usbkbd; setenv stdout serial,vidconsole; setenv stderr serial,vidconsole
```

`bootcmd`:

```
if test -n ${fel_booted}; then echo FEL: starting preloaded h3_bare.bin; go 0x40000000; fi;
if fatload mmc 0 0x40000000 h3_bare.bin; then go 0x40000000; fi;
if fatload usb 0 0x40000000 h3_bare.bin; then go 0x40000000; fi;
run distro_bootcmd
```

Порядок: **FEL-preload → SD-карта → USB-флешка → distro_bootcmd** (там
сработает `boot.scr`, если он есть). Если `h3_bare.bin` не найден, U-Boot
остаётся в консоли.

FEL-ветка срабатывает только при загрузке через `sunxi-fel`: прошивка
должна быть предварительно положена по 0x40000000 (команда в разделе 6.1).
Если запустить U-Boot через FEL без `write 0x40000000 h3_bare.bin`,
`go` уйдёт по тому, что окажется в DRAM.

Прервать отсчёт (`BOOTDELAY=3`) можно любой клавишей в UART или на
USB-клавиатуре (она опрашивается, т.к. `stdin=serial,usbkbd`).

## 8. Консоль

- **UART0**: PA4 (TX) / PA5 (RX), **115200 8N1** — основной путь.
- **HDMI**: `vidconsole` (DE2 + DW HDMI). Кадровый буфер U-Boot размещает
  в верхней части DRAM; после `go` прошивка сама перенастраивает DE/HDMI
  под свой режим, поэтому «наследство» U-Boot ей не мешает.
- Переключение консолей — переменные `stdin`/`stdout`/`stderr`
  (`serial`, `vidconsole`, `usbkbd`).

## 9. Память

- `CONFIG_TEXT_BASE=0x4A000000` — U-Boot proper линкуется сюда, при старте
  переносится в верх DRAM.
- Наша прошивка грузится по `0x40000000` и использует фиксированные зоны
  (`0x4A400000` — uncached, `0x4A40C000` — L1-таблица, `0x4A500000` — DMA,
  `0x4A700000` — куча, `0x4F000000` — арена меню, `0x5F800000`/`0x5F900000` —
  FB, `0x60000000` — стек). Пересечение с адресом линковки U-Boot безопасно:
  к моменту `go` U-Boot уже не исполняется из этих адресов.
- `go 0x40000000` не возвращает управление: прошивка забирает систему целиком.

## 10. Регистры: что и как настраивает U-Boot

Значения ниже — из драйверов U-Boot 2025.01 (`drivers/clk/sunxi/clk_h3.c`,
`arch/arm/mach-sunxi/clock_sun6i.c`, `drivers/mmc/sunxi_mmc.c` и др.) и из
снимка JTAG после SD-загрузки (`REGISTER-MAP.md`). Адреса физические:
MMU U-Boot — identity-map.

### 10.1. CCU (0x01C20000)

| Регистр | Адрес | Значение после U-Boot |
|---------|-------|------------------------|
| `PLL_CPU_CTRL` | 0x000 | PLL_CPU = 1008 МГц (`clock_set_pll1(CONFIG_SYS_CLK_FREQ)` в `sunxi_board_init`, SPL) |
| `PLL_DDR_CTRL` | 0x020 | PLL_DDR = 672 МГц (`CONFIG_DRAM_CLK=672`, настройка DRAM в SPL) |
| `PLL_PERIPH0_CTRL` | 0x028 | 600 МГц (`PLL6_CFG_DEFAULT = 0x90041811`, `clock_init_safe`) |
| `AHB1_APB1_CFG` | 0x054 | `0x00003180`: AHB1 = PLL6/3 = 200 МГц, APB1 = AHB1/2 = 100 МГц |
| `APB2_CFG` | 0x058 | OSC24M, N=1, M=1 (UART0) |
| `PLL_AUDIO_CTRL` | 0x008 | **выключен**; после U-Boot читается `0x00035514` (EN=0) |
| `I2SPCM0_CLK` | 0x0B0 | `0` — I2S0 без такта |

Гейты (бит 1 = такт включён) и сбросы (бит 1 = сброс снят), которые U-Boot
выставляет при работе соответствующих драйверов:

| Блок | Gate | Reset | Тактирование |
|------|------|-------|--------------|
| MMC0 / MMC1 | 0x060 бит 8 / 9 | 0x2C0 бит 8 / 9 | SD0 0x088, SD1 0x08C |
| SPI0 / SPI1 | 0x060 бит 20 / 21 | 0x2C0 бит 20 / 21 | 0x0A0 / 0x0A4 бит 31 |
| USB OTG | 0x060 бит 23 | 0x2C0 бит 23 | — |
| EHCI0…3 | 0x060 биты 24…27 | 0x2C0 биты 24…27 | — |
| OHCI0…3 | 0x060 биты 28…31 | 0x2C0 биты 28…31 | 0x0CC биты 16…19 |
| USB PHY0…3 | 0x0CC биты 8…11 | 0x0CC биты 0…3 | — |
| DE | 0x064 бит 12 | 0x2C4 бит 12 | 0x104 бит 31 |
| TCON0 | 0x064 бит 3 | 0x2C4 бит 3 | 0x118 бит 31 |
| HDMI | 0x064 бит 11 | 0x2C4 биты 10/11 | 0x150 бит 31; DDC 0x154 бит 31 |
| PIO | 0x068 бит 5 | — | — |
| I2C0…2 | 0x06C биты 0…2 | 0x2D8 биты 0…2 | — |
| UART0…3 | 0x06C биты 16…19 | 0x2D8 биты 16…19 | — |

Фактически на этой плате U-Boot включает: UART0, PIO, MMC0 (SD), MMC1
(узел включён, но SDIO не драйвится), EHCI1/OHCI1 + EHCI2/OHCI2 и их PHY
(два USB-A), DE/TCON0/HDMI при выводе на HDMI. Узлы `i2c0`, `spi0`,
`spi1`, `pwm` в DTS платы `disabled` — U-Boot их не поднимает, хотя
драйверы собраны. LED-пины U-Boot не трогает до команды `led`.

Аудио U-Boot не поднимает: `PLL_AUDIO` выключен, `I2SPCM0_CLK = 0`,
I2S0 в gate + reset (`GATING2` 0x068 бит 12 = 0, `RST3` 0x2D0 бит 12 = 0).
При этом DMA по снимку JTAG **включён**: `GATING0` (0x060) бит 6 = 1
(такт есть) и `RST0` (0x2C0) бит 6 = 1 (сброс снят); HSTMR (бит 19) = 0.
Драйвер `clk_h3.c` гейты DMA/HSTMR/I2S не перечисляет — это состояние
унаследовано от SPL/BROM. `DMA_SECURE_REG` (0x01C02020) = 0, `AUTO_GATE`
(0x01C02028) = 0; DMA/CCU остаются Secure — прошивка снимает это через
SMTA и всё равно переинициализирует DMA.

GIC (0x01C81000/0x01C82000) U-Boot не инициализирует: в конфиге нет
`CONFIG_GICV2`, драйвер не используется.

### 10.2. PIO (0x01C20800, R_PIO 0x01F02C00)

Раскладка на порт: `CFG0…CFG3` — 0x00/0x04/0x08/0x0C (по 4 бита на пин),
`DATA` — 0x10, `DRV0/DRV1` — 0x14/0x18, `PULL0/PULL1` — 0x1C/0x20
(по 2 бита на пин). U-Boot применяет mux/pull/drive из DTS только для
включённых узлов; `i2c0`, `spi0`, `spi1`, `pwm` в DTS платы `disabled`,
поэтому их пины U-Boot не настраивает.

| Что | Пины | Функция | Настройки |
|-----|------|---------|-----------|
| UART0 | PA4 TX, PA5 RX | 2 | — |
| MMC0 (SD) | PF0…PF5 | 2 | drive 30 Ом (`DRV=2`), pull-up (`PULL=1`) |
| MMC0 card-detect | PF6 | 0 (вход) | вход active low; отдельный pull-up в DTS не задан |
| MMC1 (SDIO WiFi) | PG0…PG5 | 2 | drive 30 Ом, pull-up (SDIO не драйвится) |
| I2C0 (Sega-пад) | PA11 SCL, PA12 SDA | 2 | **не U-Boot**: узел `i2c0` disabled; настраивает прошивка |
| SPI0 | PC0…PC3 | 3 | **не U-Boot**: узел `spi0` disabled |
| SPI1 | PA13…PA16 | — | функции `spi1` в таблице pinctrl H3 нет; не использовать |
| LED pwr | PL10 (R_PIO) | output | U-Boot не трогает до команды `led`; включает прошивка |
| LED status | PA15 | output | то же |

### 10.3. MMC

- Контроллеры: MMC0 0x01C0F000 (SD), MMC1 0x01C10000 (SDIO WiFi),
  MMC2 0x01C11000. Тактирование — CCU 0x088/0x08C/0x090.
- Драйвер пишет `GCTRL` (0x00): reset/FIFO reset; `CLKCR` (0x04):
  enable + делитель; `WIDTH` (0x0C) — 1/4 бита; `RINT` (0x38);
  `NTSR` (0x5C) — тайминги; FIFO — 0x100. Проходит CMD0…CMD7.
- После `mmc rescan` карта находится в Transfer state, и CMD17 читает
  сектор. Прошивка это наследует: `sd.c:131` — «Ничего не сбрасываем —
  наследуем настройки U-Boot», дальше только CMD17 + PIO.
- Следствие: при загрузке **чисто через FEL** (без U-Boot) MMC0 никем не
  инициализирован — `sd_init()` полагается на состояние от U-Boot, поэтому
  SD/ROM могут быть недоступны.

### 10.4. USB

- PHY: `phy_ctrl` 0x01C19400, PMU портов 0x01C1A800 / 0x01C1B800 /
  0x01C1C800 / 0x01C1D800.
- CCU 0x0CC: гейты PHY — биты 8…11, сбросы PHY — биты 0…3, такты OHCI —
  биты 16…19.
- Пары EHCI/OHCI: 0x01C1A000/0x01C1A400 (порт 0), 0x01C1B000/0x01C1B400
  (порт 1), 0x01C1C000/0x01C1C400 (порт 2), 0x01C1D000/0x01C1D400 (порт 3).
- В DTS платы включены `ehci1/ohci1` и `ehci2/ohci2` — это два USB-A;
  OTG (MUSB 0x01C19000) оставлен `disabled`, поэтому gadget/`ums` не
  активируется, хотя драйверы собраны.
- U-Boot оставляет PHY включённым; прошивка переинициализирует такты/PHY
  (`USBPHY_CFG`) и OHCI1/OHCI2 (`usb_ohci.c:246-272, 441-442`).

### 10.5. Видео

- DE2 0x01000000, TCON0 0x01C0C000, HDMI 0x01EE0000, HDMI PHY 0x01EF0000.
- CCU: DE 0x104, TCON0 0x118, HDMI 0x150, HDMI DDC 0x154; гейты/сбросы —
  в таблице выше.
- Драйвер DE2 пишет служебные `RST/GATE/BUS`, включает mixer, настраивает
  слои и blender; DW HDMI инициализирует контроллер и PHY.
- U-Boot поднимает видео при первом выводе в `vidconsole`; прошивка затем
  переинициализирует DE2/HDMI под 1024×600 (`h3_de2.c`).

### 10.6. Прочее

- I2C: 0x01C2AC00 / 0x01C2B000 / 0x01C2B400; драйвер умеет soft reset и
  `TWSIEN`, но узел `i2c0` в DTS `disabled` — U-Boot шину не поднимает.
  I2C0 (PA11/PA12) настраивает уже прошивка для Sega-пада.
- SPI: 0x01C68000 / 0x01C69000; `CTL0`, `CTL1`, `CLKD`, `TCR`; узлы
  `spi0`/`spi1` в DTS `disabled`.
- PWM: 0x01C21400; prescaler и period в `CTRL`; узел `pwm` `disabled`.
- WDT: 0x01C20CA0. Драйвер собран (`WDT_SUNXI`), но
  `CONFIG_WATCHDOG_AUTOSTART` не задан — при загрузке watchdog не
  взводится; `wdt start` только вручную.
- GIC: не используется U-Boot (см. 10.1).

## 11. Что U-Boot передаёт программе

### 11.1. Вход и состояние CPU

- `go 0x40000000` вызывает `do_go_exec` (`cmd/boot.c:18-22`) — слабую
  реализацию, которая просто делает `entry(argc, argv)`. Для ARMv7-A
  `cleanup_before_linux()` **не вызывается** (он есть в `bootm`/`bootz`
  и в ARMv7-M варианте `go`).
- `r0 = argc = 1` (`argv[0]` — строка адреса), `r1 = argv`; прошивка эти
  аргументы игнорирует.
- CPSR: режим SVC32, IRQ и FIQ замаскированы (I=1, F=1) — так U-Boot
  стартует (`arch/arm/cpu/armv7/start.S`).
- **MMU включён**, D-cache и I-cache включены; `TTBR0` указывает на
  L1-таблицу U-Boot в верхней части DRAM. Адрес не фиксирован:
  `gd->arch.tlb_addr = gd->relocaddr` (`arch/arm/lib/cache.c`),
  выравнивание 64 КиБ, поэтому зависит от размера сборки и объёма DRAM.
  `VBAR` — на векторы U-Boot.
- ATAGS/DTB и размер DRAM не передаются: `go` — не `bootm`.

### 11.2. Состояние периферии на входе

| Блок | Что оставляет U-Boot |
|------|------------------------|
| DRAM | инициализирован SPL: 512 МБ, PLL_DDR 672 МГц |
| UART0 | 115200 8N1, FIFO включён, TX готов |
| MMC0 | если карта вставлена: `mmc rescan` инициализирует её, CMD17 работает; без карты — нет |
| MMC1 | узел включён; MMC-инициализация SDIO не проходит, RTL8189 не драйвится |
| USB PHY1/2 + EHCI/OHCI1/2 | такты и сбросы сняты, порты опрошены `usb start` |
| USB OTG | узел disabled — не активен |
| DE2/TCON0/HDMI | включены, если был вывод на HDMI |
| I2C0 / SPI0 / PWM | не подняты: узлы в DTS `disabled` |
| PLL_AUDIO / I2S0 | выключены, I2S в сбросе |
| DMA | тактируется и вне сброса (по снимку JTAG) |
| GIC | не тронут U-Boot |
| WDT | не взведён |
| SMTA | DMA/CCU остаются Secure |

### 11.3. Память

- U-Boot слинкован на 0x4A000000 и переносится в верх DRAM; там же его
  таблица страниц (адрес динамический, см. 11.1), malloc и кадровый буфер
  HDMI.
- Прошивка занимает 0x40000000 и фиксированные зоны (0x4A400000 — uncached,
  0x4A40C000 — L1-таблица, 0x4A500000 — DMA, 0x4A700000 — куча,
  0x4F000000 — арена меню, 0x5F800000/0x5F900000 — FB, 0x60000000 — стек).
- После `go` память U-Boot переиспользуется; возвращаться в U-Boot нельзя.

## 12. Что прошивка делает после входа

- `startup.S`: VBAR на свои векторы; копирует L1-таблицу U-Boot в
  0x4A40C000, если `TTBR0` валиден, иначе строит identity-map; ставит
  `TTBR0`, инвалидирует TLB; включает MMU, D-cache, I-cache; стек —
  0x60000000; обнуляет BSS.
- `main()`: `uart_init()` (0x01C28000), `led_init()` (PL10/PA15),
  `h3_hs_timer_init()` (0x01C20C00), `h3_de2_init()` (DE/HDMI, 1024×600,
  FB 0x5F900000), `usb_kbd_init()` → `usb_ohci_init()` для 0x01C1B400 и
  0x01C1C400, `sd_init()` (наследует MMC0 от U-Boot), `remap_load()`,
  `mmu_mark_uncached(.coherent)`, `secure_unlock()` (SMTA 0x01C23400:
  `DEC1_SET` бит 1 — DMA, `DEC2_SET` бит 3 — CCU), `i2s_init()`
  (PLL_AUDIO: запись EN|SDM|N/M, после лока читается 0x91000D0D;
  I2SPCM0_CLK 0x80030000; гейты/сбросы I2S+DMA; PA18…PA20 — I2S,
  PA10 — SD-усилитель), `gic_dist_init()` (GICD_CTLR=0, приоритеты 0xA0, target
  CPU2, включение Grp0), инициализация падов на I2C0, запуск CPU1/CPU2.
- Прошивка **не трогает**: контроллер DRAM, PLL_CPU, PLL_PERIPH0 и
  делители AHB/APB, контроллер MMC (только CMD17) — это наследуется.

## 13. Проверка на стенде

В консоли U-Boot после загрузки:

```
usb start
usb tree            # флешка/клавиатура/хабы (порты 1 и 2)
mmc list            # mmc0 — SD, mmc1 — SDIO (WiFi)
i2c bus             # шины I2C
spi list            # SPI-контроллеры
gpio status -a      # состояние GPIO/LED
dm tree             # все устройства driver model
bdinfo              # память/адреса

# снимок регистров до go (сверить с разделом 10):
md 0x01C20000 0x40  # CCU: PLL, делители, гейты
md 0x01C20800 0x40  # PIO: PA CFG/DATA
md 0x01C0F000 0x20  # MMC0
md 0x01C19400 0x10  # USB PHY ctrl
md 0x01C1B000 0x10  # EHCI1
md 0x01000000 0x20  # DE2
md 0x01EE0000 0x20  # HDMI
md 0x01C81000 0x10  # GIC distributor
md 0x01C23400 0x20  # SMTA (TrustZone)
```

Ожидаемо:

- `usb tree` показывает устройства на портах 1/2 (USB-A). OTG-порт в этом
  образе выключен (узел `usb_otg` в DTS — `disabled`).
- `i2c bus` и `spi list` пусты: узлы `i2c0`/`spi0` в DTS выключены (см. 10.2).
- `fatload usb 0 0x40000000 h3_bare.bin` читает файл с FAT32-флешки.
- `go 0x40000000` — старт прошивки (в UART появляется её лог).

## 14. Ограничения

- HDMI-консоль инициализируется при первом выводе; без монитора возможна
  пауза на инициализации HDMI — UART остаётся надёжным путём.
- WiFi и Ethernet в U-Boot не поднимаются (см. раздел 5) — это ограничение
  драйверов U-Boot, а не платы.
- I2C/SPI/PWM-драйверы собраны, но их узлы в DTS `disabled`: шины не
  поднимаются. Для использования нужна правка DTS и пересборка.
- Драйверы OTG/`ums` (`USB_MUSB_*`, `CMD_USB_MASS_STORAGE`) собраны, но узел
  `usb_otg` в DTS платы стоит `disabled` — gadget и `ums` в этом образе не
  активируются. Для включения нужна правка DTS (`&usb_otg { status = "okay"; }`
  и при необходимости `dr_mode`) и пересборка.
- При загрузке **чисто через FEL** (без U-Boot) MMC0 не инициализирован:
  `sd_init()` в прошивке наследует состояние от U-Boot, поэтому SD-карта и
  ROM-ы могут быть недоступны. Для работы с SD используйте FEL+U-Boot или
  загрузку с SD.
- SPL из `build/u-boot/` и `tools/fel/spl/spl.bin` — разные сборки
  (последний взят из Debian-пакета); для FEL используйте цельный
  `u-boot-sunxi-with-spl.bin`, чтобы SPL и U-Boot были одной версии.
