# DEVELOPER-GUIDE.md — руководство разработчика (сборка, заливка, архитектура, отладка)

> Консолидация бывших `H3-BAREMETAL-GUIDE.md` + `SOFTWARE-ARCHITECTURE.md`.
> Ревизия: 2026-10-22 (FW vW.821).

## 0. Версионирование

- Версия прошивки: `include/version.h` (`FW_VERSION_STR`), авто-инкремент при каждой
  сборке (`tools/scripts/bump_version.sh`). Формат `W.<номер>`.
- Выводится по UART при старте и на экран (нижний правый угол).

## 1. Требования к окружению

- **Тулчейн**: `arm-none-eabi-gcc` (AArch32, `-mcpu=cortex-a7 -mfpu=neon -mfloat-abi=hard`).
  Путь по умолчанию: `C:/ARM/gcc-arm-none-eabi-15.2.1/bin`.
- **make**: MSYS2 (`C:/msys64/usr/bin/make.exe`) — добавить в PATH.
- **FEL**: `tools/fel/src/sunxi-tools/sunxi-fel.exe` + `tools/fel/spl/spl.bin`
  (U-Boot SPL 2025.01 H3). Нужна UCRT-версия `libusb-1.0.dll` из
  `C:/msys64/ucrt64/bin` (если не в PATH — падает 0xC0000135).
- **JTAG**: OpenOCD `C:/msys64/ucrt64/bin/openocd.exe` + JTAGprobe (CMSIS-DAP,
  Raspberry Pi Pico), конфиг `tools/jtag/h3_pico_jtag.cfg` (IDCODE DAP 0x5ba00477).

**Важно про temp:** `C:/Users/PCB/AppData/Local/Temp/gigatool` периодически чистится
Windows. Всё нужное для проекта — в папке проекта, не в temp.

## 2. Сборка

```sh
export PATH="/c/msys64/usr/bin:/c/msys64/ucrt64/bin:$PATH"
cd E:/Giga_1/Bare_linux
make clean && make bin     # h3_bare.elf + h3_bare.bin (~113 КБ, грузится на 0x40000000)
```

Правило: после правки любого `.h` — `make clean && make bin` (начисто, не полагаться
на инкрементальную). Многопоточность — `make -j8 bin`.

## 3. Заливка

### Через FEL (SPL) — основная для разработки

```sh
FEL=tools/fel/src/sunxi-tools/sunxi-fel.exe
SPL=tools/fel/spl/spl.bin
BIN=h3_bare.bin

"$FEL" writel 0x01C20800 0x00003333   # 1) PA0-3 в JTAG (function 3), ДО spl
"$FEL" spl "$SPL"                     # 2) SPL (инит DRAM)
"$FEL" write 0x40000000 "$BIN"        # 3) записать прошивку
"$FEL" exe 0x40000000                 # 4) запустить
```

### Через SD + U-Boot

- `h3_bare.bin` на FAT-разделе; `/boot.scr` грузит на `0x40000000` и выполняет `go 0x40000000`.
- Лог: `## Starting application at 0x40000000`.

## 4. Архитектура (модули)

| Модуль | Файл | Роль |
|---|---|---|
| Старт | `src/boot.s` | векторы, PSVC, JTAG PA0-3, BSS/.coherent, стек, `kmain` |
| Главный | `src/main.c` | `kmain()`: инициализация, меню, главный цикл |
| UART | `src/uart.c` | UART0 115200 8N1, кольцевой буфер 4096 + `uart_flush()` |
| Порты | `src/ports.c` | PIO: mode/data/pull, LED PL10, `gpio_init` |
| Таймер | `src/sys.c` | HSTMR → `micros()`, `udelay()` (со сливом UART) |
| Звук | `src/i2s.c` | PLL_AUDIO, I2S0 → MAX98357A, кольцо + DMA |
| DMA | `src/h3_dma.c` | sunxi-DMA, LLI-кольцо, smart-start канала |
| MMU/кэш | `src/h3_mmu.c` | `mmu_mark_uncached()`, `dcc_clean_range()` |
| I2C | `src/h3_i2c_bb.c` | bit-bang I2C (PG8=SDA, PG9=SCL), PCF8574 | 
| Дисплей | `src/display.c` + `h3_de2.c` + `h3_lcd.c` + `h3_hdmi.c` + `dw_hdmi.c` | DE2/TCON1/HDMI |
| ГИК | `src/gic.c` | GIC distributor + CPU iface |
| Защита | `src/secure.c` | SMTA + DMA_SEC_REG (non-secure) |
| WiFi/SDIO | `src/h3_sdc.c` | SDC1, CMD52/53, RTL8189FTV: pwon/fwdl/RF/RX/TX/join/DHCP |
| WiFi слой | `src/wifi_layer.c` | запуск/connect/rescan/poll из меню (Settings→Transfer) |
| Сеть | `src/wifi_net.c` | DHCP/ARP/ICMP (L3-L4) |
| Крипто | `src/wpa_crypto.c` | SHA-1/HMAC/PBKDF2/PRF/AES-KW unwrap |
| Прошивка чипа | `src/fw_blob.S` | `.incbin "fw_nic.bin"` |
| SD-карта | `src/h3_sdc0.c` | SDC0 (0x01C0F000): подъём с нуля, CMD0..CMD58, чтение сектора PIO |
| FAT32 | `src/fat.c` | MBR→раздел с /roms, BPB, каталоги 8.3+LFN, чтение файлов |
| SMP | `src/h3_smp.c` | подъём CPU1..3 (трамплин SRAM A1, R_CPUCFG/PSCI) |
| Ретро-меню | `src/retro_menu.c` | список систем → браузер ромов /roms/<система> → запуск (заглушка); Settings/Help/About, Transfer, Sound |
| Реестр систем | `include/systems.h` | таблица систем (id/имя/папка/группа/статус/builtin) из emulation_h3_v2.xlsx |

### Точка входа и порядок `kmain()`

```c
secure_unlock();      // SMTA/DMA non-secure
h3_smp_boot(0);       // поднять CPU1..3 (трамплин SRAM A1, R_CPUCFG/PSCI)
gic_init();           // GIC dist+cpu, группы 0/1
sys_timer_init();     // HSTMR
gpio_init();          // APB0 gate порта L
uart_init();          // UART0 115200 (PA4)
set_led(1);           // PL10
display_init();       // DE2/TCON1/HDMI 1024x600 (свой FB .fb_hdmi)
i2cbb_init(PORTG,9, PORTG,8); pcf8574_init();   // кнопки
i2s_init();           // I2S0 → MAX98357A
retro_menu_run();     // главное ретро-меню (внутри wifi_layer_poll + кнопки)
// WiFi — по требованию из Settings→Transfer (wifi_layer_start), не при старте
```

### Память/линкер

Код и стек: 0x40000000..0x5FFFFFFF (512 МБ DRAM). Секции: `.vectors → .text →
.rodata → .data → .bss → .coherent (LLI, 1МБ-граница) → .dma_buf (64К звук) →
.fb_hdmi (2.4МБ)`. `.coherent`/`.dma_buf`/`.fb_hdmi` — uncached через
`mmu_mark_uncached()` (маппится по 16К шагу на чужой TTBR U-Boot).

## 5. Звук (I2S0 → MAX98357A)

Формат: 48 кГц стерео 16 бит, отсчёт в старших 16 битах 32-бит слова FIFO,
пара `[L<<16][R<<16]` (TXIM=0). Стереокадр = 8 байт. Кольцо: 4 периода × 1024
кадра = 32 КБ (85 мс при 48 кГц). DMA играет кольцо непрерывно по LLI-списку
без прерываний; позиция — из CUR_SRC канала. Все подробности и регистры —
в `HARDWARE.md §3`.

### Ключевые API

- `i2s_init()` — PLL_AUDIO (24.576М), gate/reset, пины, I2S-регистры, uncached, DMA-кольцо.
- `i2s_play_freq(freq)` — тестовый тон (синус 256 точек, целое число периодов на кольцо).
- `i2s_write(pcm, frames)` — потоковая запись (для будущего эмулятора).
- `i2s_stop()` / `i2s_mute()` — тишина + выключение усилителя (PA10).

## 6. WiFi (SDIO + RTL8189FTV)

Слои: `wifi_layer` (UI) → `h3_sdc.c` (host-контроллер + чип) → `wifi_net.c` (L3/L4)
→ `wpa_crypto.c` (WPA2). WiFi инициализируется только из пункта меню Transfer
(`wifi_layer_start`): pwon → fwdl → MAC → LCK/IQK → RX-скан (13 каналов).
Затем `wifi_layer_connect(idx)` → `h3_wifi_join_ap()` (auth/assoc/WPA2/DHCP).

После join `wifi_layer_poll()` (в главном цикле) принимает ARP/ICMP и отвечает —
пинг с ПК работает. Полный DHCP-цикл завершается ACK (корень найден W.770:
seq данных в TX-дескрипторе).

Все регистры чипа и протокол — в `HARDWARE.md §7-8`. Состояние WiFi-этапа и
открытые вопросы — в README.md и `NEXT-QUESTIONS.md`.

## 6c. Каркас эмуляторов (наработки, задел)

- **Память** (`mem.c`/`mem.h`): динамический аллокатор (first-fit) для слоя
  эмулятора. Зоны (ПРЕДВАРИТЕЛЬНЫЙ бюджет): куча (~256 МБ, от конца образа до
  ROM-зоны), ROM-зона (до 128 МБ, `mem_rom_reserve/release`), между ними —
  запас; стек растёт вниз от верха DRAM. `mem_init/alloc/free/free_bytes`.
- **Почта CPU0↔CPU1** (`mailbox.c`/`mailbox.h`): кольцо сообщений (8 слотов,
  16 байт) в uncached секции `.mailbox` (linker.ld + `mmu_mark_uncached` в
  i2s.c). `mb_send` (блокирующая, CPU0), `mb_recv` (неблокирующая, CPU1).
- **Архитектура ядер** (предварительно): CPU0 — эмулятор/меню/WiFi-трансфер
  (весь SD-доступ из одного ядра — нет гонок при записи), CPU1 — звук (DMA),
  CPU2/CPU3 — потушены (перспектива: SPI-экран/резерв). WiFi для эмуляторов
  «не существует» (слой активен только в меню).

## 6b. Ретро-меню, SD (SDC0) + FAT32

- **SD-карта** (`h3_sdc0.c`): двухфазный init — фаза A наследование U-Boot
  (только gate + CMD17, без сброса/CMD0; при загрузке с SD), фаза B полный
  подъём (FEL). **PA15 = EN_SD** (LED_EN_SD, активный уровень 0) — горит
  только на время операций с картой (токены `SD0_LED_ON/OFF` внутри
  `h3_sdc0.c`); постоянного включения в меню нет.
- **Структура SD** = как корень проекта: `roms/<система>/` в корне раздела
  (системный раздел с boot.scr не трогаем). Папки систем созданы в корне
  проекта `roms/`; на SD папки НЕ создаём автоматически (только отмечаем
  наличие рядом с пунктом меню `[OK]`).
- **FAT32** (`fat.c`): MBR → раздел с `/roms` (или первый не-system), BPB,
  каталоги 8.3+LFN, чтение файлов. `path_lookup("roms/<система>")` — поиск
  папки системы внутри `/roms`. `fat_mkdir` реализован, но НЕ вызывается
  автоматически (на будущее).
- **Ретро-меню** (`retro_menu.c`): список систем по группам (Portable /
  Consoles / Arcade / Computers / Laptops-PDAs), **сортировка по годам**
  внутри групп; описание платформы (проц/ОЗУ/частота/год) внизу экрана;
  пункт белый только при «папка на SD есть И эмулятор реализован»
  (`g_sys_has_dir[i] && g_sys_emu[i]`), иначе серый; `[B]` у builtin-систем.
  **Индикатор жизни**: мигание PL10 (~500 мс) вызывается из `udelay` (sys.c) —
  работает во всех слоях. «Insert SD» в браузере, если карты нет.
- Описания систем — ASCII (шрифт 8x8 не имеет `·`, `μ` и т.п.); годы/CPU/ОЗУ
  сверены с известными спецификациями платформ.
- **Transfer перерисовывается при получении IP** (без кнопки): в главном цикле
  меню `transfer_ip_state()` сравнивается с предыдущим — при смене (OFFER/ACK)
  ставится `dirty=1` (`retro_menu.c`).
- **PA15 (LED_EN_SD) — инверсия уровня**: LED светится при **0** на PA15;
  `SD0_LED_ON()` пишет 0 (карта используется), `SD0_LED_OFF()` — 1 (гаснет).

## 7. Диагностика

- По UART: `=== MultiTool H3 bare-metal ===`, `FW vW.xxx`, `i2s init/ok`,
  `wifi init...`, `join: ...`, `net: DHCP ...`. Подробные трассы — под флагами
  в `include/h3_sdc.h`: `WIFI_VERBOSE_LOG`, `WIFI_TX_DIAG`, `WIFI_RX_DIAG`
  (по умолчанию выключены; диагностика гейтируется целиком, не оставляет
  MMIO-обращений в рабочем конфиге).
- JTAG: `tools/jtag/` (start_ocd.ps1, telnet_probe.ps1, dump_resume.cfg);
  после снятия снимков всегда `resume`+`shutdown`, иначе плата в halt.
- Всё проверенное на стенде — в `HARDWARE.md`; эталонные значения живой Lakka —
  в `LAKKA-ETALON.md`.

## 8. Чек-листы

### Если звук снова тихий

1. `I2SPCM0_CLK` == 0x80030000 (mux 11, bit31).
2. GATING0/RST0 bit6=1 (DMA не в reset).
3. JTAG: DESC_ADDR канала == 0x40100000; FSTA наполнен (~0x6A).
4. PA10=HIGH (усилитель), питание 5 В MAX98357A.

### Если WiFi не поднимается

1. `chip_id rc=0 manf=0x024C card=0xF179`, `pwon rc=0`, `fwdl rc=0` — базовый
   SDIO-обмен в порядке. Если нет — проверить WIFI_EN (PL7), Fn1 (CCCR 0x02),
   шину (PG4/PG5, OLD mode, CMD52/53 без CRC).
2. Загрузка с SD после U-Boot: не забыть, что U-Boot гасит GIC/такты (см. HARDWARE §6).
3. `data-probe rc=0` после 50 МГц — шина жива; иначе fallback 1-bit (W.641 rollback).
4. Скан: `rxlen>0` на каналах, `BCN plen=... ssid=...` — приём работает.
5. Join: auth/assoc rc=0, MSR=STA (**0x0102 == 0x02** — при 0x01/ADHOC чип
   режет broadcast и FromDS от других STA, пинг с ПК не идёт, W.778);
   WPA2: msg1→msg4 + `cam keys loaded` (seccfg=0x10C).
6. Пинг с ПК: в логе платы `msr=0x00000002 OK`, `net: ARP reply`,
   `ICMP reply from <ПК>` — приём broadcast/unicast жив.

## 9. Эмуляторы (перспектива)

Слой эмуляторов (ретроконсоли) — отдельный, развивается параллельно сетевому.
Сетевой слой (WiFi/DHCP/WebDAV) и эмуляторный слой не зависят последовательно.
Для эмулятора звук будет потоковым через `i2s_write` (сейчас в коде есть только
тестовый тон + API кольца).