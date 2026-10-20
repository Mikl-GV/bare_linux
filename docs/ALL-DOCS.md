# ALL-DOCS: сводный документ проекта Bare_linux

> Собрано автоматически из всех .md проекта (папка docs/).
> Дата сборки: 2026-10-20.

## Оглавление

  1. [README.md](#README.md)
  2. [SOFTWARE-ARCHITECTURE.md](#SOFTWARE-ARCHITECTURE.md)
  3. [H3-BAREMETAL-GUIDE.md](#H3-BAREMETAL-GUIDE.md)
  4. [HARDWARE-REF.md](#HARDWARE-REF.md)
  5. [HARDWARE-WIFI.md](#HARDWARE-WIFI.md)
  6. [KNOWLEDGE.md](#KNOWLEDGE.md)
  7. [REGISTER-MAP.md](#REGISTER-MAP.md)
  8. [RESEARCH-MATCHAST.md](#RESEARCH-MATCHAST.md)
  9. [LAKKA-ETALON-ZAMERY.md](#LAKKA-ETALON-ZAMERY.md)
  10. [rtl8189fs-live-snapshot.md](#rtl8189fs-live-snapshot.md)
  11. [rtl8189fs-live-snapshot-big.md](#rtl8189fs-live-snapshot-big.md)
  12. [rtl8189fs-lakka-live-dump.md](#rtl8189fs-lakka-live-dump.md)
  13. [rtl8189fs-lakka-live-rx-dump-w7.md](#rtl8189fs-lakka-live-rx-dump-w7.md)
  14. [lakka-live/LAKKA-LIVE-SNAPSHOT-W537.md](#lakka-live/LAKKA-LIVE-SNAPSHOT-W537.md)
  15. [lakka-live/LAKKA-CAM-SECURITY.md](#lakka-live/LAKKA-CAM-SECURITY.md)
  16. [lakka-live/LAKKA-MACREG-FULL.txt](#lakka-live/LAKKA-MACREG-FULL.txt)
  17. [W233-SDIO-TRACE.md](#W233-SDIO-TRACE.md)
  18. [WIFI-FW-DOWNLOAD.md](#WIFI-FW-DOWNLOAD.md)
  19. [I2S-REFERENCE.md](#I2S-REFERENCE.md)
  20. [NEXT-CHAT-PROMPT.md](#NEXT-CHAT-PROMPT.md)

---


======================================================================
# ДОКУМЕНТ: README.md
======================================================================

# Bare_linux — bare-metal для Allwinner H3 (Orange Pi Lite)

Прошивка **с нуля, без ОС и без библиотек**, для одноплатника
**Orange Pi Lite** на SoC **Allwinner H3** (SUN8I 1680, 4× Cortex-A7).

Цель проекта — поднять на голем железе всю периферию платы и её модули:
сейчас работают **звук I2S0 → MAX98357A**, **HDMI-меню**, **SDIO-доступ
к WiFi-чипу RTL8189FTV (приём эфира, сканирование beacon/SSID)**. В планах —
превратить плату в эмулятор ретроконсолей с сетевым доступом к ROM на SD.

> Рабочее состояние (W.597): **полная host-ассоциация + WPA2 4-way handshake
> + передача 3-страничных кадров + DHCP-обмен** (DISCOVER→OFFER→REQUEST).
> К любой сети из скана проходит ассоциация (AuthResp=0 → AssocReq → AssocResp=0 →
> MSR=STA, rc=0). К открытым и WPA2-сетям (A05s, MTS_GPON_9290e0). Криптоядро
> host-supplicant (SHA-1/HMAC-SHA1/PBKDF2-PRF/MIC, AES-128, RFC3394 unwrap —
> проверено на хосте). Против WPA2-AP 4-way handshake доведён до конца.
> DHCP DISCOVER после handshake уходит в эфир (3-стр кадры — через HIQ),
> OFFER принимается и распознаётся (`net: DHCP OFFER ip=...`), REQUEST
> отправлен; получение ACK — в работе. IP сохраняется в global join_target.
> Шина SDIO — 1-bit/50 МГц (4-bit ломает data-фазу CMD53). host не тянет
> многоблочный SDIO read (только 1 блок/CMD53).

---

## Что уже работает (проверено на стенде)

| Блок | Статус | Подробности |
|---|---|---|
| **Звук I2S0 → MAX98357A** | ✅ | Чистый тест-тон 440/697/1209 Гц, DMA (sunxi-DMA → I2S). Работает в FEL и SD+U-Boot. |
| **HDMI-меню 1024×600** | ✅ | Полный подъём DE2/TCON1/HDMI по u-boot (без U-Boot FB), кнопки PCF8574, LED PL10. |
| **WiFi RTL8189FTV (SDIO)** | 🚧 подключение | Полный 802.11 join + **WPA2 4-way + TX 3-стр кадров** (DHCP через HIQ) + **DHCP OFFER/REQUEST обмен**. DHCP ACK — в работе. IP сохраняется в join_target. |
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
7. **JOINBSS-движок** (W.332-W.413): host передаёт Auth/AssocReq кадры
   (не H2C 0x23 — это P2P_LPS_PARAM); захват BSSID+channel из скана,
   вызов из пункта меню Transfer с Retry (3 скана). Полная ассоциация
   (rc=0, MSR=STA) к открытым и WPA2-сетям; при смене BSSID шлётся deauth
   и выполняется drain RX FIFO перед новым auth (фикс flaky rc=-4).
8. **DHCP (W.395-W.597)**: после assoc отправляется DHCP DISCOVER
   (IP proto=UDP, broadcast); приём data-кадров после assoc (ARP/IPv6)
   подтверждён. При WPA2 — пересылается после handshake (зашифрованный CCMP).
   Длина кадра с 8-байтным CCMP IV = 320 Б (3 страницы) — такие кадры идут
   через **HIQ** (W.584), 2-стр — через LOQ. OFFER принимается и распознаётся
   (`net: DHCP OFFER ip=...`, фикс портов W.590), REQUEST отправлен; получение
   **DHCP ACK — в работе**. IP сохраняется в global join_target (W.595).
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

Ближайший шаг: добиться **DHCP OFFER после WPA2 handshake** (после `cam keys
loaded` DISCOVER пересылается зашифрованным CCMP, уходит в эфир; проверить лог
W.556 с MCS7 — ждём OFFER → IP 10.72.75.x), затем пинг с ПК. Цель — «эмулятор +
сетевой доступ к ROM на SD».
Подробности: `NEXT-CHAT-PROMPT.md`, `HARDWARE-WIFI.md`.

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
- Артефакты: `h3_bare.elf`, `h3_bare.bin` — в корне проекта (bin ~88 КБ).

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
| `NEXT-CHAT-PROMPT.md` | Контекст для продолжения разработки (WiFi: ассоциация, WPA2 4-way, DHCP). |
| `HARDWARE-WIFI.md` | Справка по чипу RTL8189FTV и драйверу. |

## Git / точки восстановления

Каждая рабочая точка фиксируется коммитом (в `origin/main`) и, для ключевых
вех, тегом. Последняя точка восстановления — **W.556** (4-way handshake WPA2
завершён; TX 3-страничных кадров работает — 0x420; коммит `16e6b8e` пушнут,
правки W.553-W.556 в рабочем дереве). Лог ветки:

```
git log --oneline -15
```

Примеры ранних тегов:

```
restore-W099-fw-ok     звук + базовый каркас
w119-wifi-init         полная инициализация чипа (FW, MAC)
w139-rf-lck            RF-часть: BB-конфиг, LSSI, калибровка LCK
```

---

*Bare-metal драйверы пишутся с нуля по даташитам и эталонному Linux-коду;
проект самодостаточен — собирается на чистой машине с MSYS2 + тулчейном
arm-none-eabi.*

======================================================================
# ДОКУМЕНТ: SOFTWARE-ARCHITECTURE.md
======================================================================

# Как работает ПО MultiTool H3 (bare-metal) — архитектура и код функций

> Документ описывает работу прошивки `h3_bare.bin` для Allwinner H3 (Orange Pi Lite)
> с точки зрения кода: инициализация, главный цикл, модули, ключевые функции.
> Все фрагменты — подлинный код из `src/` (не пересказ).
> Дата: 2026-10-17. Проект: `E:\Giga_1\Bare_linux`.

---

## 1. Общая картина

Прошивка — bare-metal приложение для 4× Cortex-A7 (ARMv7), без ОС и библиотек.
Загружается U-Boot (`go 0x40000000`) или FEL+SPL на адрес 0x40000000.

Состав (по `Makefile`):

| Модуль | Файл | Роль |
|---|---|---|
| Загрузчик | `src/boot.s` | векторы, MMU-таблица, передача в kmain |
| Прошивка WiFi | `src/fw_blob.S` | `.incbin "fw_nic.bin"` — NIC-прошивка RTL8189FTV |
| Главный | `src/main.c` | `kmain()`: инициализация, меню, главный цикл |
| UART | `src/uart.c` | UART0 115200 8N1 (диагностика) |
| Порты/GPIO | `src/ports.c` | PIO настройка, LED, кнопки |
| Таймер | `src/sys.c` | High-Speed Timer → `micros()`, `udelay()` |
| I2S/звук | `src/i2s.c` | PLL_AUDIO, I2S0 → MAX98357A, кольцо+DMA |
| DMA | `src/h3_dma.c` | DMAC (sun6i), LLI-циклическое кольцо |
| MMU/кэш | `src/h3_mmu.c` | `mmu_mark_uncached()`, `dcc_clean_range()` |
| I2C bit-bang | `src/h3_i2c_bb.c` | SDA/SCL программно |
| PCF8574 | `src/pcf8574.c` | кнопки через расширитель 0x27 |
| Дисплей | `src/display.c` | переиспользует фреймбуфер U-Boot (DE2) |
| Шрифт | `src/font8x8.c` | 8×8 растровый шрифт |
| Безопасность | `src/secure.c` | `secure_unlock()` — снять TrustZone-замки |
| GIC | `src/gic.c` | маршрутизация прерываний (distributor+CPU iface) |
| **SDIO/WiFi** | **`src/h3_sdc.c`** | **контроллер MMC, CMD52/53, RTL8189FTV: power-on, FW, RF LCK/IQK, RX-тест** |

Точка входа — `kmain()` (main.c:225):

```c
void kmain(void)
{
    secure_unlock();
    gic_init();
    sys_timer_init();
    gpio_init();
    uart_init();
    uart_print("\r\n=== MultiTool H3 bare-metal ===\r\n");
    uart_print("FW v" FW_VERSION_STR "\r\n");
    set_led(1);
    /* PA0-3 = JTAG (RMW) */
    volatile uint32_t *pio_cfg0 = (volatile uint32_t *)0x01C20800;
    *pio_cfg0 = (uint32_t)((*pio_cfg0 & 0xFFFF0000u) | 0x00003333u);

    uart_print("i2c init...\r\n");
    i2cbb_init(PORTG, 9, PORTG, 8);
    pcf8574_init();
    uart_print("i2c ok\r\n");

    uart_print("i2s init...\r\n");
    if (i2s_init() != 0) uart_print("I2S FAIL\r\n");
    else                 uart_print("i2s ok\r\n");

    /* --- WiFi: SDC1 + SDIO-карта RTL8189FTV --- */
    uart_print("wifi init...\r\n");
    h3_sdc1_init();
    /* ...диагностика CCU/PIO... */
    int rc = h3_sdio_init_chip();
    if (rc == 0) {
        wifi_active = 1;
        /* CCCR, CIS ID (0x024C:F179), power-on, Fn1 enable, FW download, eFuse/MAC */
        /* ... */
        h3_wifi_rf_lck();
        h3_wifi_rf_rx_test();
    }
    uart_print("wifi ok\r\n");

    uart_print("display init...\r\n");
    display_init();
    draw_ui();
    uart_print("display ok\r\n");

    /* --- Главный цикл --- */
    for (;;) {
        led_heartbeat();
        uint32_t btns = pcf8574_read();
        if (btns != prev_buttons) handle_buttons(btns);
        udelay(10000);
    }
}
```

---

## 2. Периферия нижнего уровня

### 2.1 UART (src/uart.c)

UART0 на 115200 8N1. U-Boot уже задал такт/скорость; код лишь включает такт,
снимает reset и настраивает PA4=TX (F2):

```c
void uart_init(void)
{
    set_pin_mode(PORTA, 4, 2);              /* PA4 = UART0 TX */
    H3_CCU_GATING3 |= (1u << 16);           /* такт UART0 */
    H3_CCU_RST4    |= (1u << 16);           /* снять reset */
    volatile uint32_t n = 1000;
    while (n--) {}
}

void uart_putc(char c)
{
    uint32_t t = 0;
    while (!uart_tx_ready()) {
        if (++t > 100000) return;           /* защита от вечного цикла */
    }
    UART0_THR = (uint32_t)(unsigned char)c;
}

void uart_print(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
```

`uart_print_hex()` печатает `000ABCDE`-стиль 8 hex-цифр; `uart_print_int()` — десятичное.

### 2.2 Порты/GPIO (src/ports.c)

Обёртка над PIO-регистрами (data/cfg/pull):

```c
void set_pin_mode(uint32_t port, uint32_t pin, uint32_t mode)
{
    volatile uint32_t *cfg = cfg_reg(port, pin);
    uint32_t shift = (pin % 8) * 4;
    uint32_t v = *cfg;
    v &= ~(0xFUL << shift);
    v |= (mode & 0xF) << shift;
    *cfg = v;
}

void set_pin_data(uint32_t port, uint32_t pin, uint32_t data)
{
    volatile uint32_t *ddr = (volatile uint32_t *)(port + 0x10);
    if (data) *ddr |= (1u << pin);
    else      *ddr &= ~(1u << pin);
}

void gpio_init(void)
{
    H3_APB0_CLK_GATING |= (1u << 0);        /* такт порта L (always-on) */
    __asm volatile("dsb" ::: "memory");
}

void set_led(int on)                        /* PL10 — пользовательский LED */
{
    set_pin_mode(PORTL, 10, 1);
    set_pin_data(PORTL, 10, on ? 1 : 0);
}
```

### 2.3 Таймер (src/sys.c)

High-Speed Timer 0x01C60000, 64-бит счётчик вниз, реально ~96.8 МГц (берём 97):

```c
uint32_t micros(void)
{
    return (~HSTMR_CURNT_LO) / HSTMR_MHZ;   /* 97 */
}

void udelay(uint32_t us)
{
    uint32_t cur = HSTMR_CURNT_LO;
    uint32_t ticks = us * HSTMR_MHZ;
    while ((uint32_t)(cur - HSTMR_CURNT_LO) < ticks) { }
}
```

`sys_timer_init()` делает полный reset HSTMR (RST0 bit19), включает gate и counter.

### 2.4 MMU/кэш (src/h3_mmu.c)

H3 не кэш-когерентен: DMAC читает физическую память в обход L1/L2. Память
буферов помечается uncached, а перед стартом DMA кэш чистят:

```c
void mmu_mark_uncached(uint32_t addr)
{
    uint32_t l1_base, ref, idx;
    __asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(l1_base));
    l1_base &= ~0x3FFFu;
    idx = (addr >> 20) & 0xFFF;
    ref = ((volatile uint32_t *)l1_base)[idx];
    switch (ref & 3u) {
    case 0b10u: {   /* Section (1MB) */
        uint32_t ne = (addr & 0xFFF00000u) | (ref & 0x0008DDF0u)
                    | (1u << 16) | (0u << 3) | (0u << 2) | 0b10u;
        ((volatile uint32_t *)l1_base)[idx] = ne;
        dcc_clean_line(l1_base + idx * 4);
        break;
    }
    case 0b11u: {   /* L2 (large page 16КБ) */
        uint32_t l2_base = ref & 0xFFFFFC00u;
        uint32_t n = (addr >> 14) & 0x3FFu;
        volatile uint32_t *l2 = (volatile uint32_t *)l2_base;
        uint32_t e0 = l2[n * 2 + 0];
        if ((e0 & 3u) == 0b01u) {
            e0 &= ~((1u << 3) | (1u << 2));   /* C=0,B=0 */
            l2[n * 2 + 0] = e0;
            dcc_clean_line(l2_base + n * 8);
        }
        break;
    }
    default: break;
    }
    __asm volatile("dsb" ::: "memory");
    __asm volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));  /* TLBIALL */
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

void dcc_clean_range(uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~0x1Fu;
    uint32_t end = (addr + len + 0x1Fu) & ~0x1Fu;
    for (uint32_t a = start; a < end; a += 32)
        dcc_clean_line(a);                  /* DCCMVAC */
    __asm volatile("dsb" ::: "memory");
}
```

---

## 3. Звук: I2S0 → MAX98357A (src/i2s.c, src/h3_dma.c)

Формат: 48 кГц стерео 16 бит; отсчёт в СТАРШИХ 16 битах 32-битного слова FIFO,
пара `[L<<16][R<<16]` (TXIM=0, SW=32, SR=16, MODE=Left+offset). Стереокадр = 8 байт.

Кольцо: 4 периода × 1024 кадра = 32 КБ (85 мс при 48 кГц). Вывод потоковый:
приложение пишет PCM через `i2s_write()`, DMAC крутит кольцо по LLI-списку,
позиция чтения — из `CUR_SRC` DMA-канала (без прерываний).

### 3.1 PLL_AUDIO (i2s.c:139)

```c
static int pll_audio_program(unsigned fs)
{
    uint32_t pat; unsigned n, m; int i;
    if (fs == 48000u)      { pat = 0xC000AC02u; n = 14; m = 14; } /* 24.576 МГц */
    else if (fs == 44100u) { pat = 0xC0010D84u; n = 7;  m = 8;  } /* 22.5792 МГц */
    else return -1;

    H3_CCU_PLL_AUDIO_PAT  = pat;
    H3_CCU_PLL_AUDIO_CTRL = (1u << 31) | (1u << 24)
                          | ((uint32_t)(n - 1) << 8) | (uint32_t)(m - 1);
    for (i = 0; i < 100; i++) {
        if (H3_CCU_PLL_AUDIO_CTRL & (1u << 28)) return 0;   /* LOCK */
        udelay(100);
    }
    return -1;
}
```

### 3.2 Кольцо и DMA (i2s.c + h3_dma.c)

```c
/* i2s.c — позиция проигрывания по CUR_SRC DMA-канала */
static void poll_played(void)
{
    uint32_t pos = h3_dma_audio_pos_bytes() / BYTES_PER_FRAME;
    if (pos >= RING_FRAMES) pos = RING_FRAMES - 1u;
    if (pos < g_last_pos)
        g_played += (uint64_t)(RING_FRAMES - g_last_pos) + pos;
    else
        g_played += (uint64_t)(pos - g_last_pos);
    g_last_pos = pos;
}
```

```c
/* h3_dma.c — конфигурация LLI: RAM(linear,32b,burst8) -> I2S0_TX_FIFO */
uint32_t cfg = CFG_SRC_DRQ(DRQ_SDRAM) | CFG_SRC_MODE_LIN
             | CFG_SRC_BURST(BURST_8) | CFG_SRC_WIDTH(2)
             | CFG_DST_DRQ(DRQ_I2S0_TX) | CFG_DST_MODE_IO
             | CFG_DST_BURST(BURST_8) | CFG_DST_WIDTH(2);
uint32_t period_bytes = buf_bytes / (uint32_t)periods;
for (int i = 0; i < periods; i++) {
    struct dma_lli *l = &g_lli[i];
    l->cfg  = cfg;
    l->src  = buf_phys + (uint32_t)i * period_bytes;
    l->dst  = H3_I2S0_TX_FIFO;
    l->len  = period_bytes;
    l->para = NORMAL_WAIT;
    l->p_next = (uint32_t)(uintptr_t)&g_lli[(i + 1) % periods];  /* циклически */
}
dcc_clean_range((uint32_t)(uintptr_t)&g_lli[0], /* LLI вне кэша для DMAC */
                (uint32_t)(uintptr_t)&g_lli[periods] -
                (uint32_t)(uintptr_t)&g_lli[0]);
```

Тестовый тон — таблица синуса 256 точек (int16):

```c
static inline int32_t sin_t(uint32_t phase) { return (int32_t)sin_tab[(phase >> 24) & 0xFF]; }
```

---

## 4. SDIO и WiFi-чип RTL8189FTV (src/h3_sdc.c)

Хост-сторона — 1:1 с эталонным драйвером rtl8189fs (v4.3.24_15589). Чип
подключён к SDC1 (PG0-PG5). Шина стенда — 1-bit/50 МГц (4-bit ломает CMD53).

### 4.1 Инициализация контроллера MMC (h3_sdc1_init, h3_sdc.c:120)

1. `wifi_power_on()` — питание чипа (WIFI_EN=PL7);
2. CCU: gate MMC1 + полный цикл reset (assert→deassert);
3. модульный такт MMC1: OSC24M, N=2, M=16 (≈375 кГц для SD-init);
4. пины PG0..PG5 = function 2 (SDC1);
5. reset контроллера (GCTL_RESET), HWRST active→reset→active, 1 мс;
6. CKC: CCLK_EN + делитель (375 кГц), SD_FUNS=0xCEAA, ширина 1-bit, блок 512.

### 4.2 CMD52/CMD53 (PIO, без IDMA)

```c
int h3_sdc_cmd52_read(uint32_t func, uint32_t reg, uint8_t *out)
{
    uint32_t arg = (0u << 31) | ((func & 7u) << 28) | ((reg & 0x1FFFF) << 9);
    int rc = h3_sdc_cmd_no_data_flags(52, arg, 0, 1, 0);
    if (rc != 0) return rc;
    *out = (uint8_t)(SDC_REG(SDC_RESP0) & 0xFF);
    return 0;
}

int h3_sdc_cmd53_write_bytes(uint32_t func, uint32_t addr, uint32_t len, const void *data)
{
    uint32_t arg = (1u << 31) | ((func & 7u) << 28)
                 | (0u << 27)               /* byte mode */
                 | (1u << 26)               /* inc */
                 | ((addr & 0x1FFFF) << 9)
                 | (len & 0x1FF);
    return h3_sdc_block_transfer(53, arg, len, 1, 1, (void *)data);
}
```

Обмен данными — **PIO** через FIFO-регистр контроллера:

```c
/* h3_sdc_block_transfer (h3_sdc.c:2837) — запись */
SDC_REG(SDC_GCTL) |= (1u << 31);                    /* FIFO_AC_MODE_AHB */
cmdr_val = SDC_CMDR_START | (cmd & SDC_CMDR_IDX_MASK)
         | SDC_CMDR_RESP_RCV | SDC_CMDR_DATA_TRANS
         | SDC_CMDR_TRANS_DIR | SDC_CMDR_WAIT_PRE;
SDC_REG(SDC_CMDR) = cmdr_val;
for (i = 0; i < words; i++) {
    sdc_wait(..., SDC_STAR_FIFO_FULL, 0);
    fifo[0] = in[i];                                /* 1 слово за раз */
}
sdc_wait(..., SDC_STAR_FIFO_EMPTY, 1);              /* дождаться отправки */
```

Столько-big диагностика: `sdc_diag_*` (cmdr/risr/sta/idst/dmac) фиксируются при
ошибках и печатаются в main.c.

### 4.3 Загрузка NIC-прошивки (h3_wifi_download_fw + fw_write_image)

Протокол — `rtl8188f_FirmwareDownload`:

1. `_FWDownloadEnable(1)`: 0x03|=0x04 (8051 enable), 0x80|=0x01 (FWDL),
   0x82&=0xf7 (reset 8051);
2. запись прошивки **постранично** (4096 б), page-select `MCUFWDL+2[2:0]`,
   байт-режим CMD53, старт `(8<<13)|0x1000`, хвост без добивки нулями;
3. полиинг `FWDL_ChkSum_rpt`;
4. `_FWDownloadEnable(0)`: снять FWDL;
5. `_FWFreeToGo`: 0x80|=MCUFWDL_RDY, &=~WINTINI_RDY, полный `_8051Reset8188`
   (0x1D bit0 снять→взвести, 0x03 bit2 снять→взвести), полиинг
   `WINTINI_RDY|ChkSum_rpt|MCUFWDL_RDY|RAM_DL_SEL` → штатно MCUFWDL=0xC6.

```c
static int fw_write_image(const uint8_t *b, uint32_t size)
{
    uint8_t t;
    WREG8(REG_MCUFWDL_OFF, (t | FWDL_ChkSum_rpt));      /* сброс chksum до записи */
    uint32_t page = 0, off = 0;
    while (off < size) {
        uint32_t page_end = off + 4096; if (page_end > size) page_end = size;
        t = (t & 0xF8) | (page & 0x07);                 /* page-select */
        WREG8(REG_MCUFWDL_OFF + 2, t);
        uint32_t ftaddr = (8u << 13) | (FW_8188F_START & 0x7FFFu);
        for (uint32_t i = 0; i < n4; i++)               /* слова по 4 б */
            h3_sdc_cmd53_write_bytes(1, ftaddr + i * 4, 4, b + off + i * 4);
        if (r4) h3_sdc_cmd53_write_bytes(1, ftaddr + n4 * 4, r4, b + off + n4 * 4);
        off = page_end; page++;
    }
    for (w = 0; w < 500; w++) {                         /* полиинг chksum */
        if (RREG8(REG_MCUFWDL_OFF, &t)) return -35;
        if (t & FWDL_ChkSum_rpt) return 0;
        udelay(100);
    }
    return -38;
}
```

Прошивка встроена в образ через fw_blob.S (`.incbin "fw_nic.bin"`), 18802 байта,
идентична `Array_MP_8188F_FW_NIC` из драйвера (SHA256 C016CF54...).

### 4.4 RF-доступ (LSSI, rf_write/rf_read, h3_sdc.c:847)

```c
static void rf_write(uint8_t off, uint32_t data)
{
    h3_sdc_write_wlan_reg32(0x840, (((uint32_t)off & 0xFF) << 20) | (data & 0xFFFFF));
}

static uint32_t rf_read(uint8_t off)
{
    uint32_t t2 = 0, p1 = 0, rb = 0;
    h3_sdc_read_wlan_reg32(0x824, &t2);
    t2 = (t2 & ~0x7F800000u) | (((uint32_t)off & 0xFF) << 23);
    h3_sdc_write_wlan_reg32(0x824, t2 & ~0x80000000u);
    h3_sdc_read_wlan_reg32(0x824, &t2);
    h3_sdc_write_wlan_reg32(0x824, t2 & ~0x80000000u);
    h3_sdc_write_wlan_reg32(0x824, t2 | 0x80000000u);   /* edge-триггер чтения */
    udelay(100);
    h3_sdc_read_wlan_reg32(0x820, &p1);
    if (p1 & (1u << 8))
        h3_sdc_read_wlan_reg32(0x8B8, &rb);             /* PI */
    else
        h3_sdc_read_wlan_reg32(0x8A0, &rb);             /* LSSI */
    return rb & 0xFFFFF;
}
```

### 4.5 LCK-калибровка (h3_wifi_rf_lck, h3_sdc.c:891)

Последовательность (phy_LCCalibrate_8188F):
0) BB-конфиг (209) + AGC (66) + RF-таблица (136, **с пропуском записи 0x18**),
   затем RF 0x18=0x401; 1) TXPAUSE=0xFF; 2) RF 0xB0=0xDFBF0 (LDO ON);
3) RF 0x00=0; 4) RF 0x18 |= 0x8000 (LCK-триггер); 5) полиинг сброса bit15
(1 с); 6) RF 0xB0=0xDFFF0; 7) RF 0x18=0x401; 8) RF 0x00=0x33D95;
9) TXPAUSE=0. Итог: `lck: done in 70 ms`.

### 4.6 IQK-калибровка (h3_wifi_rf_iqk, h3_sdc.c:1155)

phy_IQCalibrate_8188F Path A 1T1R:
- H2C 0x6D={1} (BT_WLAN_CALIBRATION), PI-режим: 0x860/0x868=0x01000100;
- MAC-настройки: TXPAUSE=0x3F, BCN_CTRL &=~BIT3, GPIO 0x40 &=~BIT5;
- Tx one-shot (0xE48=0xF9000000→0xF8000000, 25 мс) — сохранить LOK_Result из
  RF 0x08 (**W.265**); проверка e94/e9c/eac (Tx OK);
- Rx two-pass (RF_TXPA_G2, refine), после каждого — восстановить RF 0x08=LOK_Result;
- выход: 0xE28=0, тоны 0x01008C00, SI-режим 0x860/0x868=0x01000000,
  restore Path_SEL (BB 0x948, RF 0xB0), restore BB (0x800/0xC04/0xC08/0x874),
  TXPAUSE=0, H2C 0x6D={0}.

```c
/* сохранение LOK_Result после Tx one-shot (W.265-D8) */
lok_result = rf_read(0x08);
uart_print("  iqk: lok08=0x"); uart_print_hex(lok_result); uart_print("\r\n");
/* ... в Rx-проходах: */
rf_write(0x08, lok_result);
```

Итог на стенде: Tx OK / Rx OK, `lok08=0x8F800` (живой чип 0xF800).

### 4.7 RX-конфигурация и свип (h3_wifi_rf_rx_test, h3_sdc.c:1670)

1. пробуждение FW: RPWM(0x80)=0x00, полиинг CPWM(0x38)=0x00; прерывания FBR/
   CCCR; H2C SET_PWR_MODE (box0 0x1D0 + EX 0x1F0);
2. RF_CHNLBW=0x401 (канал 1, 20 МГц), RFMOD |= BIT24|BIT25 (bCCKEn|bOFDMEn);
3. RQPN (0x200=0xE7020C)+NPQ(0x214=0x02), LLT (0x224|=BIT16), rx_bndy
   (0x116=0x3F7F), BWOPMODE=0x04, SIFS/RL/EDCA;
4. RCR=0x7000604E, RRSR=0x1F5F, RXFLTMAP, MAR; REG_CR |= MACTXEN|MACRXEN
   → 0x0206FF;
5. antenna_sel: LEDCFG2 |= BIT7 (W.264);
6. свип RF 0x18 = 0xC01/0xC06/0xC0B (каналы 1/6/11, BW20), по 3 probe request
   (`h3_wifi_tx_probe_req`), слушание 150 мс, диагностика rxlen/rxpkt/hisr/cca.

### 4.8 Probe request (h3_wifi_tx_probe_req, h3_sdc.c:3111)

```c
/* tx_desc 40 байт: OFFSET=40, BMC|FIRST|LAST|OWN, QUEUE_SEL=QSLT_MGNT=0x12,
   USE_RATE=1, TX_RATE=1M, RETRY_LIMIT=12 */
buf[2] = (uint8_t)TXDESC_SIZE_W220;
buf[3] = (uint8_t)((1u << 0) | (1u << 2) | (1u << 3) | (1u << 7));
buf[5] = (uint8_t)(0x12u << 0);
buf[13] = (uint8_t)(1u << 0);
buf[18] = (uint8_t)((1u << 1) | (12u << 2));
/* кадр: probe request, bcast, наш MAC из 0x610-0x615, IE: SSID, Rates, DS */
uint32_t total = (40 + plen + 3) & ~3u;
uint32_t fifo_addr = (WLAN_TX_HIQ_DEVICE_ID_W220 << 13) | ((total / 4) & 0x1FFFu);
return h3_sdc_cmd53_write_bytes(1, fifo_addr, total, buf);
```

Перед записью — проверка свободных страниц HIQ (`h3_wifi_tx_free_pages_hiq`),
после записи драйвер НЕ делает триггер — FW сама стартует передачу.

---

## 5. Дисплей и меню (src/display.c, src/main.c)

Дисплей переиспользует фреймбуфер U-Boot (DE2 UI-канал), не трогая DE/HDMI/TCON:

```c
int display_init(void)
{
    uint32_t laddr = UI_TOP_LADDR;      /* 0x01103010 */
    uint32_t pitch = UI_PITCH;          /* 0x0110300C */
    if (laddr >= 0x40000000u && laddr < 0x60000000u) {
        fb = (volatile uint32_t *)laddr;
        fb_pitch = (pitch >= FB_STRIDE && pitch <= 8192) ? pitch : FB_STRIDE;
        mmu_mark_uncached(laddr);       /* DE читает физическую память */
        fb_clean();
    } else {
        extern unsigned char _fb_hdmi_start[];
        fb = (volatile uint32_t *)(uintptr_t)_fb_hdmi_start;
        fb_pitch = FB_STRIDE;
    }
    return 0;
}
```

Меню — конечный автомат (SCR_MAIN / SCR_TONE / SCR_SETTINGS). Рендер рисует
прямоугольники, текст по центру, маркер выбора; кнопки — PCF8574 через I2C
bit-bang. Heartbeat — `led_heartbeat()` (каждые 500 мс переключает PL10).

---

## 6. Управление кнопками (src/pcf8574.c + h3_i2c_bb.c)

I2C bit-bang: SCL=PG9, SDA=PG8, PCF8574 @0x27. Детект нажатий — по фронту
`btns & ~prev_buttons` (main.c:173-175).

---

## 7. Поток выполнения WiFi-этапа (сводно, актуально на W.316)

```
SDIO CMD5→3→7 → CCCR → chip_id 0x024C:F179 → PowerSeq CARDEMU→ACTIVE
 → Fn1 enable → FW download (5 страниц, chksum OK, MCUFWDL=0xC6)
 → eFuse/MAC (программный 02:00:00:xx) → REG_MACID verify
 → LCD: MAC(101)+BB(207)+AGC(66)+RF(136) → LCK
 → RX: FW active (RPWM/CPWM), H2C SET_PWR_MODE, IGI/MSR, FBR/CCCR,
   RFMOD, RQPN/LLT/bndy, antenna_sel, BWOPMODE, CR=0x206FF
 → IQK: PI, Tx/Rx one-shot, LOK_Result, restore
 → RX-sweep ch 1..13, probe request, чтение RX FIFO по 1 блоку
   → парсер beacon → SSID (W.316: BCN plen= ch= ssid=)
 → C2H-ping (H2C 0x43) → C2H-ABSENT (см. NEXT-CHAT-PROMPT)
```

**Текущий статус (W.338):** хост-инициализация 1:1 с драйвером, **RX-приём
работает** — чип принимает beacon'ы на всех 13 каналах, парсер извлекает SSID.
Хост-чтение RX-FIFO по 1 блоку (512 б)/CMD53 (host H3 не тянет многоблок).
RPWM/CPWM/HIMR/HISR пишутся через **fn1** (SDIO-local), не fn0 — с W.329 FW
подтверждает переход в ACTIVE по смене toggle (cpwm=0x80, tog_sw=1), HIMR=0x01.
Эксперимент W.337/W.338: регистр HRPWM1 принимает полный 0xCC (rb=0xCC), но
CPWM state[3:0] остаётся 0 — на этой связке FW репортит ACK только через
toggle; маска 0xC1 у драйвера не баг, C2H-ABSENT **не связана** с RPWM.
Реализован JOINBSS-движок (host передаёт Auth/AssocReq кадры, не H2C 0x23;
захват BSSID+channel из скана; вызов из wifi_layer с Retry). Осталось:
обработка Auth/AssocResp из RX-пути для подтверждения ассоциации, затем
фикс IP (192.168.x.199) к цели «эмулятор + сетевой доступ к ROM на SD».
Дальнейшие шаги — в `NEXT-CHAT-PROMPT.md`.

======================================================================
# ДОКУМЕНТ: H3-BAREMETAL-GUIDE.md
======================================================================

# MultiTool Bare-Metal — руководство по сборке, заливке и отладке (Allwinner H3)

> Оранж Пи Лайт (Orange Pi Lite), SoC Allwinner H3 (SUN8I 1680, Cortex-A7).

> **Версия:** 1.7
> **Дата:** 2026-10-19
> **Изменения (1.7):** WiFi-этап: **ассоциация + полный 4-way handshake WPA2**
> (W.369-W.459) — полный join (AuthResp=0→AssocResp=0→MSR=STA, rc=0) к открытым
> и WPA2-сетям; криптоядро host-supplicant (SHA-1/HMAC/PBKDF2/PRF, AES-128,
> RFC3394) и доведённый до конца 4-way (msg2 принят AP→msg3 GTK unwrap→msg4
> secure→PTK/GTK в CAM). Ранее (1.5): полная инициализация RTL8189FTV — PowerSeq,
> fwdl rc=0, eFuse пуст, программный MAC в REG_MACID.

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
| 1.6 | 2026-10-18 | WiFi-этап: **RX-приём заработал** (W.316). Чип принимает beacon'ы эфира, парсер извлекает SSID. Причина прошлых «мёртвых» сессий: host SDIO H3 не тянет многоблочный read (30 блоков → rc=-1) и сдвиг смещения кадра в парсере (off+28 вместо off+24). |
| 1.7 | 2026-10-19 | WiFi-этап (W.369-W.459): **ассоциация + полный 4-way handshake WPA2**. Полный join (AuthResp=0 → AssocResp=0 → MSR=STA, rc=0) к открытым и WPA2-сетям; криптоядро host-supplicant (SHA-1/HMAC/PBKDF2/PRF, AES-128, RFC3394 unwrap); 4-way доведён до конца (msg1→msg2 принят AP→msg3 GTK unwrap→msg4 secure→PTK/GTK в CAM). DHCP OFFER на WPA2-сети — в работе. |
| 1.7 | 2026-10-18 | **Дисплей выведен с нуля** (W.328): полный порт DE2/TCON1/HDMI по u-boot, картинка на стенде без FB U-Boot. display_init в начале kmain. Меню: Sound test / Transfer (WiFi) / Settings; WiFi вынесен в отдельный слой `wifi_layer.c`, вызывается из пункта Transfer. |
| 1.8 | 2026-10-18 | WiFi-этап (W.329-W.338): **RPWM/CPWM/HIMR/HISR через fn1** (SDIO-local, не fn0) — скан на всех 13 каналах, декодирование beacon/SSID стабильно. Эксперимент W.338: запись 0xCC в HRPWM1 даёт rb=0xCC, но CPWM state[3:0]=0 — ACK идёт только через toggle, маска 0xC1 у драйвера не баг. Реализован JOINBSS-движок (host Auth/AssocReq кадры, захват BSSID+channel из скана, Retry). Дальше — обработка Auth/AssocResp из RX-пути для подтверждения ассоциации (цель «эмулятор + сетевой доступ к ROM на SD», фикс IP 192.168.x.199). |

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
  устанавливается, калибровка **LCK** и **IQK** выполнены. **RX-приём работает
  (W.338, 13 каналов)**: чип принимает beacon'ы эфира, парсер извлекает SSID
  (`BCN plen= ch= ssid=`). RPWM/CPWM/HIMR/HISR через fn1; реализован
  JOINBSS-движок (host Auth/AssocReq кадры, захват BSSID+channel из скана).
  Хост-чтение RX-FIFO — по 1 блоку (512 б)/CMD53 (host H3 не тянет многоблок).
  **Ассоциация (rc=0, MSR=STA)** и **полный 4-way handshake WPA2** (msg1→msg4,
  PTK/GTK в CAM) работают; стадия DHCP OFFER → IP — в работе.

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
- Размер бинаря ~88 КБ.

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

======================================================================
# ДОКУМЕНТ: HARDWARE-REF.md
======================================================================

# Аппаратная справка (H3 / Orange Pi Lite) — проверено на стенде

> Источник: `E:\Giga_1\pico-retro-new-gen` (референс, только чтение) + H3 Datasheet.
> Значения помечены как проверенные на стенде; всё остальное — из даташита.
> Это справочник для написания чистого кода, НЕ копируемый модуль.

## Базы периферии

| Блок | Адрес | Примечание |
|---|---|---|
| CCU | 0x01C20000 | такт/сбросы |
| PIO (GPIO) | 0x01C20800 | порты A..H |
| R_PIO | 0x01F02C00 | always-on, порт L (WIFI_EN=PL7) |
| PRCM | 0x01F01400 | always-on RCU (R_REG) |
| UART0 | 0x01C28000 | PA4=TX PA5=RX, 115200 |
| DMAC | 0x01C02000 | sunxi-DMA, канал +0x100+N*0x40 |
| I2S0 | 0x01C22000 | звук MAX98357A |
| GIC dist | 0x01C81000 | |
| GIC cpu | 0x01C82000 | banked, каждый CPU видит свои |
| USB OHCI1 | 0x01C1B400 | |
| USB OHCI2 | 0x01C1C400 | |
| MMC0 (SD) | 0x01C0F000 | SD-карта |
| MMC1 (SDC1) | 0x01C10000 | WiFi-чип RTL8189FTV (SDIO, PG0-PG5) |
| R_CPUCFG | 0x01F01C00 | запуск вторичных ядер |
| SRAM A1 | 0x00000000 | трамплин CPU1..3 |

## Память (карта)

| Адрес | Назначение |
|---|---|
| 0x40000000 | код (U-Boot `go`) |
| 0x50000000 | буфер загрузки ROM с SD |
| 0x5F800000 | emu-кадровый буфер 320x240 RGB565 |
| 0x5F900000 | HDMI-кадровый буфер 1024x600 XRGB8888 |
| ~0x60000000 | стек (растёт вниз от верха DRAM) |

## Звук (I2S0 → MAX98357A), проверено на стенде

Пины: PA18=SYNC(LRC), PA19=CLK(BCLK), PA20=DOUT(DIN) — функция 2 в CFG2.
PA10=SD усилителя (output, HIGH=работа).

- PLL_AUDIO 24.576 МГц: `PLL_AUDIO_CTRL=(1<<31)|(1<<24)|((14-1)<<8)|(14-1)`; `PLL_AUDIO_PAT=0xC000AC02`; ждать lock bit28.
- CCU: `BUS_CLK_GATING2 |= (1<<12)` (I2S0), reset `CCU_BASE+0x2D0 |= (1<<12)`, `I2SPCM0_CLK=(0x0B0)` биты [17:16]=3 (mux pll-audio-8x), бит31=1.
- I2S0 регистры: CTRL 0x00 (BCLK_OUT=bit18, LRCK_OUT=bit17, TX_EN=bit2, SDO_EN0=bit8, GL_EN=bit0), FMT0 0x04 (SR=3 16bit<<4, SW=7 32слот<<0, LRCKPER=32 через (32-1)<<8), CLK_DIV 0x24 (MCLK_EN=bit8, BCLK_DIV=5<<4, MCLK_DIV=2<<0), CHAN_CFG 0x30 (TXSLOT=2), TX_CSEL 0x34, TX_CMAP 0x44=0x76543210, FIFO_TX 0x20 (пары: L<<16|...см. ниже), FIFO_CTL 0x14 (TXTL=0x40<<12), INT 0x1C (TX_DRQ=bit7).
- Формат DMA-буфера: 32-бит слова, сэмпл в СТАРШИХ битах. Пара = 2 слова `[L<<16][R<<16]`.
- Скорости: LRCK=48 кГц; BCLK=3.072 МГц (PLL/8); MCLK=PLL/2.

## DMA (sunxi-DMA → I2S TX), проверено на стенде

- База 0x01C02000. Канал N: `+0x100 + N*0x40`:
  EN 0x00, PAU 0x04, LLI 0x08, CFG 0x0C(RO), CUR_SRC 0x10(RO), PARA 0x1C.
  DMA_STA 0x30 (bitN=канал busy), DMA_IRQ_EN 0x00, DMA_IRQ_PEND 0x10, DMA_AUTO_GATE 0x28 (=0x4), DMA_PKG_NUM 0x130.
- LLI-дескриптор: 6 слов — cfg,src,dst,len,para,next. 2 циклических; `next` последнего → первый.
- CFG-поля H3: SRC_WIDTH[10:9], SRC_BURST[7:6], SRC_DRQ[4:0], SRC_MODE[5], DST_WIDTH[26:25], DST_BURST[23:22], DST_DRQ[20:16], DST_MODE[21].
- DRQ: SDRAM=1, I2S0_TX=3=0x01C22020.
- Ширина 32бита=2, burst4=1, burst1=0; IO-mode=1 для DST.
- Критично:
  - Полный сброс DMAC перед настройкой (BUS_SOFT_RESET0 bit6: and-then-or).
  - `DMA_IRQ_PEND=0xFFFFFFFF` перед стартом канала (иначе канал замирает).
  - LLI обязан лежать в uncached-памяти (иначе stale кэш).
- INTID аудио-DMA = **82** (= 32+SPI50) — НЕ 32+82! OPHR = SPI 50.
- PKG-прерывание: `DMA_IRQ_EN |= (1<<1) << (ch*4)`.

## USB

OHCI1/OHCI2. Для клавиатуры: USB HID boot protocol. USB Audio (внешний ЦАП) — класс UAC.

## Запуск вторичных ядер (H3, проверено)

- R_CPUCFG 0x01F01C00: RST_CTRL(cpu)=+0x40+cpu*0x40, CTRL=+0x44, STATUS=+0x48.
- Трамплин в SRAM A1 (0x00000000): 5 слов — sp, entry, bx. После записи flush c7,c10,1 + dsb.
- Запуск: RST_CTRL=0 (удержать) → udelay → RST_CTRL=3 (release). Ждать magic в SRAM.
- В non-secure (PSCI u-boot): через SMC PSCI_CPU_ON 0x84000003.

## JTAG H3 (проверено по даташиту H3 Table 3-2)

> Важно: это ПРАВИЛЬНАЯ распиновка из даташита. В интернете встречается
> неверная инфа (PA21=JTAG_TCK) — PA21 НЕ JTAG (это PCM0_DIN). Не путать.

| Пин H3 | Function (Alt) | JTAG-сигнал |
|---|---|---|
| PA0 | Function 3 | JTAG_MS (TMS) |
| PA1 | Function 3 | JTAG_CK (TCK) |
| PA2 | Function 3 | JTAG_DO (TDO) |
| PA3 | Function 3 | JTAG_DI (TDI) |

- Все 4 пина — в CFG0 порта A (PIO 0x01C20800), Function 3 (значение 0x3 на пин).
- Коммутация: `CFG0 = (CFG0 & 0xFFFF0000) | 0x00003333`.
- H3 НЕ включает JTAG сам по себе — пины надо переключить кодом (в boot.s)
  до подключения отладчика.
- Отладчик: JTAGprobe (Raspberry Pi Pico, CMSIS-DAP). Прошивка — в `tools/jtag/`.
  Конфиг OpenOCD — `tools/jtag/h3_pico_jtag.cfg`, IDCODE DAP 0x5ba00477.
- Полная проверенная инструкция — `tools/jtag/JTAG-SETUP.md`.

## WiFi (RTL8189FTV на SDC1), проверено на стенде

Чип RTL8189FTV (Realtek, SDIO) подключён к контроллеру SDC1 (MMC1) H3.

### Питание и пины

- **WIFI_EN = PL7** (R_PIO data 0x01F02C10, бит 7, активный high). Чип питается от
  3.3 В постоянно; энубл-сигнал поднимаем на старте (power-cycle PL7).
- **SDC1 на PG0-PG5** (function 2): PG0=CLK, PG1=CMD, PG2=D0, PG3=D1, PG4=D2, PG5=D3.
  Настройка: `PORTG CFG0 [15:0] = 0x2222`, `CFG1 [7:0] = 0x22` (код в h3_sdc1_init).

### SDIO-протокол (проверено на стенде)

- Порядок инициализации чипа (строго, подтверждён на стенде):
  `CMD5 (ждём C=1) → CMD3 (RCA) → CMD7 (select) → CMD52 вкл. Fn1 (CCCR 0x02) →
  CMD53 загрузка FW (byte-mode по 4 байта, адрес (8<<13)|0x1000) → eFuse →
  программный MAC в REG_MACID 0x610-0x615`.
  PowerSeq CARDEMU→ACTIVE (`h3_wifi_power_on_chip`) выполняется **ДО включения
  Fn1** — без него CMD53-write к функции 1 срывается (rc=-2, risr=0).
- Чистая I/O-карта: **CMD0/CMD8/ACMD41 не используются** (по спецификации SDIO).
- Рабочая ширина по эталону (dtb `sun8i-h3-orangepi-lite.dtb`, образ 2.3.1) — **4-bit**
  для mmc@01c10000 (bus-width=<4>) + pinctrl `drive-strength=<30>`, `bias-pull-up`
  (у child `sdio_wifi@1` — drive-strength=<40>). Переключение: CCCR 0x07 [2:0] = 0b10
  (bit1) + `SDC_CTYPE=1`. Такт: SCLK 24M/2/2 = 6 МГц (N=0, M=2), CKC DIV=0.
- CIS: адрес из CCCR 0x09-0x0B = 0x1000, тюпл 0x20 → Manufacturer 0x024C,
  Card/PID 0xF179 (RTL8189FTV).
- ID чипа печатается как `chip_id manf=0x024C card=0xF179`.
- MAC: eFuse по 0x11A на Orange Pi Lite пуст (FF) → задаётся программно
  (локально-администрируемый 02:00:00:XX:XX:XX) в REG_MACID 0x610-0x615
  через CMD53 без инкремента (0x610 len=4, 0x614 len=2).

### RF-доступ и калибровка (работает)

- **LSSI-запись RF** (Path A): BB 0x840 = `(rf_off << 20) | (data & 0xFFFFF)`.
- **LSSI-чтение RF**: BB 0x824 (`rFPGA0_XA_HSSIParameter2`) — адрес в [30:23]
  (сдвиг 23, НЕ 20), edge-триггер bit31 (записать со снятым → поднятым edge),
  пауза ~100 мкс, readback из BB **0x8A0** (LSSI) или 0x8B8 (PI, если bit8 в
  0x820=1); значение = readback & 0xFFFFF.
- **ВАЖНО**: LSSI работает только после загрузки полного BB-конфига
  (`bb_phy_reg[]` из `halhwimg8188f_bb.c`, 209 записей) — он настраивает
  RF-интерфейс (0x840 LSSI mode, 0x860/0x864/0x870 SW control). Без него
  readback = 0.
- **Включение BB/RF** (PHY_BBConfig8188F): SYS_FUNC_EN(0x02) |= BIT13|BIT0|BIT1
  → сброс BB (байт0=0x16 → 0x14) → REG_RF_CTRL(0x1F)=0x07 (RF_EN|RF_RSTB|
  RF_SDMRSTB) → RF reg 1 = 0x780 (power-on, ПОСЛЕ сброса!) → SYS_FUNC_EN=0x17
  → REG_CR(0x100)=0x63F → BB-конфиг.
- **LCK** (`h3_wifi_rf_lck`, по phy_LCCalibrate_8188F): TXPAUSE(0x522)=0xFF →
  RF reg 0xB0=0xDFBF0 (LDO ON) → канал 1 в RF_CHNLBW(0x18)[7:0] →
  RF_CHNLBW |= 0x8000 (бит 15 trigger) → 100 мс → RF 0xB0=0xDFFF0 (LDO OFF) →
  TXPAUSE=0x00. Бит 15 после LCK может остаться 1 — норма для 8188F.
- RF_CHNLBW дефолт 0x400 (бит 10 = полоса 20/40М), канал в [7:0].

### RX-инициализация (по rtl8188f_hal_init/sdio_halinit) 

Порядок (проверен, все адреса из драйвера 8188F):

1. **LLT auto-init** (`rtl8188f_InitLLTTable`): `0x0224 |= BIT16`, поллинг сброса (до 1 с).
2. **TX-границы** (`_InitTxBufferBoundary`): `txpktbuf_bndy=0xF1` = TX_PAGE_BOUNDARY_8188F
   (0xFF − BCNQ 0x08 − BCNQ1 0x00 − WOWLAN 0x07 + 1) в 0x0424, 0x0425, 0x045D, 0x0114.
3. **RX-граница** (`_InitPageBoundary`): `0x0116 = 0x3EFF` (16-бит) = RX_DMA_BOUNDARY_8188F
   (0x4000 − 0x100 − 1). ВАЖНО: 0x0116, не 0x0115.
4. **PBP** (`_InitTransferPageSize`): `0x0104 = 0x11` (PBP_128|PBP_128).
5. **DRVINFO_SZ** (`_InitDriverInfoSize`): `0x060F = 0x04` (единицы 8 байт).
6. **NETTYPE_AP** (`_InitNetworkType`): `REG_CR[17:16] = 0x2` (32-бит RMW!).
7. **RCR/RRSR/RXFLTMAP/MAR** (`_InitWMACSetting`): 0x0608=0x7000208F (AAP|APP_PHYST_RXFF|
   APP_ICV|APP_MIC|AMF|CBSSID...), 0x0440=0x000F0F (скорости), 0x06A0/0x06A2/0x06A4,
   0x0620/0x0624=0xFFFFFFFF.
8. **RXDMA** (`_initSdioAggregationSetting`): `0x010C |= BIT2` (RXDMA_AGG_EN),
   `0x0290 |= BIT1` (RXDMA_AGG_MODE_EN).
9. **REG_CR MAC-блок** (sdio_halinit.c:1240 «Enable MACTXEN/MACRXEN block»):
   `0x0100 |= BIT6|BIT7 (MACTXEN=BIT6, MACRXEN=BIT7 — hal_com_reg.h:1293-1294)` —
   без MACTXEN/MACRXEN 802.11-MAC не запущен, RX невозможен в принципе.
   К ним же добавляются bit8/9 (ENSWBCN/ENSEC, из `_InitNetworkType`).
   (у нас из BB-конфига REG_CR=0x63F без них; итог 0x0206FF).
10. **SDIO HIMR** (0x14, fn0, 32-бит!): байт 0x14 = 0x07 (RX_REQUEST|AVAL|TXERR),
    байт 0x16 = 0x02 (C2HCMD BIT17). HISR (0x18) = 0xFF (clear).
11. **RW_RELEASE_EN**: `0x287 &= ~BIT18` (release RX DMA; BIT18=1 — пауза DMA).
12. **Канал**: RF_CHNLBW(0x18) = 0x401 (канал 1, BW 20М; НЕ 0x8C01 — там бит 15=rежим калибровки).

### Уточнения RX-сессии (добавлено к RX)

- **H2C SET_PWR_MODE (0x20)** — перевод FW в активный режим:
  `rtl8188f_set_FwPwrMode_cmd(psmode=0)`: payload 6 байт — [0]MODE=0, [1]rlbm(2),
  [2]awake_intvl(4), [3]uapsd=0, [4]PowerState=0x0C (AllON), [5]byte5=0x40.
  **Формат FillH2CCmd8188F**: CmdLen>3 — первые 3 байта в box0 `0x1D0` (с id),
  остальные в **EX-box `0x1F0`** (REG_HMEBOX_EXT0). НЕ писать в 0x1D4/0x1D6!
- **CCCR IENM**: `0x04` (fn0) |= BIT0 — глобальное разрешение SDIO-прерываний
  (было 0; без него 8051 не поднимает HISR-линию). **FBR INT_EN**: `0x100` (fn0)
  |= BIT0 — прерывания функции 1.
- **RPWM→CPWM active**: записать `HRPWM1 (0x0080, fn0) = 0x00` (запрос active),
  поллить `CPWM (0x0038, fn0) == 0x00` — FW подтверждает (active=1).
  0xC0 в RPWM — это запрос LPS/сна, для active надо 0x00.
- **RCR для скана**: снять `RCR_CBSSID_BCN (BIT7)` — принимаем beacon любой
  BSSID (wireless сканирование). Итог скана `0x7000604E` = AAP(0)|APM(1)|AM(2)|AB(3)
  + CBSSID_DATA(6) + AMF(13) + APP_PHYST_RXFF(28)|APP_ICV(29)|APP_MIC(30),
  снят только bit7. (ранний вариант 0x7000200F без AM/CBSSID_DATA — устарел).
- **BBTurnOnBlock**: `RFMOD (0x800) |= bCCKEn | bOFDMEn` — биты **BIT24 (bCCKEn) |
  BIT25 (bOFDMEn)** (Hal8188FPhyReg.h:515-516; НЕ bit16/17!). В
  BB-конфиге RFMOD=0x83045700 (биты 24/25 сброшены; без них CCK/OFDM-блоки
  выключены, приёмник не декодирует эфир). После: 0x83075700.
- **Канал в РАБОТЕ**: свип пишет RF_CHNLBW напрямую `0xC01/0xC06/0xC0B`
  (BW20-биты 0xC00 | канал — W.242+; раньше писали 0x201/0x206/0x20B, что
  ставило не тот бит полосы). НЕ 0x8C01 (там старшие биты от LCK/режим
  калибровки, ломает приём).
- **H2C SCAN_ENABLE (0x02) / MEDIA_STATUS_RPT (0x01)**: в драйвере-эталоне
  (8189fs v4.3.24, образ 2.3.1) определены; MEDIA_STATUS_RPT используется
  в нашем RX-блоке (MSR=STA + H2C 0x01), скан идёт через регистры.
- **Симптом**: вся конфигурация 1:1 по драйверу, но rxlen=0/pkt=0/hisr=0/c2h=0,
  rxpk BIT25=1, fifopg=0 — 8051 исполняет FW (WINTINI_RDY=1), но физически не
  декодирует кадры. Хост-сторона исчерпана → блокер в RF-приёмном тракте
  (усиление/калибровка) или FIFOPAGE 0x0200. См. NEXT-CHAT-PROMPT.md. Шаг D.

### Индикаторы RX (диагностика)

- **RX0_REQ_LEN**: `0x1C` (fn0) — длина ожидающего RX-запроса (16-бит); >0 = данные в FIFO.
- **RXPKT_NUM**: `0x284` (device 8) — число пакетов в RXPKTBUF (FW), биты [15:0].
  `0x00020000` в 32-бит = RXDMA_IDLE (BIT17, не счётчик!).
- **C2H_EVT**: `0x01A0` — событие firmware→host (0 = FW не генерирует).
- **HISR**: `0x18` (fn0) — статус SDIO-прерываний (bit0 RX_REQUEST).
- **MCUFWDL**: `0x0080` — `0xC6` = MCUFWDL_RDY|FWDL_ChkSum_rpt|WINTINI_RDY|RAM_DL_SEL
  — **ШТАТНОЕ состояние загруженной и готовой 8051** (маска value_expected драйвера).
  Бит 2 = FWDL_ChkSum_rpt (подтверждение ЧК), НЕ ошибка!

### Важные особенности (из отладки)

- Такт MMC1: **OLD mode** (бит30=0), НЕ MODE_NEW — иначе карта не отвечает.
  `SDMMC1_CLK_REG` (0x01C2008C) = SCLK_GATING | SRC_OSC24M | (2<<16) | (15<<0)
  ≈ 750 кГц, затем CKC делитель (DIV=0 → 6 МГц рабочего такта).
- Для SDIO-команд CRC ответа НЕ проверяется (CMD52/CMD53).
- Для data-команд контроллер НЕ взводит CMD_DONE — завершение ждём по
  DATA_DONE (TRANS_COMPLETE). Без этого блочная запись/чтение «зависали».
- SDIO-локальные регистры чипа (0x100, 0x86) лежат по device id **0**;
  регистры WLAN IOREG (MAC 0x05, 0x06, 0xF0) — по device id **8**.
  SDIO-адрес команды = `(device_id << 13) | (offset & mask)`.

Документация по чипу и драйверу — в `HARDWARE-WIFI.md`, референсный
драйвер Linux — в `tools/wifi/rtl8189fs/` (не включается в репозиторий).

======================================================================
# ДОКУМЕНТ: HARDWARE-WIFI.md
======================================================================

# WiFi (RTL8189FTV) на Orange Pi Lite — аппаратная справка

> Источник: схема платы `Orange_pi-lite-v1_1.pdf` (в корне проекта) + даташит H3.
> Эталон: распакованный рабочий образ `ref/Lakka-Allwinner.orangepi_lite.arm-2.3.1.image`
> и его dtb `sun8i-h3-orangepi-lite.dtb` (4-bit SDIO, шина mmc1, drive-strength/pull-up).
> Дата: 2026-10-16. Этап проекта: поднимаем WiFi.

## Чип

- **RTL8189FTV** (Realtek), QFN24, SDIO-интерфейс.
- Частота: WiFi 2.4 ГГц, 802.11 b/g/n (20/40 МГц), 1T1R.
- Кварц: 26 МГц (TSX-3225).
- Антенна: MHF/IPEX (ANT1) через согласование (10pF, 0Ом, 50 Ом трасса).

## Подключение к H3 (SO Схема)

| Сигнал чипа | Пин H3 | Назначение |
|---|---|---|
| WL-SDIO-CLK | PG0 / SDC1_CLK | SDIO-такты |
| WL-SDIO-CMD | PG1 / SDC1_CMD | SDIO-команда |
| WL-SDIO-D0 | PG2 / SDC1_D0 | данные 0 |
| WL-SDIO-D1 | PG3 / SDC1_D1 | данные 1 |
| WL-SDIO-D2 | PG4 / SDC1_D2 | данные 2 |
| WL-SDIO-D3 | PG5 / SDC1_D3 | данные 3 |
| WL-PMU-EN | — | управление питанием (мост) |
| WL-WAKE-HOST | — (GPIO0 чипа) | прерывание от чипа |
| WIFI_EN (CHIP_EN) | **PL7** (`S_PL_EINT7`) | включение чипа (active high) |

Порты PG0-PG5 — это **контроллер SDC1** (второй хостовой MMC) SoC H3.
WIFI_EN — обычный GPIO (порт L, R_PIO), HIGHT — чип включён.

## Питание

- VCC-WIFI (ядро чипа) / VCC-WIFI-IO — отдельные LDO от 3.3В (WIFI Power 3.3V/300mA).
- VCC3V3-USBWIFI — общий вход.

## Шаг 1 (план) — SDIO-контроллер H3 (SDC1)

Для общения с чипом по SDIO нужен хост-контроллер SDC1 (0x01C10000):
- Такт/сброс: CCU GATING/RST биты SDC1 (проверить в даташите H3).
- Регистры хоста — как у SD/MMC (CMD, ARG, STATUS, RCTL, BLK, DT, FIFO).
- Перед чтением чипа: подать WIFI_EN=1 (PL7), дать время на стабилизацию,
  затем выполнить SDIO CMD52/CMD53 (шина, функция 0/1).

Далее: чтение CCID (Card Information) чипа по SDIO — первый контрольный результат,
что чип отвечает. Затем — загрузка firmware (halhwimg8188f_fw.c) и ассоциация.

---

## Статус работ — актуально

### Готово (проверено на стенде)
- Схема платы изучена: чип RTL8189FTV, SDC1 (SD/MMC1 = 0x01C10000), PG0-5, WIFI_EN=PL7.
- Драйвер rtl8189fs (Linux v4.3.24) — в `tools/wifi/rtl8189fs/` (референс, не в репозитории):
  прошивка, power-seq, регистры чипа; соответствует драйверу в эталоне (образ 2.3.1).
- Собственный SDC1-драйвер `src/h3_sdc.c` + `include/h3_sdc.h`.
- SDIO-обмен работает: CMD5→CMD3→CMD7, CMD52 (обе функции), CMD53-блок-запись.
- Чтение CIS: адрес 0x1000, тюпл 0x20/0x02 → Manufacturer 0x024C (Realtek), PID 0xF179.
- 4-bit режим по эталону (dtb): CCCR 0x07 bit1=1 + SDC_CTYPE=1 +
  pinctrl drive-strength=30/pull-up (регистры DRV/PUL порта G).
- Такт: OSC24M/2/2 = 6 МГц (фазы output/sample по таблице sunxi-mmc).
- Reset контроллера: 20 мс стабилизации (патч u-boot 0015).
- Power-on чипа: PowerSeq CARDEMU→ACTIVE (WIFI_EN=PL7).
- Загрузка прошивки: протокол MCUFWDL (`h3_wifi_download_fw`), FW встроена
  в бинарь (`src/fw_blob.S` + `fw_nic.bin`), загрузка на 1-bit/50 МГц
  (4-bit ломает data-фазу CMD53 на этом стенде).
- RF-калибровки LCK/IQK, RX-приём (beacon/SSID), probe request (TX FIFO HIQ).

### В работе (W.329+)
- **RX-приём работает**: чип принимает beacon'ы на всех 13 каналах, парсер
  извлекает SSID (`BCN plen= ch= ssid=`), скан стабилен (5+ уникальных SSID).
- **RPWM/CPWM/HIMR/HISR — через fn1** (SDIO-local, device id 0), не fn0.
  С W.329 FW подтверждает переход в ACTIVE по смене toggle (cpwm=0x80, tog_sw=1),
  HIMR=0x01. Эксперимент W.337/W.338: HRPWM1 принимает 0xCC (rb=0xCC), но
  CPWM state[3:0] остаётся 0 — на связке чип/FW ACK идёт только через toggle;
  маска 0xC1 в драйвере не баг, C2H-ABSENT не связана с RPWM.
- Наблюдения по SDC1:
  - Для data-команд SDC1 не взводит CMD_DONE — ждать DATA_DONE.
  - Шина MMC1 — OLD mode (бит30=0), иначе карта не отвечает.
  - SDIO-local регистры — device id 0, WLAN IOREG — device id 8, RX FIFO — id 7.
  - **Host SDIO H3 не тянет многоблочный read CMD53** (30 блоков → rc=-1);
    только 1 блок (512 б) за CMD53.
- JOINBSS-движок реализован (host Auth/AssocReq кадры; захват BSSID+channel
  из скана; вызов из wifi_layer с Retry). Далее: обработка Auth/AssocResp из
  RX-пути для подтверждения ассоциации, затем TCP/IP (фикс IP 192.168.x.199)
  — цель «эмулятор + сетевой доступ к ROM на SD».
- **Экран Transfer**: список сетей (дедуп по BSSID, dwell 300 мс), навигация
  UP/DOWN, A=подключиться к выбранной (wifi_layer_connect). Целевая сеть для
  теста — `MTS_GPON_9290e0`.
- **ГЛАВНЫЙ БЛОКЕР TX (W.350-W.353)**: host пишет кадр в HIQ (wrc=0,
  mgmt→HIQ=4 mapping верен), но **FW не берёт его в TX-очередь**:
  `txpkt_empty=0xFF` (все очереди пусты) до и после, `TXFF_PTR` не сдвигается,
  `hiq_free` не убывает. Вероятная причина — **A0=3 (S3 = Card Enable, RF off)**
  против эталонного A0[1:0]=0x01; в S3 RX работает, TX невозможен. Правки
  TX-enable (0x101[1]=1, 0x02[1:0]=11, REG_CR|=0xC0), прямая запись A0=1,
  диагностика TRXFF_BNDY/PBP по 8-бит, udelay 50мс — внесены в W.353 (результат
  на стенде за W.353). C2H нем на все H2C (SET_PWR_MODE id=0). Пока A0 не станет
  S0/ACTIVE — JOINBSS/auth невозможны. Известные кандидаты: полная PwrSeq
  (не упрощённый 7-шаговый seq_ce2ac), RPWM не доводит до S0 (state-биты мертвы,
  W.337).

### Снимок JTAG (когда был нужен)
- После U-Boot (SD): PG0-3 уже function 2 (SDC1), такт MMC1 включён, но WIFI_EN
  (PL7)=0 — чип выключен. Наша инициализация включает PL7 и донастраивает PG4-5.
- FEL (чистое состояние): всё в сбросе; нужен полный подъём (PL7, PG0-5, такт/сброс).

### Драйвер rtl8189fs (структура, что брать)
- `hal/phydm/rtl8188f/halhwimg8188f_fw.c` — загрузочная прошивка чипа (голые байты).
- `include/Hal8188FPwrSeq.h` + `hal/rtl8188f/Hal8188FPwrSeq.c` — power sequence.
- `hal/rtl8188f/rtl8188f_hal_init.c` — загрузка прошивки (MCUFWDL) и инициализация.
- `hal/rtl8188f/sdio/sdio_ops.c`, `sdio_halinit.c` — SDIO-протокол (адресация FIFO).
- `include/hal8188freg.h` — регистры чипа.

### SDIO-команды (напоминание)
- CMD52 — байтовый доступ к функциям (R/W register).
- CMD53 — блочный обмен (данные чипа; для загрузки прошивки).
- CMD5 — I/O send op cond (детект SDIO-карты).
- CMD3/7 — выбор карты (SEL/DESELECT).

======================================================================
# ДОКУМЕНТ: KNOWLEDGE.md
======================================================================

# База знаний: Allwinner H3 (Orange Pi Lite) — bare-metal, звук I2S0 → MAX98357A

> Рабочий документ. Каждый факт помечен источником:
> - **[СТЕНД]** — проверено на нашем стенде (JTAG/FEL/логи);
> - **[LINUX]** — из официального драйвера Linux (sun4i-i2s.c / sun6i-dma);
> - **[РЕФ]** — из референсного репозитория uli/allwinner-bare-metal;
> - **[ИНОЕ]** — из внешней выжимки, НЕ проверено, возможны ошибки;
> - **[ГИПОТЕЗА]** — рабочие догадки, требуют проверки.
>
> Ревизия: 2026-10-11.

---

## 1. Проект

- Каталог: `E:\Giga_1\Bare_linux`.
- Прошивка: `h3_bare.bin` (грузится на 0x40000000 — U-Boot `go` или FEL+SPL).
- Цель: HDMI-меню + звук I2S0 → MAX98357A (кнопки), этап WiFi — подъём RTL8189FTV.
- Состояние:
  - **Звук:** готов и работает (FEL и SD) на DMA (H3 не кэш-когерентен — нужен
    clean D-cache перед стартом DMA; см. H3-BAREMETAL-GUIDE.md §5).
  - **WiFi (тег w210-rx-fifo-iqk):** SDC1-драйвер, SDIO CMD52/CMD53, чтение CIS-ID
    (manf=0x024C, card=0xF179 — RTL8189FTV), PowerSeq CARDEMU→ACTIVE, загрузка
    прошивки (fwdl rc=0, **MCUFWDL=0xC6 = все 4 бита маски драйвера
    MCUFWDL_RDY|FWDL_ChkSum_rpt|WINTINI_RDY|RAM_DL_SEL — 8051 загружена и
    готова**, 0xC6 это НЕ ошибка), чтение eFuse (MAC пуст — FF) и программная
    установка MAC в REG_MACID 0x610-0x615 (подтверждена readback).
    **RF-калибровка LCK работает** (LSSI-доступ к RF-регистрам через
    BB 0x824+edge → 0x8A0, канал читается/пишется). Ключевое: для LSSI обязателен
    полный BB-конфиг (bb_phy_reg[] из halhwimg8188f_bb.c). Подробности в
    HARDWARE-REF.md и H3-BAREMETAL-GUIDE.md §9.
    **Сессия (Шаг D, RF-приёмный тракт) — что сделано и НАЙДЕНО:**
    - **AGC-таблица** загружается ОТДЕЛЬНО после BB-конфига (bb_agc_tab, 66
      записей 0xC78+0xC50 из Array_MP_8188F_AGC_TAB) — драйвер грузит её
      вторым шагом phy_BB8188f_Config_ParaFile.
    - **RF-таблица (rf_radioa, 136 пар) загружается ДО LCK с ПРОПУСКОМ записи
      0x18** (таблица ставит 0xFC07 — ломает LCK-триггер, done за 0 мс);
      после неё явно RF 0x18=0x401; после LCK RF 0x00=0x33D95 (из таблицы,
      НЕ 0x30000). LCK снова 70 мс.
    - **RFMOD: bCCKEn=BIT24, bOFDMEn=BIT25** (Hal8188FPhyReg.h:515-516).
      Долго ставили bit16/17 — приёмник был физически глух (rfmod=0x82075700
      вместо 0x83075700).
    - **RCR=0x7000604E** (_InitWMACSetting: AM bit2 + CBSSID_DATA bit6 + ...,
      минус CBSSID_BCN для скана). Было 0x7000200F без AM/data.
    - **RQPN**: 0x200=0x80E0020C (HPQ 0x0C|LPQ 0x02<<8|PUBQ 0xE0<<16|LD_RQPN),
      0x214=0x02 — _InitQueueReservedPage. Порядок: RQPN→TX-границы(0xF1)дрLLT.
    - **BWOPMODE (0x603)=0x04** (20 МГц), SIFS/RL/EDCA как _InitEDCA/_InitAdaptiveCtrl.
    - **H2C: НЕ писать 0x00 в 0x1CC** (HMETFR) — сбивает valid-бит, FW
      перестаёт читать команды (1cc=0xFF); только ждать valid==0.
    - **IQK**: воспроизведён phy_IQCalibrate_8188F Path A 1T1R
      полностью — PI-режим (0x860/0x868=0x01000100 ON / 0x01000000 OFF),
      MAC-настройки (TXPAUSE=0x3F, BCN_CTRL 0x550/0x551 &=~BIT3, GPIO 0x40
      &=~BIT5), RF 0xEF/0x30/0x31/0x32/0xDF=0x980/0x56, тоны 0xE30-0xE4C,
      one-shot 0xE48, reload 0xDF=0x180. **Всё равно 0x142/0x42 стабильно**.
      Обвязка исчерпана; движок работает (e94/e9c обновляются), тон не доходит.
    - **W.265 (LOK_Result)**: RF 0x08 (LOK_Result из IQK) сохраняется после
      Tx one-shot и восстанавливается после каждого Rx one-shot — как драйвер
      (halphyrf_8188f.c:737/894). RF 0x08: было 0x85000 → стало 0x8F800
      (у живого 0xF800). После этого rxrpt на ch11 стал живым (0x0030FBF4 —
      младшие биты двигаются) — BB-приёмник ch11 реально декодирует сигнал.
    - **W.267-W.269**: UART-вывод убран из hot path (тайминги не искажаются).
      H2C 0x6D={1} отправляется БЕЗ ожидания valid==0 (драйвер не ждёт,
      rtl8188f_cmd.c:96-127); C2H-пинг добавлен: H2C 0x43 (AP_REQ_TXRPT),
      детект по 0x1A0 / HISR bit17 / RXPKT bit16 → **C2H-ABSENT стабильно**.
    **Результат сессии RX (финал, W.338): приём эфира работает на 13 каналах.**
    - **Host SDIO H3 не тянет многоблочный read CMD53** (>1 блок/512 б → `rc=-1`
      и зависание). Выдать пакет можно ТОЛЬКО по 1 блоку (512 б) за CMD53,
      поэтому RX-FIFO выгребается в цикле по 512 б, `seq`/окно фиксируется на
      пакет, инкремент `seq++` только когда `RX0_REQ_LEN` (0x1C) подтвердил 0.
    - **`RX0_REQ_LEN` (0x1C) показывает накопленный объём, а не один кадр**;
      ориентир завершения пакета — сброс 0x1C в 0 после выдачи.
    - **Парсер: 802.11-кадр (beacon) на смещении off+24** (RX-дескриптор 24 б,
      drvinfo в этом build = 0). SSID — IE с id=0. Парсер идёт по всем пакетам в
      окне (`off += pkt_total`, round 8), выводит каждый beacon.
    - **Результат: `BCN plen=0x10F ch=2 ssid=midea_b8_6515`** — реальный beacon
      декодирован. RX-приём ок. Блокер был НЕ в прошивке 8051 (она как раз
      принимала и копила в FIFO), а в host-чтении (30-блочный read) и в
      неправильном смещении кадра (fallback drvsz сдвигал на 28 вместо 24).
    - **W.329 (RPWM/CPWM/HIMR через fn1)**: SDIO-local регистры (device id 0)
      доступны только через fн1. После перевода с fn0: cpwm=0x80 (tog_sw=1) —
      FW подтверждает ACTIVE по смене toggle, HIMR=0x01.
    - **Эксперимент W.338 (маска RPWM 0xC1 — НЕ баг)**: запись 0xCC в HRPWM1
      даёт readback 0xCC (регистр хранит полный байт), но CPWM state[3:0]
      остаётся 0 — на связке чип/FW ACK перехода идёт только через toggle,
      как и в драйвере (rtw_pwrctrl.c:630). Снять версию «FW не в ACTIVE из-за
      маски»; C2H-ABSENT не связана с RPWM.
    - **JOINBSS (host Auth/AssocReq, W.332-W.338)**: ассоциацию в 8189fs делает
      host, не FW — issue_auth (opensystem) → issue_assocreq, ответ AP идёт
      в RX-путь. Реализован захват BSSID+channel цели из скана
      (`join: bssid=... ch=...`), вызов из пункта Transfer с Retry.
    - **Ассоциация + WPA2 4-way (W.369-W.459)**: полный join (AuthResp=0 →
      AssocResp=0 → MSR=STA, rc=0) к открытым и WPA2-сетям; 4-way доведён до
      конца (msg1→msg2 принят→msg3 GTK unwrap→msg4 key_info=0x030A с Secure→
      PTK/GTK в CAM).
    - **TX-путь и DHCP (W.540-W.597) — итоги**:
      * **Дескриптор TX** обязательные блоки: AGG_BREAK (pDesc+8 bit16),
        checksum (XOR 16 слов → байты 28-29), 8-байтный CCMP IV в кадре
        (между 802.11 hdr и LLC), OQT-слот (0x002A) перед записью.
      * **КОРЕНЬ зависания 3-страничных кадров (W.555/W.584)**: DHCP (320 Б =
        3 страницы) не выгружался LOQ. Решение — слать DHCP (≥3 стр) в **HIQ**
        (device 4) с QUEUE_SEL=QSLT_MGNT; 2-стр кадры идут в LOQ как раньше.
      * **Корень «кадр не уходил в очередь»** — блок-режим W.554 был ОШИБКОЙ
        (писал мимо окна FIFO); только byte-mode CMD53.
      * **RX-парсер**: OFFER/ACK приходят (src=67→dst=68). Исправлена проверка
        портов (W.590) — раньше принимала только 68→67 и дропала OFFER.
      * **DHCP-состояние**: `dhcp_offer_seen` не сбрасывается в DISCOVER,
        сброс на старте join (W.594); после OFFER не шлём повторный DISCOVER.
      * **Глобальный IP**: сохранён в join_target (ip/mask/gw/have_ip), раньше
        net_state был локальным в join и IP терялся (W.595).
      * Скорость DHCP/EAPOL — 1M (CCK), надёжно (W.568).

    Эталон D8: rtl8189fs-lakka-live-dump.md, LAKKA-ETALON-ZAMERY.md §6-7
    (живая CAM 0x8010/0x8051, REG_SECCFG=0x010C, 0x420=0xff311f00,
    0x200=0x00E7020C, macid id0=AP/id1=bmc RSVD, TX rate=MCS7).

---

## 2. Карта I2S0 (H3/sun8i; база 0x01C22000)

| Смещение | Регистр | Назначение | Значение / биты |
|---|---|---|---|
| 0x00 | I2S_CTL | Управление | BCLK_OUT=18, LRCK_OUT=17, MODE_SEL[5:4], TXEN=2, RXEN=1, GEN=0, SDO0_EN=8 → 0x00060117 [РЕФ] |
| 0x04 | I2S_FMT0 | Формат 0 | SW(3)<<0, SR(3)<<4, LRCK_PERIOD-1<<8, LRCKR<<20 → 0x1F33 [РЕФ] |
| 0x08 | I2S_FMT1 | Формат 1 | 0x0 (PCM без знакового расширения) [РЕФ] |
| 0x0C | I2S_INT_STA | Статус прерываний (sun8i) | [LINUX] |
| 0x10 | I2S_RXFIFO | RX FIFO | — |
| 0x14 | I2S_FCTL | Управление FIFO | TXTL(0x40)<<12, RXTL<<4, TXIM(1)<<2, RXOM<<0, FRX=24, FTX=25 |
| 0x18 | I2S_FSTA | Статус FIFO | TXE=28 (TX FIFO empty), уровень [24:16] [РЕФ] |
| 0x1C | I2S_DMA_INT_CTRL | DMA/IRQ разрешение | TX_DRQ_EN=7, RX_DRQ_EN=3 [LINUX] |
| 0x20 | I2S_FIFO_TX | TX FIFO (запись) | 32-бит [LINUX: reg_offset_txdata=0x20; СТЕНД] |
| 0x24 | I2S_CLK_DIV | Делители тактов | MCLK_EN=8, BCLKDIV<<4, MCLKDIV<<0 [LINUX] |
| 0x28 | I2S_TX_CNT | Счётчик TX сэмплов | [LINUX] |
| 0x2C | I2S_RX_CNT | Счётчик RX сэмплов | [LINUX] |
| 0x30 | I2S_CHAN_CFG | Каналы | RX_SLOT_NUM<<4, TX_SLOT_NUM<<0 [LINUX] |
| 0x34 | I2S_TX_CHAN_SEL | Выбор TX | OFFSET(1)<<12, CHSEL<<0, CHEN<<4 [LINUX] |
| 0x44 | I2S_TX_CHAN_MAP | Карта TX | 0x76543210 [LINUX] |
| 0x54 | I2S_RX_CHAN_SEL | Выбор RX | [LINUX] |
| 0x58 | I2S_RX_CHAN_MAP | Карта RX | [LINUX] |

**Замечание про адреса [СТЕНД+LINUX]:** 0x20 = FIFO_TX (подтверждено и стендом:
DMA писал в 0x01C22020 — звук шёл). 0x24 = CLK_DIV, 0x28 = TX_CNT.
Те, кто пишут «0x20 = INT, 0x24 = TXFIFO», путают с раскладкой A10/sun4i
(TX FIFO там на 0x0C). **Всё выше — для H3/sun8i.**

---

## 3. Ключевые биты I2S_CTL (проверено)

```c
I2S_CTL_BCLK_OUT  (1<<18)   // H3 генерирует BCLK (master)
I2S_CTL_LRCK_OUT  (1<<17)   // H3 генерирует LRCK (master)
I2S_CTL_SDO0_EN   (1<<8)    // выход данных SDO0 (PA20 DOUT)
I2S_CTL_MODE_SEL(n) (n<<4)  // 0=PCM,1=Left(LJ),2=Right(RJ); I2S = LEFT+offset
I2S_CTL_TXEN      (1<<2)
I2S_CTL_RXEN      (1<<1)
I2S_CTL_GEN       (1<<0)    // глобальный enable
```

**Мастер-режим = BCLK_OUT|LRCK_OUT (биты 18/17), НЕ биты 5:6.** [LINUX][РЕФ]
Биты 5:6 — это MODE_SEL (формат), их путают с Master/Slave в некоторых
выжимках. [ИНОЕ — ошибочно]

---

## 4. Тактирование (CCU)

| Регистр | Адрес | Назначение |
|---|---|---|
| PLL_AUDIO_CTRL | 0x01C20008 | PLL аудио 24.576 МГц |
| PLL_AUDIO_PAT_CTRL0 | 0x01C20284 | Pattern (sigma-delta дробь) |
| I2S_PCM0_CLK | 0x01C200B0 | Такт I2S0 (mux PLL_AUDIO) |
| BUS_CLK_GATING2 | 0x01C20068 | I2S0 = бит 12 |
| BUS_SOFT_RST3 | 0x01C202D0 | I2S0 = бит 12 (НЕ 14 — бит14 это I2S2!) [РЕФ] |
| BUS_CLK_GATING0 | 0x01C20060 | DMA = бит 6 |
| BUS_SOFT_RST0 | 0x01C202C0 | DMA = бит 6 |

**Источник такта I2S0 [даташит 4.3.5.28]:** I2S/PCM0_CLK_REG (0x0B0).
- бит31 = SCLK_GATING (0=off,1=on)
- биты[17:16] CLK_SRC_SEL: 00=**PLL_AUDIO(8X)**, 01=(8X)/2, 10=(8X)/4,
  **11=PLL_AUDIO** (1X). Для 48k НЕОБХОДИМ mux=11 (PLL_AUDIO 1X=24.576M).
  (Раньше ставили 00=8X — BCLK был неверным → тишина.)

Схема частот (48 кГц):
- AudioPLL (SDM) = 24.576 МГц
- I2SPCM0_CLK mux=11 → модульный SCLK = 24.576 МГц
- BCLKDIV=5(÷8) → BCLK = 3.072 МГц = 48k × 32 × 2
- MCLKDIV=2(÷2) → MCLK = 12.288 МГц

Наш код инициализации (I2S0):
```c
PLL_AUDIO_PAT  = 0xC000AC02u;
PLL_AUDIO_CTRL = (1<<31)|(1<<24)|(13<<8)|13;   // N=14,M=14,SDM -> 24.576M
udelay(3000);  // ждать bit28 (LOCK)
H3_CCU_GATING2 |= (1u << 12);
CCU_RST_I2S0 &= ~(1<<12); udelay(10);
CCU_RST_I2S0 |=  (1<<12); udelay(10);
I2SPCM0_CLK = (I2SPCM0_CLK & ~(3<<16)) | (3<<16) /*mux 11=PLL_AUDIO*/ | (1<<31);
```

---

## 5. Пины (PIO, PA-порт)

| Пин | Функция | Регистр/биты |
|---|---|---|
| PA0 | JTAG_TMS (F3) | PIO_CFG0 [3:0]=3 |
| PA1 | JTAG_TCK (F3) | [7:4]=3 |
| PA2 | JTAG_TDO (F3) | [11:8]=3 |
| PA3 | JTAG_TDI (F3) | [15:12]=3 |
| PA4 | UART0_TX (F2) | [19:16]=2 |
| PA5 | UART0_RX (F2) | [23:20]=2 |
| PA10 | SD усилителя (GPIO out, HIGH=работа) | PIO_CFG2 [7:4]=1 |
| PA18 | I2S0 LRCK(SYNC) (F2) | PIO_CFG2 [11:8]=2 |
| PA19 | I2S0 BCLK (F2) | PIO_CFG2 [15:12]=2 |
| PA20 | I2S0 DOUT (F2) | PIO_CFG2 [19:16]=2 |

**Важно [СТЕНД]:** запись `PIO_CFG0 = 0x3333` (полная) затирает PA4-5 → UART
умирает. Только RMW: `PIO_CFG0 = (PIO_CFG0 & 0xFFFF0000) | 0x3333`.

---

## 6. U-Boot vs FEL — фактическая разница регистров (снято JTAG [СТЕНД])

### 6a. Чистое состояние ПОСЛЕ U-Boot (SD), до нашей инициализации
Снято прошивкой jtagonly (только включила JTAG-пины PA0-3=0x3333, периферию
не трогала). U-Boot 2025.01. Полный лист — REGISTER-MAP.md (СНИМОК 2).

| Регистр | FEL/эталон (работает) | U-Boot чистый (не работает) | Что делаем мы |
|---|---|---|---|
| GICD_CTLR (0x81000) | 0x3 | **0x0** | gic_init(): =3 |
| GICC_CTLR (0x82000) | 0x3 | **0x0** | gic_init(): =3 |
| PLL_AUDIO_CTRL (0x08) | 0x91000d0d | **0x00035514** (выкл) | enable+lock |
| I2SPCM0_CLK (0xB0) | 0x80030000 | **0x0** (выкл) | bit31=1, mux=11 |
| GATING2 bit12 (I2S0) | 1 | **0** | gate on |
| RST3 bit12 (I2S0) | 1 | **0** | deassert |
| GATING0 bit6 (DMA) | 1 | **0** | gate on |
| RST0 bit6 (DMA) | 1 | **0** | deassert |
| DMA_SEC_REG (0x20) | 0xFFFF | **0x0** (secure) | =0xFFFF |
| SMTA DEC1/DEC2 | FF/7F | FF/7F (уже non-sec) | — (не трогаем) |

**Итог:** U-Boot держит DMA и I2S0 и в gate и в reset, PLL_AUDIO и I2SPCM0_CLK
ВЫКЛЮЧЕНЫ, GIC выключен, DMA-каналы secure. Наша init всё это включает
(проверено: после init все значения сошлись с эталоном FEL).

### 6b. После нашей init в U-Boot-среде — применилось всё, кроме DMA-заполнения
Такты/сбросы/защиты сошлись с FEL. Но:
- I2S0 FSTA = 0x10800000 (FIFO ПУСТ) vs рабочий 0x003c0000 (FIFO польный 60)
- ISTA = 0x54 (TXU_INT = FIFO UNDERRUN)
- при этом DMA-канал 4: EN=1, DESC="SPL", CUR_SRC="eGON" (зомби U-Boot!)

Значит: регистры ок, но FIFO не наполняется — либо DMA-канал занят зомби
загрузчика, либо наш старт не перебивает его. Это и есть корень «гонки».
Решение — dma_reap_stray() + dma_pick_channel() (см. §8).

---

## 7. TrustZone-защита (ПО ДАТАШИТУ H3 V1.2, §4.11.4.5, §4.18) [СТЕНД/ПРОВЕРЕНО]

**Важно:** прежняя запись про "TZPC 0x01C23410-20" была из внешнего референса
и НЕ соответствовала даташиту. На H3 блок 0x01C23400 — это **SMTA**
(Secure Memory Touch Arbiter, §4.18), а не TZPC. Регистры порта:

```
SMTA_DECPORT0: STA 0x04, SET 0x08, CLR 0x0C
SMTA_DECPORT1: STA 0x10, SET 0x14, CLR 0x18   <- bit1=DMA, bit3=Crypto, bit4=USB
SMTA_DECPORT2: STA 0x1C, SET 0x20, CLR 0x24   <- bit3=CCU, bit2=SysCtl, bit6=RTC
```
STA=RO (0=secure,1=non-secure). SET=WO (1->non-secure), CLR=WO (1->secure).

Плюс **DMA_SECURE_REG** (DMA база + 0x20): бит канала, 0=Secure,1=Non-secure.

**Снять защиты для звука (secure.c):**
```c
DMA_SECURE_REG |= 0xF;        /* каналы DMA0-3 non-secure */
SMTA_DEC1_SET  |= (1<<1);     /* DMA non-secure */
SMTA_DEC2_SET  |= (1<<3);     /* CCU non-secure */
```
U-Boot может вести CPU в Non-Secure → Secure-каналы DMA и CCU недоступны.

**Глобальные регистры прерываний:** U-Boot гасит GIC. Карточно нужен
`gic_init()` (distributor 0x01C81000: сброс enable/pending/active, target CPU0,
включить группы 0/1; CPU interface 0x01C82000: PMR=0xFF, CTL=grp0|grp1). Сделан
в src/gic.c. Для tone с DMA-IRQ внешние irq не обязательны, но GIC инициализация
обязательна чтобы вообще не было мусора.

---

## 8. DMA sun6i (что проверено [СТЕНД])

- База 0x01C02000; канал n: регистры +0x100 + n*0x40.
- LLI = 6 слов (cfg, src, dst, len, para, next).
- Конец одноразового списка: `next = 0xfffff800`.
- Кольцо: next1 → addr(desc2), next2 → addr(desc1) (без 0xfffff800).
- **Выравнивание дескриптора: строго 32-байт** (правило sunxi-dma) [ИНОЕ+СТЕНД].
- DRQ: SDRAM=1, I2S0_TX=3.
- Запуск: gate bit6 → деассерт reset bit6 → LLI в uncached RAM → CH_LLI=addr →
  CH_EN=1.
- CFG для memcpy 0x04c104c1; I2S TX (RAM→FIFO): SRC 32b burst4, DST 32b burst1
  DRQ=3.

**Критично [СТЕНД]:** один самозацикленный LLI на весь буфер — **молчит**.
Два LLI по полбуфера — работают (звук был). Проверено достоверно.

### Важно: 0x01C02020 — это DMA_SEC_REG, НЕ soft-reset [даташит 4.11.4.5]
- Регистр 0x20 DMA — **DMA_SEC_REG** (бит на канал: 0=Secure,1=Non-secure).
- НЕ путать с «soft-reset». Мягкий сброс DMAC делается через CCU 
  BUS_SOFT_RESET0 bit6. Но в SD-режиме это РИСКОВАННО (см.ниже).

### Осиротевшие DMA-каналы после U-Boot — корень «то рычание, то тишина» [СТЕНД]
- После `go` код U-Boot не исполняется, НО его включённый DMA-канал — автономный
  агент: продолжает возить данные в DRAM, пока не остановят. В JTAG-дампе видно
  канал 4: EN=1, DESC потом "SPL", CUR_SRC="eGON" — остаток чтения eGON-образа.
- Такой «зомби» конфликтует с нашим LLI/буфером -> гонка: перебил чужое =
  звук (рычание), не перебил = тишина.
- Лечение (`dma_reap_stray()` в h3_dma.c): гасить ТОЛЬКО канальными регистрами
  (EN=0, PAU=0, ждать busy=0 в DMA_STA с таймаутом, W1C по 2 битам канала),
  БЕЗ CCU-сброса и БЕЗ трогания глобальных IRQ_EN. USB имеет собственный
  HC-DMA (не общий 0x01C02000), HDMI — регистры DE2/TCON: их не задеваем.
- После reap выбираем канал через dma_pick_channel() (сначала предпочт., потом
  любой свободный по DMA_STA), а не жестко канал 4.

### ВАЖНО: в SD-режиме канал 4 ПРОДОЛЖАЕТ жить дескриптором SPL [СТЕНД, JTAG]
- JTAG-снимок всех 12 каналов в U-Boot: канал 4 = EN=1, DESC=0x034c5053("SPL"),
  CFG=0xea000016, CUR_SRC=0x4e4f4765("eGON"). Он стабильно держится между
  снимками и ИГНОРИРУЕТ наши записи DESC_ADDR/EN -> звук на канале 4 не идёт.
- Каналы 5..11 полностью свободны (все 0). Поэтому dma_pick_channel() теперь
  выбирает СНАЧАЛА 5..11 (выше SPL-диапазона), затем 0..4. Ожидаем ровный звук
  в U-Boot на канале 5+.
- Замечание: реестра 0x20 DMA = DMA_SEC_REG (не soft-reset). Аналогично в FEL
  DMA стартует В RESET (RST0 bit6=0) — нужен условный deassert, иначе регистры
  DMA (SEC/AUTOGATE/каналы) не отвечают на запись (см. §ниже "FEL deassert").

---

## 9. I2S TX FIFO формат — TXIM (КРИТИЧНО, [даташит 8.6.7.6])

- FIFO пишется 32-бит словами. **Пара = [L<<16][R<<16]** (сэмпл в СТАРШИХ битах).
- **TXIM (FCTL bit2) должен быть 0** («данные в MSB»). При TXIM=1 FIFO берёт
  младшие 16 бит (0) → тишина на выходе. Это была ОДНА из причин.
- TXTL (FCTL[18:12]) = 0x40 → trigger level 64 (DRQ просит FIFO когда
  empty-place > 64).

## 10. Правильный порядок плайбека [LINUX sun4i_i2s_start_playback]

При каждом старте (после init):
```c
I2S_FCTL |= (1<<25);      // flush TX FIFO
I2S_TXCNT  = 0;           // сброс TX counter
I2S_CTL  |= TX_EN;        // TX block enable
I2S_INT  |= (1<<7);       // TX DRQ enable
```
I2S_CTL включает BCLK_OUT|LRCK_OUT|SDO0_EN|MODE_LEFT(1)|TX_EN|GL_EN.
stop: I2S_CTL &= ~TX_EN; I2S_INT &= ~TX_DRQ.

## 11. Корректность DMA (без прерываний)

При DMA с LLI прерывания можно НЕ включать (IRQ_EN=0): pending-биты не
блокируют канал. `IRQ_PEND=0xFFFFFFFF` в главном цикле (старый i2s_tick)
был ВРЕДЕН — снимал все каналы и мог ломать U-Boot DMA. Убран.

**LLI обязан лежать в uncached.** В linker.ld .coherent (g_lli) вынесен на
0x40100000 (своя 1МБ-секция), .dma_buf на 0x40200000, обе uncached через
mmu_mark_uncached(). НЕ помещать g_lli в секцию кода (0x40000000) — DMAC
читал стэйл-данные кэша.

## 12. Сравнение H3 vs H313/H616 (для будущего) [ИНОЕ]

| Параметр | H3 (OPi Lite) | H313/H616 (TV-box) |
|---|---|---|
| CPU | Cortex-A7 ×4 (ARMv7) | Cortex-A53 ×4 (ARMv8-A) |
| Тулчейн | arm-none-eabi (`-mcpu=cortex-a7`) | aarch64-none-elf (`-march=armv8-a`) |
| CCU | 0x01C20000 | 0x03001000 |
| PIO | 0x01C20800 | 0x0300B000 |
| UART0 | 0x01C28000 | 0x05000000 |
| GIC | 0x01C81000/0x01C82000 (GIC-400) | 0x03021000/0x03022000 |
| Старт RAM | 0x40000000 | 0x40000000 |
| I2S | 0x01C22000 | 0x05060000 |

---

## 11. Комьюнити / источники [ИНОЕ]

- **IRC `#linux-sunxi`** на Libera.chat — главный оплот (разработчики U-Boot/Linux).
- **GitHub**: `uli/allwinner-bare-metal` (MIT, фреймворк: audio_i2s.c, dma.c,
  mmu.c), форки utmux/dwelch67.
- **Форум Armbian** — темы по H3/H616, тактированию, GICv2.
- **Китайские**: Gitee, CSDN, EEFocus — по sun50iw9/H616 дисплей и DMA.

---

## 13. Открытые вопросы / TODO (этап WiFi)

- [x] Загрузка прошивки чипа: `h3_wifi_download_fw` — **работает (fwdl rc=0)**.
- [x] PowerSeq CARDEMU→ACTIVE до включения Fn1 — **работает (pwon rc=0)**.
- [x] eFuse: MAC пуст (FF) на Orange Pi Lite, задаётся программно в REG_MACID.
- [x] **RF включение + LSSI-доступ + калибровка LCK** — работают:
      канал читается/пишется через LSSI (BB 0x824+edge → 0x8A0), RF 0x18
      дефолт 0x400. RF-таблица до LCK (пропуск 0x18), LCK 70 мс.
- [x] **AGC-таблица** — загружается (bb_agc_tab, 66 пар, +0xC50/0xC78).
- [x] **IQK** — обвязка СДЕЛАНА полностью (PI-режим, MAC-настройки,
      RF/BB по эталону), но НЕ СХОДИТСЯ (0x142/0x42 стабильно). Движок работает,
      тон не доходит. Оставлен в коде (main.c: LCK→IQK→RX), не блокирует прогрузку.
- [ ] **RX-путь** — полностью настроен (RQPN, BWOPMODE, RCR 0x7000604E,
      RFMOD BIT24|25), но FW не принимает ни одного пакета (rxpkt счетчик=0).
      Блокер — физика/состояние RF (см. NEXT-CHAT-PROMPT Шаг D остаток).
- [ ] Далее: 802.11 (probe/scan), ассоциация, WPA2, TCP/IP, SMB/HTTP.
- [ ] Прошивку стоит грузить с SD-карты, а не встраивать в бинарь (для компактности
      образа и переносимости).
- [ ] sunxi-fel собран и живёт в `tools/fel/` (из исходников sunxi-tools).

## 14. Уроки (не наступать дважды)

1. Полная запись `PIO_CFG0=0x3333` убивает UART0-TX (PA4) — только RMW. [СТЕНД]
2. Один LLI на H3 молчит; два LLI — работают. [СТЕНД]
3. U-Boot гасит GIC И держит DMA/I2S0 в gate+reset, PLL и I2SPCM0_CLK выкл. [СТЕНД/JTAG]
4. Диагностировать регистры ДО нашей инициализации, а не после. [СТЕНД]
5. JTAG = PA0-3 (F3, 0x3333). Данные про PF — ошибочны. [СТЕНД]
6. 0x20 I2S = TXFIFO; 0x20 DMA = DMA_SEC_REG (не soft-reset). [СТЕНД]+[LINUX]+[даташит]
7. **LLI DMA НЕ в секцию кода** — выносить в uncached 1МБ-секцию. [даташит]
8. **TXIM=0** (данные в MSB) — иначе тишина при формате [L<<16][R<<16]. [даташит]
9. **I2SPCM0_CLK mux=PLL_AUDIO (не 8x)** — иначе неверный BCLK. [даташит+Linux]
10. **После U-Boot `go` есть осиротевшие DMA-каналы** — гасить только канальными
    регистрами + W1C, без CCU-сброса и без 0xFFFFFFFF. [СТЕНД/JTAG]
11. **openocd по конфигу делает halt и НЕ resume** — «зависания платы» при отладке
    часто мои: всегда после снятия карты делать `resume`+`shutdown`.
12. Всё нужное для проекта хранить в папке проекта (temp-каталог gigatool чистится).
    sunxi-fel собран и живёт в `tools/fel/` (из исходников sunxi-tools).
13. **Для data-команд SDC1 НЕ взводит CMD_DONE** — ждать DATA_DONE
    (TRANS_COMPLETE), иначе блок-операция возвращает -2 с risr=0. [СТЕНД]
14. **Шина MMC1 — OLD mode** (бит30=0 в SDMMC1_CLK_REG); MODE_NEW не даёт ответа
    карты. [СТЕНД]
15. **SDIO-адресация двух доменов**: SDIO-local регистры — device id 0, WLAN IOREG —
    device id 8. Смешивание даёт неверные значения (0xEA). [СТЕНД]
16. **Функция 0 (CCCR/CIS) не поддерживает блочный CMD53** — тюплы читать по
    байтам через CMD52. Блочный CMD53 — только для функции 1 (WLAN IOREG/FW). [СТЕНД]
17. **Для CMD52/CMD53 CRC ответа НЕ проверять** (SDIO-команды) — иначе ложная
    ошибка CRC. [СТЕНД]
18. **PowerSeq CARDEMU→ACTIVE обязателен ДО включения Fn1 и ДО загрузки FW**:
    без него CMD53-write к функции 1 срывается (rc=-2, risr=0), хотя CMD52
    проходят. Порядок: CMD5/3/7 → pwon → fn1(CCCR 0x02) → fwdl. [СТЕНД]
19. **CMD53 byte-mode: хвост (total не кратен 4) надо явно дописывать/дочитывать.**
    Write: FIFO требует ВСЕ байты BYCNT — остаток писать упакованным словом.
    Read: для len<4 FIFO пуст в момент проверки — ждать данные (sdc_wait
    FIFO_EMPTY=0), иначе хвост читается нулями. [СТЕНД]
20. **CMD52 write маска данных — `val & 0xFF`, НЕ `0x7F`** (терялся 8-й бит). [КОД]
21. **Побайтовое чтение через CMD53 (len=1 на каждый байт) даёт сдвиг/дубли** —
    читать одним CMD53 по 4 байта (или 2 по явному адресу, inc=0). [СТЕНД]
22. **Инкремент адреса (bit26) CMD53 в byte mode глючит на границе 4 байт**
    (REG_MACID 0x610/0x614): надёжнее раздельные CMD53 с inc=0 на явные адреса. [СТЕНД]
23. **Для RF-доступа (LSSI) обязателен полный BB-конфиг** (bb_phy_reg[] из
    halhwimg8188f_bb.c, 209 записей): он настраивает RF-интерфейс (0x840 LSSI
    mode, 0x860/0x864/0x870). Без него LSSI-readback = 0. [СТЕНД]
24. **LSSI-чтение 8188F**: адрес RF в BB 0x824 [30:23] (сдвиг 23, не 20!),
    edge-триггер bit31 (снять → поднять), пауза, readback из BB 0x8A0 (LSSI)
    или 0x8B8 (PI, если bit8 в 0x820). Запись: BB 0x840 = (off<<20)|data. [СТЕНД]
25. **Порядок включения BB/RF (PHY_BBConfig8188F)**: SYS_FUNC_EN|=BIT13|BIT0|BIT1
    → сброс BB (0x16→0x14) → RF_CTRL(0x1F)=0x07 → RF reg1=0x780 (power-on ПОСЛЕ
    сброса!) → SYS_FUNC_EN=0x17 → REG_CR=0x63F → BB-конфиг. [СТЕНД]
26. **После LCK бит 15 в RF_CHNLBW может остаться 1** — это норма для 8188F:
    драйвер не поллит сброс, а ждёт 100 мс и выключает LDO (RF 0xB0=0xDFFF0). [СТЕНД]
27. **RFMOD: bCCKEn=BIT24, bOFDMEn=BIT25** (Hal8188FPhyReg.h:515-516). Ставили
    bit16/17 — приёмник глух (rfmod=0x8207 vs 0x8307). [СТЕНД]
28. **RF-таблицу грузить с ПРОПУСКОМ записи 0x18** (таблица даёт 0xFC07 ->
    LCK done за 0 мс); после неё явно RF 0x18=0x401. RF 0x00 после LCK=0x33D95
    (из таблицы), НЕ 0x30000. [СТЕНД]
29. **H2C: НЕ писать 0x00 в 0x1CC (REG_HMETFR)** — это сбивает valid-бит, FW
    перестаёт читать мейлбокс (лог 1cc=0xFF). Драйвер (FillH2CCmd8188F) только
    читает 0x1CC и ждёт valid==0. [СТЕНД]
30. **IQK Path A 1T1R (halphyrf_8188f.c)**: нужен PI-режим (0x860/0x868=
    0x01000100 ON, 0x01000000 OFF — не 0!) и MAC-настройки калибровки
    (TXPAUSE=0x3F, 0x550/0x551 &=~BIT3, 0x40 &=~BIT5). RF 0xDF readback
    неточен (0x980->0x900) — норма. 0x948 в Path A не трогать. [СТЕНД]
31. **RXPKT_NUM (0x284)**: бит17=RXDMA_IDLE (чтение 0x284 даёт 0x00020000).
    Счётчик пакетов [15:0]. 0x287 — старший байт 0x284, скрывает счётчик.
    trssi/cca (0xA50/0xA08) в драйвере НЕ читаются — не живой индикатор. [СТЕНД]

---

## Приложение: JTAG-подключение

```
openocd -f tools/jtag/h3_pico_jtag.cfg    # CMSIS-DAP, 500 кГц, TAP 0x5ba00477
```
- Чтение регистров: `mdw <addr> <count>` через telnet :4444.
- ВАЖНО: конфиг делает `halt`; после чтения всегда `resume` + `shutdown`, иначе
  плата «зависает» (она не виснет — это мой halt).
- Для снятия чистой карты после U-Boot — прошивка jtagonly (только JTAG-пины).

Распиновка: PA0=TMS(pin13→GP14), PA1=TCK(11→GP19), PA2=TDO(22→GP21),
PA3=TDI(15→GP18), GND(pin6). Скорость 500 кГц (выше — нестабильно).

======================================================================
# ДОКУМЕНТ: REGISTER-MAP.md
======================================================================

# Карта регистров Allwinner H3 — актуальные значения

> Снимки значений, проверенные на стенде (FEL-загрузка, наша прошивка).
> Это рабочий справочник для отладки; подробные выводы — в H3-BAREMETAL-GUIDE.md
> и KNOWLEDGE.md. Ревизия: 2026-10-16 (эталон — распакованный образ Lakka 2.3.1).

## I2S0 (0x01C22000) — эталон (работает)

```
0x01C22000 CTL    = 00060111   <- BCLK_OUT(18) LRCK_OUT(17) SDO0(8) MODE_LEFT(1<<4) TX_EN(2) GL_EN(0)
0x01C22004 FMT0   = 00001f37   <- LRCKPER=32(0x1f<<8) SR=3(16b,<<4) SW=7(32b)
0x01C22008 FMT1   = 00000000   <- SEXT=0 (zeros LSB pad)
0x01C2200C ISTA   = 00000044
0x01C22014 FCTL   = 000400f0   <- TXTL=64(0x40<<12) TXIM=0 RXOM=0
0x01C22018 FSTA   = 003c0000   <- TX FIFO level 0x3c=60 (почти полон -> DMA пишет)
0x01C2201C INT    = 00000000   <- DRQ через DMA (INT reg не обязателен)
0x01C22020 TXFIFO = 00000000
0x01C22024 CLKD   = 00000152   <- MCLKO_EN(8) BCLKDIV=5(<<4=0x50) MCLKDIV=2
0x01C22028 TXCNT  = 0001248d   <- считает отправленные сэмплы
0x01C2202C RXCNT  = 00000000
0x01C22030 CHCFG  = 00000011   <- TXSLOT=2, RXSLOT=2
0x01C22034 TXSEL  = 00001031   <- OFFSET=1(12) CHEN=2(<<4) CHSEL=2-1
```

## CCU (0x01C20000)

```
PLL_AUDIO_CTRL(0x08) = 91000d0d   <- EN(31) SDM(24) LOCK(28) N=13 M=13 + PAT
I2SPCM0_CLK(0xB0)    = 80030000   <- bit31=on, bits17:16=11 => PLL_AUDIO 1X
GATING0 (0x60) = 00884040   <- DMA(bit6), HSTMR(bit19)
GATING2 (0x68) = 00001020   <- I2S0(bit12)
RST0    (0x2C0)= 00884040   <- DMA/HSTMR deassert
RST3    (0x2D0)= 00001000   <- I2S0 deassert
SDMMC1_CLK (0x8C) = 8002000F   <- SCLK_GATING(31), SRC_OSC24M, N=2, M=16 (~750 кГц)
RST0     (0x2C0) bit9 = 1     <- MMC1 deassert (GATING0 bit9=1)
```

## DMA (0x01C02000)

```
0x01C02000 IRQ_EN0   = 00000000
0x01C02020 SEC_REG   = 0000FFFF   <- все каналы non-secure
0x01C02028 AUTO_GATE = 00000004   <- авто-gating
0x01C02030 DMA_STA   = 00000010   <- при активном звуке канал 4 busy
```

## SMTA / TrustZone (0x01C23400)

- SET-регистры читаются как 0 всегда (WO); STA показывает реальную защиту.
- У нас STA=FF/7F — всё non-secure, защиты сняты.

## GIC (0x01C81000 dist / 0x01C82000 cpu)

```
DIST_CTL(0x00) = 00000003   <- EnableGrp0|EnableGrp1
CPUIF_CTL(0x00)= 00000003   <- Grp0|Grp1 enabled
CPUIF_PMR(0x04)= 000000f8   <- prio mask
```

## SDC1 / MMC1 (0x01C10000) — WiFi, снимок после нашей инициализации

```
0x00 GCTL  = 20000110   <- INT_ENB(4) CD_DBC(8) + AHB FIFO mode (bit31) при передаче
0x04 CKC   = 00030000   <- CCLK_ENB(16) CCLK_CTRL(17) DIV=0 (такт 6 МГц, 24M/2/2)
0x08 TMOR  = 00FFFFFF40
0x0C CTYPE = 00000001   <- 4-bit шина (эталон dtb bus-width=<4>)
0x10 BLKSZ = 00000200   <- 512
0x38 RISR  = 00000004   <- после CMD: CMD_DONE (для data-команд ждём DATA_DONE/bit3)
0x3C STA   = 0001AD06   <- FIFO/статус
0x44 FUNS  = 0000CEAA   <- включить функции контроллера
```

Начальные значения снимка (до инициализации):
```
pin: pl_before=0x400, pl_after=0x480 (WIFI_EN PL7 поднят)
pg_cfg0=0x77772222, pg_cfg1=0x00777722 (PG0-5 = function 2 = SDC1)
```

## Фактические значения из UART-лога (работает — полный init + RF LCK)

```
sdio_init rc=0
ccid0=0x32 ccid1=0x02
chip_id rc=0 manf=0x024C card=0xF179    <- RTL8189FTV подтверждён
pwon rc=0                                <- PowerSeq CARDEMU→ACTIVE
fn1 ioen=0x02                            <- функция 1 включена
fwdl rc=0 len=0x4972                     <- загрузка прошивки OK, FW READY
efuse rc=0 mac=FF:FF:FF:FF:FF:FF         <- eFuse пуст (Orange Pi Lite)
mac_set=OK macid_re=02:00:00:54:46:98    <- программный MAC записан и подтверждён
lck: sf_pre=0xFC00 sf8=0x17              <- BB/RF enable применился
lck: bb_config loaded (209 regs)         <- полный BB-конфиг (LSSI-интерфейс)
lck: lssi_8A0raw=0x00100000              <- LSSI readback ожил
lck: chnlbw_pre=0x400 set=0x401          <- канал 1 через LSSI (дефолт полоса 0x400)
lck: chnlbw_trig=0x8401 done=0x8401      <- LCK отработал (бит15 остаётся — норма)
lck: done
```

Ключевые регистры SDC1 во время работы (4-bit, 6 МГц — эталон dtb):
```
0x00 GCTL  = 0x20000110   <- INT_ENB(4) CD_DBC(8) + AHB FIFO (bit31) при передаче
0x04 CKC   = 0x00030000   <- CCLK_ENB(16) CCLK_CTRL(17) DIV=0 (6 МГц)
0x08 TMOR  = 0xFFFFFFFF   <- ставится на каждую передачу (макс. таймаут)
0x0C CTYPE = 0x00000001   <- 4-bit шина (эталон bus-width=<4>)
0x18 CMDR  = 0x80002675   <- CMD53 write: START|idx|RESP|DATA_TRANS|TRANS_DIR|WAIT_PRE
0x38 RISR  = 0x00000004   <- CMD_DONE (для data-команд ждём DATA_DONE/bit3)
0x3C STA   = 0x0001AD06   <- FIFO/статус
0x44 FUNS  = 0x0000CEAA   <- включить функции контроллера
0x200 FIFO = PIO-доступ
```

RF/BB-регистры чипа (WLAN IOREG, Path A), задействованные в LCK:
```
0x0100 REG_CR     = 0x0000063F  <- HCI_DMA|TXDMA|RXDMA|PROTOCOL|SCHEDULE|MAC_EN|RF_EN
0x0002 SYS_FUNC_EN= 0x0000FC17  <- BB/RF питание (BIT13) + BB из сброса (0x17)
0x001F REG_RF_CTRL= 0x07        <- RF_EN|RF_RSTB|RF_SDMRSTB
0x0E30 BB rTx_IQK_Tone_A = 0x18008C1C  <- запись-чтение работает (BB жив)
0x0820 BB HSSI-para1   = 0x01000100  <- LSSI длина данных (из BB-конфига)
0x0824 BB HSSI-para2   = 0x00390204  <- LSSI адрес/edge (из BB-конфига)
0x08A0 BB LSSI readback(Path A)      <- данные RF-регистра (readback)
RF 0x18 RF_CHNLBW = 0x400 (дефолт полоса) / 0x401 (канал 1) / 0x8401 (LCK trig)
RF 0x01 = 0x780   <- RF power-on
RF 0xB0 = 0xDFBF0 (LDO ON) / 0xDFFF0 (LDO OFF)
```

## Снимок RX W.316 — приём РАБОТАЕТ (beacon/SSID декодирован)

> Актуально на W.316 (2026-10-18). Старые снимки ниже про `rxlen=0/pkt=0`
> и «блокер физический приёмный тракт» — УСТАРЕЛИ: RX-приём ожил.

```
rx: t=0 ch=1 rxlen=0x0127 blk rc=0 blk rc=0 fifo_rd rc=0 read=0x400 rxlen2=0
    BCN plen=0x0000010F ch=1 ssid=midea_b8_6515
```

Ключевые индикаторы живого RX-приёма (по всем 13 каналам):
- `trxst` (0x11C RXFF_PTR) монотонно растёт (FIFO реально заполняется).
- `pkt` (0x287) растёт с каждым каналом.
- `hisr` (0x18) bit0=1 = RX_REQUEST — FW выложила пакет.
- Парсер выводит `BCN plen= ch= ssid=` — кадр декодирован, SSID извлечён.

Почему раньше «ни один пакет» (W.269-W.315): НЕ прошивка 8051 и НЕ физика.
Причины: (1) host SDIO H3 не тянет многоблочный read CMD53 (30 блоков → rc=-1
и зависание) — выдать можно только по 1 блоку 512 б/CMD53; (2) парсер искал
кадр на off+28 (fallback drvsz=4), а реально beacon лежит на off+24;
(3) парсер гейтился по `frc==0`, но лишний big-read возвращал -1. Исправлено
в W.316: чтение по 1 блоку, drvsz из поля (0) без fallback, парсер по `acc>0`.

## Снимок RX-диагностики (полный init + RX-конфиг)

```
rx: mcu=0xC6 (fw_rdy=1 rst=0)      <- 8051 загружена: RDY|ChkSum|WINTINI|RAM_DL_SEL
rx: sf16=0xFC17 (cpu_en=1)          <- SYS_FUNC_EN (bit10 CPUEN взведён)
rx: rpwm=0xC0 cpwm=0x80 (tog_sw=1)  <- ACTIVE подтверждена по смене toggle (fn1)
rx: rf18=0x401                      <- канал 1, BW 20М
rx: llt=0x2020                      <- LLT построена (инкремент половины?)
rx: rx_bndy=0x3EFF                  <- RX_DMA_BOUNDARY (0x0116)
rx: cr=0x000207FF (bit17=1 bit8=1 bit9=1) <- NETTYPE_AP + MACTXEN + MACRXEN
rx: rxpkt=0x2X08000 (rw_release=0)  <- счётчик пакетов растёт; RXDMA_IDLE снят
rx: himr_lo=0x01                    <- RX_REQUEST размаскирован (fn1)
rx: hisr_before=0x01 (b0=1)         <- RX_REQUEST пришёл от FW
rx: BCN plen=0x10F ch=2 ssid=midea  <- beacon декодирован (RX-приём работает)
```

## Снимок (финал хост-стороны RX)

```
rx: mcu=0xC6 (fw_rdy=1 rst=0)      <- 8051 исполняет FW (WINTINI_RDY=1)
rx: rpwm=0x00 cpwm=0x00 (active=1) <- FW подтвердила активный режим
rx: h2c_pwrmode 1cc=0x0            <- H2C 0x20 SET_PWR_MODE принят (box0 0x1D0)
rx: fbr=0x07 cccr_set=0x01         <- FBR INT_EN + CCCR IENM включены
rx: rf18=0x401                     <- канал (свип: 0x201/0x206/0x20B как SwChnl)
rx: rfmod=0x83075700               <- CCK/OFDM-блоки ON (было 0x83045700)
rx: cr=0x000207FF (bit17/8/9=1)    <- NETTYPE_AP + MACTXEN + MACRXEN
rx: rxpk=0x02000000 (rw_release=0)  <- 0x284: RXDMA_IDLE (старшие биты; счётчик [15:0]=0); RW_RELEASE=0
rx: himr_lo=0x07 hi=0x03           <- RX_REQUEST|AVAL|TXERR + C2HCMD
rx: rcr=0x7000200F                 <- CBSSID_BCN(bit7) снят; AB(bit3) сохранён
rx: c2h_evt=0x0 hisr=0x0           <- 8051 не генерирует события
rx: t=0..11 ch=1/6/11 rxlen=0 pkt=0<- ни одного декодированного кадра
```

**Вывод (хост-сторона)**: хост-сторона 1:1 по драйверу (все шаги: H2C 0x20,
CCCR/FBR, RPWM active, RFMOD CCK/OFDM, RCR ~CBSSID_BCN, каналы SwChnl 0x201),
FW жива и читает H2C, но RX-событий нет. Блокер — физический приёмный тракт.

## Снимок (точка восстановления — RF-тракт исчерпан)

Ключевые регистры после полной инициализации:

```
# LCK (RF-таблица до LCK с пропуском 0x18, чистый канал)
lck: rf_tab loaded (136 regs)  rfread df=0x100 56=0x51CF0 30=0x10000 ...
lck: rf18_after_tab=0x401  chnlbw_set=0x401  done in 70 ms
lck: rf00_restored=0x33D95   <- из RF-таблицы (не 0x30000!)

# IQK (PI-режим + MAC-настройки, но 0x142/0x42 стабильно)
iqk: pi_on 860=0x01000100 868=0x01000100   <- PI-режим RF-интерфейса
iqk: mac_cal set            <- TXPAUSE=0x3F, BCN_CTRL 0x550/0x551 &=~BIT3, 0x40 &=~BIT5
iqk: rf30=0x20000 rf31=0xF rf32=0x7FF7 rfdf=0x900 rf56=0x5102A rfEF=0x80000
iqk: tx e94=0x01420000 e9c=0x00420000 eac=0x04002000 <- FAIL (0x142/0x42)

# RX-конфигурация
rx: rfmod=0x83045700        <- RFC MOD: bCCKEn(24)|bOFDMEn(25) ON
rx: rqpn=0x00E7020C          <- REG_RQPN (0x200): HPQ 0x0C|LPQ 0x02<<8|NQ 0x07<<24
rx: llt=0x2020  rx_bndy=0x3F7F
rx: bwop=0x04                <- REG_BWOPMODE (0x603): BW_OPMODE_20MHZ
rx: cr=0x000207FF (macrx_en=1 mactx_en=1)  <- MACTXEN=6, MACRXEN=7
rx: rxdma_mode=0x0E agg_th=0x010F  <- RXDMA_MODE_CTRL (0x290), AGG_PG_TH (0x280)
rx: rcr=0x7000604E rrsr=0xFFF mar=0xFFFFFFFF
rx: fifopg=0x00E7020C        <- readback RQPN (0x200)

# Свип — FW не приняла НИ ОДНОГО пакета
rx: t=0..11 ch=1/6/11 rxlen=0 rxpkt=0x00020000 (RXDMA_IDLE) pkt=0
rx: c2h_evt=0 hisr=0
```

**Вывод (RF-тракт)**: BB/RF/MAC полностью 1:1 с драйвером (209+66+136 регистров,
LCK 70 мс, RQPN, BWOPMODE, агрегация, RCR, H2C active), IQK-обвязка по эталону
(PI+MAC+RF+BB), но FW не приняла ни одного пакета (счётчик RXPKT_NUM [15:0]=0).
Блокер — физический приёмный тракт (сигнал не доходит до демодулятора) либо
требуется эталонный лог с Linux-драйвера. Далее: NEXT-CHAT-PROMPT.md Шаг D
(probe request, физика, эталонный лог).

======================================================================
# ДОКУМЕНТ: RESEARCH-MATCHAST.md
======================================================================

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

======================================================================
# ДОКУМЕНТ: LAKKA-ETALON-ZAMERY.md
======================================================================

# Эталон: замеры работающего Linux (Lakka 2.3.1) — WiFi RTL8189FTV

> Дата: 2026-10-16. Снято живьём по SSH с платы Orange Pi Lite,
> загруженной **Lakka-Allwinner.orangepi_lite.arm-2.3.1** (тот же образ,
> что распакован в `ref/Lakka-Allwinner.orangepi_lite.arm-2.3.1.image`).
> Плата питaется, WiFi **работает** (wlan0 UP, link ready).

## Доступ

- SSH: `root@10.72.75.55`, пароль `root` (включён в настройках Lakka).
- Ядро: `Linux Lakka 4.14.18 #1 SMP Fri Sep 13 14:36:07 CEST 2019 armv7l`.
- UART (COM13): консоль U-Boot; после старта ядра молчит (Lakka уводит
  приглашение на HDMI), зато паникует в drm/DMA — для замеров использовать SSH.

## 1. Драйвер

- Модуль: **`8189fs v4.3.24_15589.20151023`** — В ТОЧНОСТИ наша версия
  (`tools/wifi/rtl8189fs/include/rtw_version.h: DRIVERVERSION v4.3.24_15589.20151023`).
- Прошивка: `/lib/firmware/rtlwifi/rtl8188efw.bin` = **11216 байт** = наш
  `fw_nic.bin` (0x2BD0). Для 8189F используется именно 8188E-FW.
- Интерфейс: `wlan0`, MAC `02:81:38:27:88:cd` (локально-администрируемый —
  как и мы генерируем).

## 2. SDIO-шина mmc1 (debugfs, ЖИВЫЕ значения)

`/sys/kernel/debug/mmc1/mmc1:0001/ios`:

| Параметр | Значение | Наш bare-metal (до W.234) | Расхождение |
|---|---|---|---|
| clock | **50000000 Hz** (50 МГц) | 6 МГц (24М/2/2) | в 8 раз ниже |
| actual clock | 50000000 Hz | — | — |
| bus width | **2 (4 bits)** | 1-bit (4-bit отключён) | 1 vs 4 |
| timing spec | **2 (sd high-speed)** | нет HS | HS не включали |
| vdd | 21 (3.3~3.4V) | 3.3V | ок |
| power mode | 2 (on) | — | ок |
| signal voltage | 0 (3.30V) | 3.3V | ок |

## 3. Тактирование (debugfs clk)

- `mmc1` rate = **50 МГц**; `mmc1_sample` = 50 МГц; `mmc1_output` = 50 МГц.
- `mmc1_sample/clk_phase` = **120°**; `mmc1_output/clk_phase` = **90°**.
- `clk_parent` = **200 МГц** (т.е. источник — PLL_PERIPH0/делитель, не OSC24M).
- `bus-mmc1` (AHB-обвязка) — рабочий.

Итог: `50 МГц = 200 МГц (parent) / 4`. Формула H3 sun8i mmc-клока:
`mmc_clk = parent / (N * M)` (подтверждено: 24М/2/16=750 кГц при N=2,M=16).
Для 50 МГц: **N=1, M=4** (200/4). Источник — `CLK_SRC_SEL=01 (PLL_PERIPH0)`.

Фазы для 50 МГц (в нашей таблице `sdc_prog_mmc_phase`): **output=90°, sample=120°**
— совпадает со снятым эталоном (строка `>25 МГц` нашей функции).

## 4. Что это значит для нашего кода

1. **Такт**: `H3_CCU_SDMMC1_CLK = SCLK_GATE | SRC_PLLPERI0 | N=1 | M=4` → 50 МГц.
2. **Фазы**: `sdc_prog_mmc_phase(50e6)` → out=90, smp=120.
3. **Ширина**: 4-bit (CCCR 0x07=0x02 + CTYPE=1) на всю работу (эталон — 4-bit).
4. **HS**: `timing sd-high-speed` — для SDIO HS включается через CCCR 0x13
   (SDR/high-speed) — проверить/включить.
5. Раньше наш «4-bit ломал data-фазу» — вероятно, из-за такта 6 МГц/неверных
   фаз; на 50 МГц + правильные фазы работает в Linux. Подлежит повторной проверке.

## 5. Ссылки

- `sunxi-mmc-4.14.c` — драйвер хоста (полный, у нас в tools/wifi/rtl8189fs).
- `ref/Lakka-Allwinner.orangepi_lite.arm-2.3.1.image` — образ (dtb, SYSTEM).
- `tools/wifi/W233-SDIO-TRACE.md` — наш полный поадресный трейс (для сверки).

## 6. Живая CAM-раскладка и REG_SECCFG (добавлено 2026-10-20)

Снято с той же платы по SSH (10.72.75.55), подключена к той же сети
**A05s пользователя М** (BSSID `9a:4d:56:7e:81:4f`, ch1, WPA2-PSK).
Файлы: `docs/lakka-live/LAKKA-CAM-SECURITY.md`, `docs/lakka-live/LAKKA-MACREG-FULL.txt`.

### CAM (HW, /proc/net/rtl8189fs/wlan0/sec_cam)

```
id ctrl   addr              kid type MK GK valid
 4 0x8010 9a:4d:56:7e:81:4f   0 AES  0  0  1   <- pairwise (BSSID)
 5 0x8051 9a:4d:56:7e:81:4f   1 AES  0  1  1   <- group GTK (BSSID!, keyid=1)
```

- **pairwise** → CAM entry **4**, `ctrl = BIT15|BIT6?0|AES<<2|keyid0`, **addr = BSSID**.
- **group (GTK)** → CAM entry **5** (не 1, не 0), `ctrl = 0x8051` (`BIT15|BIT6|AES<<2|1`),
  **addr = BSSID** (НЕ null!).
- Остальные 0..15 — `ctrl=0`, invalid (мусор/остатки, чип их игнорирует).

### REG_SECCFG

```
reg_scr = 0x010c  (SCR_CHK_KEYID BIT8 | SCR_RxDecEnable BIT3 | SCR_TxEncEnable BIT2)
```

- **Без** `SCR_CHK_BMC` (BIT9), **без** `TXBCUSEDK/RXBCUSEDK` (BIT6/BIT7) — для
  WPA2-PSK (802.1X) их в эталоне НЕТ.
- `sec_info: hw_decrypted=1, aes_sw_dec_cnt=0` — RX-декрипт аппаратный.

### Вывод для bare-metal (W.537)

1. `wpa_cam_write_key(4, bssid, ptk+32, 0)` — pairwise (у нас так).
2. `wpa_cam_write_key(5, bssid, gk, 1)` — **GTK в slot 5 с MAC=BSSID** (а не 1+null).
3. `REG_SECCFG = 0x010C` (16-бит), а не 0xCC/0x30C/0x34C.
4. Прежняя запись GTK в entry 1 с null-MAC не находилась чипом при RX
   (`cam_cache_chk` сверяет MAC) → `swdec=1` на broadcast.

## 7. TX-путь: живые замеры (добавлено 2026-10-20, W.555-W.556)

Снято с той же платы (Lakka 2.3.1, SSH 10.72.75.55), подключена к A05s
(BSSID 9a:4d:56:7e:81:4f, ch1, WPA2). Послужило основой для фиксов W.555/W.556.

### REG_FWHW_TXQ_CTRL (0x0420) — КОРЕНЬ зависания TX

```
0x420 = 0xff311f00   (bit7 = EN_AMPDU_RTY_NEW = 1)
```

- **W.556**: наш код ставил `EN_AMPDU_RTY_NEW` (BIT7) в **0x0818** (неверно!),
  а эталон — в **0x0420** (`_InitRetryFunction`, sdio_halinit.c:632-642).
  Пока бит не стоял, 3-страничные кадры (DHCP) висели в LOQ вечно;
  после исправления — `loq` возвращается, очереди пустеют (лог W.555).

### RQPN (0x0200) — резерв страниц TX

```
0x200 = 0x00E7020C   (HPQ=0x0C/12, LPQ=0x02/2, PUBQ=0xE7/231, без LD_RQPN после инициализации)
```

- LPQ у эталона = **2 страницы** (мы ставим 8 — отклонение-страховка, не критично).
- DHCP (3 стр) у эталона проходит через **PUBQ** (проверка `rtw_hal_sdio_query_tx_freepage`
  = `FreePage[PageIdx] + FreePage[PUBLIC]`).

### Регистры очередей TX (idle, после DHCP)

```
0x400 = 0x017f80ff   (Q0)
0x404 = 0x007f80ff   (Q1)
0x408 = 0x007f80ff   (Q2/BE)
0x40C = 0x007f80ff   (Q3)
0x410 = 0x007f80ff   (MGQ)
0x414 = 0x007f80ff   (HGQ)
0x418 = 0x0fff00f8   (BCNQ)
0x41C = 0x000000f8   (CPU_MGQ)
0x41A = 0x0fff00f8   (TXPKT_EMPTY, 16-bit: 0x00F8)
```

- Наша прошивка после фикса W.555 даёт **идентичные** значения (q0..7 в логе W.555).
- До фикса (W.552): `0x41A = 0x00F80FFE` (bit0=0 — LOQ не пуст, кадр висит).

### TX rate и MACID (живая Lakka)

```
current_tx_rate: id0 9a:4d:56:7e:81:4f MCS7   (0x13 — используем для DHCP, W.556)
macid_info:  id 0 bmc=0 mac=9a:4d:56:7e:81:4f C AP
             id 1 bmc=1 mac=ff:ff:ff:ff:ff:ff RSVD
```

### SDIO FREE_TXPG (sdio_local 0x20)

```
0x20 = 0x0002000c → [0]=HIQ=0x0c(12), [2]=MIDQ=0x02(2)
0x24 = 0x00e70002 → [4]=LOWQ=0x02(2), [6]=PUBQ=0xe7(231)
```

- Формат `HalQueryTxBufferStatus8188FSdio` (sdio_ops.c:2273): чётные байты 8-байтного
  блока = HIQ[0], MIDQ[2], LOWQ[4], PUBQ[6] (16-bit LE).

======================================================================
# ДОКУМЕНТ: rtl8189fs-live-snapshot.md
======================================================================

# Эталон приёма живого чипа RTL8189FTV (Lakka, wlan0)

Источник: Lakka 192.168.1.15, подключена через САМ чип RTL8189FTV (SSH идёт через wlan0).
Драйвер 8189fs v4.3.24_15589. Прошивка/ядро 4.14.18. Дата снятие: 2026-10-16.
Чтение регистров: `/proc/net/rtl8189fs/wlan0/read_reg` (формат "addr len", 4=32-бит).

## Режим: STA ассоциирован (RX работает)
SSID=MTS_GPON_9290e0 (mac 54:c2:50:92:90:e1), канал 4, BW20.
RSSI -50..-53 дБм, signal_strength 47, signal_qual 100.

## Beacon в эфире (survey_info, реально принимаются)
канал 1: 7 точек; канал 4: 4; канал 6: 2; канал 7: 1; канал 8: 3; канал 9: 3; канал 11: присутствует.
Сигнал на ch1: -58..-76, ch4: -50..-68, ch6: -60..-75, ch8: -53..-68, ch9: -17..-62.
=> Эфир ПОЛОН, beacon реально идут на каналах, которые сканирует bare-metal (1/6/11).

## Ключевые регистры (живой чип vs наш bare-metal)
| Адрес | Имя | Живой чип (RX ОК) | Наш bare-metal | Примечание |
|---|---|---|---|---|
| 0x100 | REG_CR | 0x000206ff | 0x63F | живой вкл bit17 (NETTYPE_AP), наш RMW 6,7,8,9 |
| 0x102 | MSR | 0x06ff0002 (мл.байт=0x02 MSR_INFRA) | 0x00 (NOLINK) | жив чип в STA |
| 0x110 | | 0 | 0xF1 | |
| 0x114 | | 0x3f7f0ff8 | 0xF1 | границы |
| 0x116 | rxff_bndy | 0x0ff83f7f | 0x3EFF | RX DMA boundary (32-бит) |
| 0x284 | RXPKT_NUM | 0x00020000 | 0x00020000 | RXDMA_IDLE set, пакетов в буфере нет по простоял |
| 0x608 | RCR | 0x700060ce | 0x7000604e | **разница бит 0x80 = CBSSID_BCN** (живой взведён, мы сняли) |
| 0x606 | | 0x30000000 | | |
| 0x290 | RXDMA_MODE | 0x0e | 0x0e | совпадает |
| 0x280 | RXDMA_AGG | 0x2000010f | 0x010f | живой старший байт 0x20 |
| 0x4c7 | | 0x4034080 | 0x80 | |
| 0x60c | | 0x4000418 | 0x18 | |
| 0x18 | MAC (не HISR sdio!) | 0x00400100 | | это MAC рег 0x18 |

## SIFS / EDCA / режимы (живой)
0x603 BWOPMODE=0x00, 0x428=0x3030100a, 0x63a=0x2828100a, 0x514=0x100a100a,
0x516=0x100a100a, 0x42a=0x100a3030, 0x508=0x5ea42b, 0x50c=0x0a44f, 0x504=0x5e4322,
0x500=0x2f3222, 0x638=0x100a2828, 0x55c=0x0f0fff28, 0x450=0x3ff000,
0x452=0xf000003f, 0x454=0xf6700005, 0x456=0x5f670.

## sdio_local_reg_dump (живой)
0x00 0x00000010  0x00002ee0  0 ... 
0x10 0x00000000  0x00000001  0x60800010  0   <- 0x14(HIMR if local base?) байт... 
0x20 0x0002000c  0x00e70002  0x20200000  0 
0x30 0x096a7fd0  0 ...
0x40 0x00000000  0x042374c8  ...
0x80 0x00000007  0x00120100  ...
0xc0 0x00000001 ...
Примечание: "sdio_local" dump читается как 32-бит слова начиная с 0x00; 0x14(б2-3)? В нашей коди: HIMR(0x14)=бит RX_REQUEST, HISR(0x18). В живом дампе 0x10 слов=0x00000000, 0x14(след.слово)=0x00000001 => HIMR bit0=1 (RX_REQUEST вкл). 0x18=0x60800010 (HISR активные биты).

## Полные дампы живого чипа (BB/RF/SDIO)
=====BB_BEGIN=====
======= BB REG =======
0x800 0x83045700  0x00000001  0x0000fccd  0x0000000a 
0x810 0x10001331  0x020c3d10  0x00200385  0x00000000 
0x820 0x01000100  0xa1390204  0x00000000  0x0c000000 
0x830 0x00000000  0x00000000  0x00000000  0x00000000 
0x840 0x042374c8  0x00000000  0x00000000  0x00000000 
0x850 0x00030000  0x00000000  0x569a569a  0x569a569a 
0x860 0x00100130  0x00000000  0x00000000  0x30303000 
0x870 0x00000000  0x25004000  0x00000808  0x004f0201 
0x880 0xb0000b1e  0x00000007  0x00000000  0xccc000c0 
0x890 0xffff0900  0xc350fffe  0xffffff50  0xffffffff 
0x8a0 0x00100000  0x00000000  0x0000010e  0x00001c80 
0x8b0 0x00000000  0x000000e9  0x001074c8  0x00000000 
0x8c0 0x0384c88d  0x000000f6  0xb2b3b316  0xadb0b8b4 
0x8d0 0x00000000  0x00000000  0x00000000  0x00000000 
0x8e0 0x00000014  0x00000000  0xf8080000  0x31313131 
0x8f0 0x31313131  0x00000000  0x000000d6  0x00000000 
0x900 0x00000000  0x00000023  0x00000208  0x81121111 
0x910 0x00000002  0x00000201  0x00000000  0x00000000 
0x920 0x00000000  0x00000000  0x00000000  0x00000000 
0x930 0x00000077  0x00000000  0x00000000  0x00000000 
0x940 0x00000000  0x00000003  0x99000000  0x00000010 
0x950 0x20003000  0x4a300000  0x4bc5d87a  0x04eb9b79 
0x960 0x00000000  0x00000000  0x00000000  0x00000003 
0x970 0x00000000  0x00000000  0x00000000  0x00000000 
0x980 0x00000000  0x00000000  0x00000000  0x00000000 
0x990 0x00000000  0x00000000  0x00000000  0x00000000 
0x9a0 0xd0001001  0xd0001001  0xd0000001  0x00000000 
0x9b0 0x00000000  0x00000000  0x00000000  0x00000000 
0x9c0 0x000016f1  0x00000000  0x00000000  0x003d003d 
0x9d0 0x00010001  0x00000000  0x00000000  0x00000000 
0x9e0 0x00000000  0x00000000  0x00000000  0x00000000 
0x9f0 0x00000000  0x00000000  0x00000000  0x00000000 
0xa00 0x00d047c8  0x80ff800c  0x8c038300  0x2e7f120f 
0xa10 0x9500bb78  0x1114d028  0x00881117  0x89140f00 
0xa20 0xd1d80000  0x5a7da0bd  0x0000223b  0x00d3a000 
0xa30 0x00000000  0x00000000  0x00000000  0x00000000 
0xa40 0x00000000  0x00000000  0x00000000  0x00000000 
0xa50 0x3100b300  0x003097ff  0x01005881  0x00000006 
0xa60 0x80801703  0x03170106  0x00000000  0x00000000 
0xa70 0x101fbf00  0x00000007  0x00008900  0x225b0606 
0xa80 0x218075b1  0x00120000  0x040c0000  0x12345678 
0xa90 0xabcdef00  0x001b1b89  0x05100000  0x3f000000 
0xaa0 0x00000000  0x00000000  0x00000000  0x00000000 
0xab0 0x00000000  0x00000000  0x00000000  0x00000000 
0xac0 0x00000000  0x00000000  0x00000000  0x00000000 
0xad0 0x00000000  0x00000000  0x00000000  0x00000000 
0xae0 0x00000000  0x00000000  0x00000000  0x00000000 
0xaf0 0x00000000  0x00000000  0x00000000  0x00000000 
0xb00 0x00000000  0x00000000  0x00000000  0x00000000 
0xb10 0x00000000  0x00000000  0x00000000  0x00000000 
0xb20 0x00000000  0x00000000  0x00000000  0x00000000 
0xb30 0x00000000  0x00000000  0x00000000  0x00000000 
0xb40 0x00000000  0x00000000  0x00000000  0x00000000 
0xb50 0x00000000  0x00000000  0x00000000  0x00000000 
0xb60 0x00000000  0x00000000  0x00000000  0x00000000 
0xb70 0x00000000  0x00000000  0x00000000  0x00000000 
0xb80 0x00000000  0x00000000  0x00000000  0x00000000 
0xb90 0x00000000  0x00000000  0x00000000  0x00000000 
0xba0 0x00000000  0x00000000  0x00000000  0x00000000 
0xbb0 0x00000000  0x00000000  0x00000000  0x00000000 
0xbc0 0x00000000  0x00000000  0x00000000  0x00000000 
0xbd0 0x00000000  0x00000000  0x04100000  0x00000000 
0xbe0 0x00000000  0x00000000  0x0001ff8f  0x00000000 
0xbf0 0x00000000  0x00000000  0x00000000  0x00000000 
0xc00 0x48071d40  0x03a05611  0x000000e4  0x6c6c6cec 
0xc10 0x18800000  0x4000f0fd  0x08800000  0x40000100 
0xc20 0x00000000  0x00000000  0x00000000  0x00000000 
0xc30 0x69e9cc4a  0x31000040  0x21688080  0x00001714 
0xc40 0x0b78403f  0x00010036  0xec020107  0xa03e0346 
0xc50 0x6955342d  0x43bc0094  0x00013169  0x00250492 
0xc60 0x00000000  0x7112848b  0x47c07bff  0x00000036 
0xc70 0x2c7f000d  0x020600db  0x003f0001  0x00b91612 
0xc80 0x43c20113  0x21f60000  0x40000100  0x20200000 
0xc90 0x00091521  0x00000000  0x00121820  0x00007f7f 
0xca0 0xf0000000  0x000300a0  0x00000000  0x00000000 
0xcb0 0x00000000  0x00000000  0x00000000  0x28000000 
0xcc0 0x00000000  0x00000000  0x00000000  0x00000000 
0xcd0 0x00000000  0x00000000  0x64b22427  0x00766932 
0xce0 0x00222222  0x10000000  0x37644302  0x2f97d40c 
0xcf0 0x00030000  0x002f0000  0x00000000  0x00000000 
0xd00 0x04030740  0x40020401  0x0000907f  0x20010201 
0xd10 0xa0633333  0x3333bc53  0x7a8f5b6f  0x00000000 
0xd20 0x00000000  0x00000000  0x00000000  0xcb979175 
0xd30 0x00000000  0x80608000  0x98000000  0x40127353 
0xd40 0x06000000  0x00000000  0x00000000  0x00000000 
0xd50 0x6437140a  0x00000000  0x00000282  0x30032064 
0xd60 0x4653de68  0x04518a3c  0x00002101  0x2a201c16 
0xd70 0x00000000  0x00000000  0x00000000  0x00000000 
0xd80 0x00000000  0x00000000  0x00000000  0x00000000 
0xd90 0x00000000  0x00000000  0x00000000  0x0050009d 
0xda0 0x005000f6  0x0006005d  0x00000005  0xffec0000 
0xdb0 0xffb80000  0xffb8e000  0x00030000  0x1fbd0000 
0xdc0 0x00000000  0x03f10000  0x00000331  0x00000002 
0xdd0 0x0000001d  0x0000003c  0x00dc0923  0x000077d6 
0xde0 0x1fb80000  0x00000000  0x000201c9  0x00000000 
0xdf0 0x00000000  0x00d5803e  0x00000005  0x00000000 
0xe00 0x31313131  0x31313131  0x0390302d  0x00000000 
0xe10 0x2f2f2f2f  0x2f2f2f2f  0x2d2d2d2d  0x2d2d2d2d 
0xe20 0xc0000000  0x00100000  0x000000ff  0x00000000 
0xe30 0x01008c00  0x01008c00  0x82160000  0x281613ff 
0xe40 0x810a7c02  0x01004800  0xf8000000  0x0046a911 
0xe50 0x1000dc1f  0x10008c1f  0x02140102  0x28160d05 
0xe60 0x021400a0  0x281600a0  0x01c00056  0x01c00010 
0xe70 0x01c00010  0x02000010  0x02000010  0x02000010 
0xe80 0x02000010  0x01c00010  0x02000010  0x01c00010 
0xe90 0x000c57c8  0x010a0000  0x000011e4  0x00020000 
0xea0 0x001a8354  0x00fd0000  0x00005c90  0x07fc3000 
0xeb0 0x00000000  0x00000000  0x00000000  0x00000000 
0xec0 0x00000000  0x00000000  0x00000000  0x00000000 
0xed0 0x01c00010  0x01c00010  0x01c00010  0x00000010 
0xee0 0x00000010  0x015e5448  0x21555448  0x03c00010 
0xef0 0x83045700  0x00000001  0x00000000  0x00000000 
0xf00 0x00000300  0x00000000  0x00000000  0x00000000 
0xf10 0x00000000  0x00000003  0x00000000  0x00000000 
0xf20 0x00000000  0x00000000  0x00000000  0x00000000 
0xf30 0x00000000  0x00000000  0x00000000  0x00000000 
0xf40 0x00000000  0x00000000  0x00000000  0x00000000 
0xf50 0x00000000  0x00000000  0x00000000  0x00000000 
0xf60 0x00000000  0x00000000  0x00000000  0x00000000 
0xf70 0x00000000  0x00000000  0x00000000  0x00000000 
0xf80 0x000006cd  0x00003cce  0x0000ffff  0x00000000 
0xf90 0x07950166  0x7ec75795  0x00260001  0x00000000 
0xfa0 0xffff0000  0x0002e44a  0x00008607  0x4000501e 
0xfb0 0x00a0000a  0x00408f8f  0x00ad03df  0x00000000 
0xfc0 0x00000000  0x00000000  0x00000000  0x00000000 
0xfd0 0x00000000  0x00000000  0x00000000  0x00000000 
0xfe0 0x00000000  0x00000000  0x00000000  0x00000000 
0xff0 0x00000000  0x00000000  0x00000000  0x00000000 
=====BB_END=====
=====RF_BEGIN=====
======= RF REG =======
RF_Path(0)
0x00  0x000356b7  0x00000000  0x00000000  0x00000000 
0x04  0x00000000  0x00000000  0x00000000  0x00000000 
0x08  0x0000f800  0x00000000  0x00000000  0x00000000 
0x0c  0x00000000  0x00000000  0x00000000  0x00000000 
0x10  0x00000000  0x00000000  0x00000000  0x00000000 
0x14  0x00000000  0x00000000  0x00000000  0x00000000 
0x18  0x00000c04  0x00072d70  0x00058000  0x00000c6c 
0x1c  0x00000000  0x00080000  0x00000000  0x00000000 
0x20  0x00000000  0x00000000  0x00000000  0x00000000 
0x24  0x00000000  0x00000000  0x00000000  0x00000000 
0x28  0x00000000  0x00000000  0x00000000  0x00000000 
0x2c  0x00000000  0x00000000  0x00000000  0x0001e280 
0x30  0x00030000  0x0000000f  0x000f7ff2  0x00000000 
0x34  0x00000c48  0x00000290  0x00019064  0x00000000 
0x38  0x00000000  0x00000000  0x00000000  0x00010400 
0x3c  0x00000000  0x00000000  0x00000000  0x00028000 
0x40  0x00000000  0x00000000  0x000074c8  0x00000000 
0x44  0x00000000  0x00000000  0x00000000  0x00000000 
0x48  0x00000000  0x00000000  0x00000000  0x00000000 
0x4c  0x00000000  0x00000000  0x00000000  0x00000000 
0x50  0x00000000  0x000e8231  0x000fac88  0x00000141 
0x54  0x00015007  0x00082060  0x00051cf1  0x000d0000 
0x58  0x000c0160  0x00000000  0x00048000  0x00000000 
0x5c  0x00000000  0x00000000  0x00000000  0x00000000 
0x60  0x00000000  0x00000000  0x00000000  0x00000000 
0x64  0x00000000  0x00000000  0x00000000  0x00001552 
0x68  0x00000000  0x00000000  0x00000000  0x00000000 
0x6c  0x00000000  0x00000000  0x00000000  0x00000000 
0x70  0x00000000  0x00000000  0x00000000  0x00000000 
0x74  0x00000000  0x00000000  0x00000000  0x00000000 
0x78  0x00000000  0x00000000  0x00000000  0x00000000 
0x7c  0x00000000  0x00000000  0x00000000  0x00000082 
0x80  0x0000000e  0x0003fc00  0x0000f042  0x00000000 
0x84  0x00048000  0x00000000  0x0000002a  0x00000065 
0x88  0x00000000  0x00000000  0x00000000  0x00000000 
0x8c  0x00000000  0x00000000  0x00065540  0x00088000 
0x90  0x00000000  0x00000000  0x00000000  0x00000000 
0x94  0x00000000  0x00000000  0x00000000  0x00000000 
0x98  0x00000000  0x00000000  0x00000000  0x00000000 
0x9c  0x00000000  0x00000000  0x00000000  0x00000000 
0xa0  0x00000000  0x00000000  0x00000000  0x00000000 
0xa4  0x00000000  0x00000000  0x00000000  0x00000000 
0xa8  0x00000000  0x00000000  0x00000000  0x00000000 
0xac  0x00000000  0x00000000  0x00000000  0x00000000 
0xb0  0x000ffff0  0x00022218  0x00034c00  0x00000000 
0xb4  0x000c484b  0x0000112a  0x00000540  0x00010408 
0xb8  0x00010200  0x00080001  0x00040001  0x00000400 
0xbc  0x00000000  0x00000000  0x00000000  0x000c0000 
0xc0  0x00000000  0x00000000  0x00002400  0x00000009 
0xc4  0x0004207b  0x00013b13  0x0000008d  0x0008f820 
0xc8  0x00076c06  0x00000000  0x00000000  0x00000000 
0xcc  0x00000000  0x00000000  0x00000000  0x00000000 
0xd0  0x00000000  0x00000000  0x00000000  0x00000000 
0xd4  0x00000000  0x00000000  0x00000000  0x00000000 
0xd8  0x00000000  0x00000000  0x00000000  0x00000000 
0xdc  0x00000000  0x00000000  0x00000000  0x00000140 
0xe0  0x00000000  0x00000000  0x00000000  0x00000000 
0xe4  0x00000000  0x00000000  0x00000000  0x00000000 
0xe8  0x00000000  0x00000000  0x00000000  0x00000000 
0xec  0x00000000  0x00000000  0x00000000  0x00080000 
0xf0  0x00008000  0x00032b35  0x0003a148  0x00037500 
0xf4  0x00030000  0x00000800  0x00000000  0x00000000 
0xf8  0x00000000  0x00000000  0x00000000  0x00000000 
0xfc  0x00000000  0x00000000  0x00000000  0x00000000 
=====RF_END=====
=====SDIO_BEGIN=====
0x00 0x00000010  0x00002ee0  0x00000000  0x00000000 
0x10 0x00000000  0x00000001  0x60800010  0x00000000 
0x20 0x0002000c  0x00e30002  0x20200000  0x00000000 
0x30 0x086e7ed4  0x00000000  0x00000000  0x00000000 
0x40 0x00000000  0x00100000  0x00000000  0x00000000 
0x50 0x00000000  0x00000000  0x00000000  0x00000000 
0x60 0x00000000  0x00000000  0x00000000  0x00000000 
0x70 0x00000000  0x00000000  0x00000000  0x00000000 
0x80 0x00000007  0x00120100  0x0000000a  0x00000000 
0x90 0x00000000  0x00000000  0x00000000  0x00000000 
0xa0 0x00000000  0x00000000  0x00000000  0x00000000 
0xb0 0x00000000  0x00000000  0x00000000  0x00000000 
0xc0 0x00000001  0x00000000  0x00000000  0x00000000 
0xd0 0x00000000  0x00000000  0x00000000  0x00000000 
0xe0 0x00000000  0x00000000  0x00000000  0x00000000 
0xf0 0x00000000  0x00000000  0x00000000  0x00000000 
=====SDIO_END=====
=====F0_BEGIN=====
0x00 32 02 02 02 03 00 00 02 	13 00 10 00 00 00 00 00 
0x10 00 00 01 03 07 00 01 00 	00 00 00 00 00 00 00 00 
0x20 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x30 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x40 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x50 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x60 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x70 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x80 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x90 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xa0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xb0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xc0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xd0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xe0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xf0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
=====F0_END=====

## Финальный набор регистров приёма (живой чип, STA ch4)
0x100=206ff, 0x102=6ff0002, 0x10c=f5b4, 0x280=2000010f, 0x284=20000(RXDMA_IDLE),
0x290=e, 0x608=700060ce, 0x60a=60ce7000, 0x60c=4000418, 0x4ca=8ff0c1f,
0x4c7=4034080, 0x510=2800660f, 0x420=ff311f00, 0x444=10, 0x603=0, 0x116=ff83f7f(32-бит rxff_bndy).

## Параметры
rf_info cur_ch=4 bw=0, fwstate=0x9, mlmext_state=0x4402, tx_rate MCS7, rx rssi=-55.

## TRX / MAC-Q / F0 / HAL (живой чип)
=====TRX_BEGIN=====
os_queue[0]:waked
os_queue[1]:waked
os_queue[2]:waked
os_queue[3]:waked
free_xmitbuf_cnt=16, free_xmitframe_cnt=256
free_ext_xmitbuf_cnt=32, free_xframe_ext_cnt=32
free_recvframe_cnt=256
0, hwq.accnt=0
1, hwq.accnt=0
2, hwq.accnt=0
3, hwq.accnt=0
Rx: Counts of Packets Whose Seq_Num Less Than Reorder Control Seq_Num: 0
Rx: Reorder Time-out Trigger Counts: 0
Rx: Packet Loss Counts: 0
Rx: Duplicate Management Frame Drop Count: 0
Rx: AMPDU BA window shift Count: 0
Rx: Conflict MAC Address Frames Count: 0
=====TRX_END=====
=====MACQ_BEGIN=====
Q0 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:2
Q1 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:2
Q2 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q3 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q4 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q5 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q6 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q7 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
MG head:0xff, tail:0xff, pkt_num:0, macid:1, ac:0
HI head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
BCN head:0xf8, pkt_num:0
=====MACQ_END=====
=====F0_BEGIN=====
0x00 32 02 02 02 03 00 00 02 	13 00 10 00 00 00 00 00 
0x10 00 00 01 03 07 00 01 00 	00 00 00 00 00 00 00 00 
0x20 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x30 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x40 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x50 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x60 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x70 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x80 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x90 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xa0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xb0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xc0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xd0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xe0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xf0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
=====F0_END=====
=====HAL_BEGIN=====
macid_num:16
sec_cap:0x00
sec_cam_ent_num:16
nss_num:1
band_cap:2G 
bw_cap:20M 40M 
wl_func:P2P MIRACAST TDLS 
=====HAL_END=====

## Свежий survey приёма (живой чип, RSSI -55, qual 96)
20+ BSSID, beacon реально принимаются на каналах 1,4,6,7,8,9,11.
ch1: midea_b8(-58), MGTS_GPON_d23898(-70), MTS_GPON_b4d378(-62)...
ch4: MTS_GPON_9290e0(-52, ассоциирован), MTS_GPON_929638(-66), MTS_GPON_9914(-68)...
ch6: Beeline_2G_F5554F(-59), MGTS_GPON_38AD(-61).
ch8: Netcraze-4479(-62), ch9: VIOL(-67), ch11: MGTS_GPON_C046(-64)...
=> эфир ПОЛОН на всех каналах скана bare-metal (1/6/11), сигнал до -52 дБм.

## H2C / HMETFR живого чипа (ПОЗДНЕЕ снятие, RSSI -56, канал 4)
REG_HMETFR 0x1CC = 0 (firmware свободна, читает H2C).
H2C-боксы (заполнены драйвером, все id=0x42 RSSI_SETTING в мл. байте):
  0x1D0 = 0x2e000042, 0x1D4 = 0x2a000042, 0x1D8 = 0x2c000042, 0x1DC = 0x2a000042
  EX-боксы: 0x1F0 = 0x4, 0x1F4 = 0x5
=> H2C SCAN_ENABLE(0x02) в живой Lakka НЕ используется (в боксах RSSI_SETTING 0x42).
   Fwstate=0x9 (STA), scan_abort=0, cur_ch=4, rssi=-56.

## Скан-снимок живого (фоновый скан STA, fwstate 0x809 = WIFI_SITE_MONITOR)
При фоновом скане: RCR 0x608=0x700060ce (CBSSID_BCN 0x80 взведён), MSR 0x102=0x06ff0002, scan_abort менялся 120/160.
Драйвер hw_var_set_mlme_sitesurvey (rtl8188f_hal_init.c:5557) при sitesurvey СНИМАЕТ RCR_CBSSID_BCN (0x80),
т.е. скан-набор bare-metal 0x7000604e корректен. RCR НЕ причина мёртвого RX_REQUEST.

## Вывод для bare-metal (W.318+
Пробуждение firmware работает (rpwm=0xC0 PS_ACK, cpwm=0x80), H2C-формат/адресация 0x1CC=device8 верна,
но у нас 0x1CC=1 (firmware не забирает H2C), на живом 0x1CC=0. H2C SCAN_ENABLE в скане не нужен.


======================================================================
# ДОКУМЕНТ: rtl8189fs-live-snapshot-big.md
======================================================================

# Эталон приёма живого чипа RTL8189FTV (Lakka, wlan0)

Источник: Lakka 192.168.1.15, подключена через САМ чип RTL8189FTV (SSH идёт через wlan0).
Драйвер 8189fs v4.3.24_15589. Прошивка/ядро 4.14.18. Дата снятие: 2026-10-16.
Чтение регистров: `/proc/net/rtl8189fs/wlan0/read_reg` (формат "addr len", 4=32-бит).

## Режим: STA ассоциирован (RX работает)
SSID=MTS_GPON_9290e0 (mac 54:c2:50:92:90:e1), канал 4, BW20.
RSSI -50..-53 дБм, signal_strength 47, signal_qual 100.

## Beacon в эфире (survey_info, реально принимаются)
канал 1: 7 точек; канал 4: 4; канал 6: 2; канал 7: 1; канал 8: 3; канал 9: 3; канал 11: присутствует.
Сигнал на ch1: -58..-76, ch4: -50..-68, ch6: -60..-75, ch8: -53..-68, ch9: -17..-62.
=> Эфир ПОЛОН, beacon реально идут на каналах, которые сканирует bare-metal (1/6/11).

## Ключевые регистры (живой чип vs наш bare-metal)
| Адрес | Имя | Живой чип (RX ОК) | Наш bare-metal | Примечание |
|---|---|---|---|---|
| 0x100 | REG_CR | 0x000206ff | 0x63F | живой вкл bit17 (NETTYPE_AP), наш RMW 6,7,8,9 |
| 0x102 | MSR | 0x06ff0002 (мл.байт=0x02 MSR_INFRA) | 0x00 (NOLINK) | жив чип в STA |
| 0x110 | | 0 | 0xF1 | |
| 0x114 | | 0x3f7f0ff8 | 0xF1 | границы |
| 0x116 | rxff_bndy | 0x0ff83f7f | 0x3EFF | RX DMA boundary (32-бит) |
| 0x284 | RXPKT_NUM | 0x00020000 | 0x00020000 | RXDMA_IDLE set, пакетов в буфере нет по простоял |
| 0x608 | RCR | 0x700060ce | 0x7000604e | **разница бит 0x80 = CBSSID_BCN** (живой взведён, мы сняли) |
| 0x606 | | 0x30000000 | | |
| 0x290 | RXDMA_MODE | 0x0e | 0x0e | совпадает |
| 0x280 | RXDMA_AGG | 0x2000010f | 0x010f | живой старший байт 0x20 |
| 0x4c7 | | 0x4034080 | 0x80 | |
| 0x60c | | 0x4000418 | 0x18 | |
| 0x18 | MAC (не HISR sdio!) | 0x00400100 | | это MAC рег 0x18 |

## SIFS / EDCA / режимы (живой)
0x603 BWOPMODE=0x00, 0x428=0x3030100a, 0x63a=0x2828100a, 0x514=0x100a100a,
0x516=0x100a100a, 0x42a=0x100a3030, 0x508=0x5ea42b, 0x50c=0x0a44f, 0x504=0x5e4322,
0x500=0x2f3222, 0x638=0x100a2828, 0x55c=0x0f0fff28, 0x450=0x3ff000,
0x452=0xf000003f, 0x454=0xf6700005, 0x456=0x5f670.

## sdio_local_reg_dump (живой)
0x00 0x00000010  0x00002ee0  0 ... 
0x10 0x00000000  0x00000001  0x60800010  0   <- 0x14(HIMR if local base?) байт... 
0x20 0x0002000c  0x00e70002  0x20200000  0 
0x30 0x096a7fd0  0 ...
0x40 0x00000000  0x042374c8  ...
0x80 0x00000007  0x00120100  ...
0xc0 0x00000001 ...
Примечание: "sdio_local" dump читается как 32-бит слова начиная с 0x00; 0x14(б2-3)? В нашей коди: HIMR(0x14)=бит RX_REQUEST, HISR(0x18). В живом дампе 0x10 слов=0x00000000, 0x14(след.слово)=0x00000001 => HIMR bit0=1 (RX_REQUEST вкл). 0x18=0x60800010 (HISR активные биты).

## Полные дампы живого чипа (BB/RF/SDIO)
=====BB_BEGIN=====
======= BB REG =======
0x800 0x83045700  0x00000001  0x0000fccd  0x0000000a 
0x810 0x10001331  0x020c3d10  0x00200385  0x00000000 
0x820 0x01000100  0xa1390204  0x00000000  0x0c000000 
0x830 0x00000000  0x00000000  0x00000000  0x00000000 
0x840 0x042374c8  0x00000000  0x00000000  0x00000000 
0x850 0x00030000  0x00000000  0x569a569a  0x569a569a 
0x860 0x00100130  0x00000000  0x00000000  0x30303000 
0x870 0x00000000  0x25004000  0x00000808  0x004f0201 
0x880 0xb0000b1e  0x00000007  0x00000000  0xccc000c0 
0x890 0xffff0900  0xc350fffe  0xffffff50  0xffffffff 
0x8a0 0x00100000  0x00000000  0x0000010e  0x00001c80 
0x8b0 0x00000000  0x000000e9  0x001074c8  0x00000000 
0x8c0 0x0384c88d  0x000000f6  0xb2b3b316  0xadb0b8b4 
0x8d0 0x00000000  0x00000000  0x00000000  0x00000000 
0x8e0 0x00000014  0x00000000  0xf8080000  0x31313131 
0x8f0 0x31313131  0x00000000  0x000000d6  0x00000000 
0x900 0x00000000  0x00000023  0x00000208  0x81121111 
0x910 0x00000002  0x00000201  0x00000000  0x00000000 
0x920 0x00000000  0x00000000  0x00000000  0x00000000 
0x930 0x00000077  0x00000000  0x00000000  0x00000000 
0x940 0x00000000  0x00000003  0x99000000  0x00000010 
0x950 0x20003000  0x4a300000  0x4bc5d87a  0x04eb9b79 
0x960 0x00000000  0x00000000  0x00000000  0x00000003 
0x970 0x00000000  0x00000000  0x00000000  0x00000000 
0x980 0x00000000  0x00000000  0x00000000  0x00000000 
0x990 0x00000000  0x00000000  0x00000000  0x00000000 
0x9a0 0xd0001001  0xd0001001  0xd0000001  0x00000000 
0x9b0 0x00000000  0x00000000  0x00000000  0x00000000 
0x9c0 0x000016f1  0x00000000  0x00000000  0x003d003d 
0x9d0 0x00010001  0x00000000  0x00000000  0x00000000 
0x9e0 0x00000000  0x00000000  0x00000000  0x00000000 
0x9f0 0x00000000  0x00000000  0x00000000  0x00000000 
0xa00 0x00d047c8  0x80ff800c  0x8c038300  0x2e7f120f 
0xa10 0x9500bb78  0x1114d028  0x00881117  0x89140f00 
0xa20 0xd1d80000  0x5a7da0bd  0x0000223b  0x00d3a000 
0xa30 0x00000000  0x00000000  0x00000000  0x00000000 
0xa40 0x00000000  0x00000000  0x00000000  0x00000000 
0xa50 0x3100b300  0x003097ff  0x01005881  0x00000006 
0xa60 0x80801703  0x03170106  0x00000000  0x00000000 
0xa70 0x101fbf00  0x00000007  0x00008900  0x225b0606 
0xa80 0x218075b1  0x00120000  0x040c0000  0x12345678 
0xa90 0xabcdef00  0x001b1b89  0x05100000  0x3f000000 
0xaa0 0x00000000  0x00000000  0x00000000  0x00000000 
0xab0 0x00000000  0x00000000  0x00000000  0x00000000 
0xac0 0x00000000  0x00000000  0x00000000  0x00000000 
0xad0 0x00000000  0x00000000  0x00000000  0x00000000 
0xae0 0x00000000  0x00000000  0x00000000  0x00000000 
0xaf0 0x00000000  0x00000000  0x00000000  0x00000000 
0xb00 0x00000000  0x00000000  0x00000000  0x00000000 
0xb10 0x00000000  0x00000000  0x00000000  0x00000000 
0xb20 0x00000000  0x00000000  0x00000000  0x00000000 
0xb30 0x00000000  0x00000000  0x00000000  0x00000000 
0xb40 0x00000000  0x00000000  0x00000000  0x00000000 
0xb50 0x00000000  0x00000000  0x00000000  0x00000000 
0xb60 0x00000000  0x00000000  0x00000000  0x00000000 
0xb70 0x00000000  0x00000000  0x00000000  0x00000000 
0xb80 0x00000000  0x00000000  0x00000000  0x00000000 
0xb90 0x00000000  0x00000000  0x00000000  0x00000000 
0xba0 0x00000000  0x00000000  0x00000000  0x00000000 
0xbb0 0x00000000  0x00000000  0x00000000  0x00000000 
0xbc0 0x00000000  0x00000000  0x00000000  0x00000000 
0xbd0 0x00000000  0x00000000  0x04100000  0x00000000 
0xbe0 0x00000000  0x00000000  0x0001ff8f  0x00000000 
0xbf0 0x00000000  0x00000000  0x00000000  0x00000000 
0xc00 0x48071d40  0x03a05611  0x000000e4  0x6c6c6cec 
0xc10 0x18800000  0x4000f0fd  0x08800000  0x40000100 
0xc20 0x00000000  0x00000000  0x00000000  0x00000000 
0xc30 0x69e9cc4a  0x31000040  0x21688080  0x00001714 
0xc40 0x0b78403f  0x00010036  0xec020107  0xa03e0346 
0xc50 0x6955342d  0x43bc0094  0x00013169  0x00250492 
0xc60 0x00000000  0x7112848b  0x47c07bff  0x00000036 
0xc70 0x2c7f000d  0x020600db  0x003f0001  0x00b91612 
0xc80 0x43c20113  0x21f60000  0x40000100  0x20200000 
0xc90 0x00091521  0x00000000  0x00121820  0x00007f7f 
0xca0 0xf0000000  0x000300a0  0x00000000  0x00000000 
0xcb0 0x00000000  0x00000000  0x00000000  0x28000000 
0xcc0 0x00000000  0x00000000  0x00000000  0x00000000 
0xcd0 0x00000000  0x00000000  0x64b22427  0x00766932 
0xce0 0x00222222  0x10000000  0x37644302  0x2f97d40c 
0xcf0 0x00030000  0x002f0000  0x00000000  0x00000000 
0xd00 0x04030740  0x40020401  0x0000907f  0x20010201 
0xd10 0xa0633333  0x3333bc53  0x7a8f5b6f  0x00000000 
0xd20 0x00000000  0x00000000  0x00000000  0xcb979175 
0xd30 0x00000000  0x80608000  0x98000000  0x40127353 
0xd40 0x06000000  0x00000000  0x00000000  0x00000000 
0xd50 0x6437140a  0x00000000  0x00000282  0x30032064 
0xd60 0x4653de68  0x04518a3c  0x00002101  0x2a201c16 
0xd70 0x00000000  0x00000000  0x00000000  0x00000000 
0xd80 0x00000000  0x00000000  0x00000000  0x00000000 
0xd90 0x00000000  0x00000000  0x00000000  0x0050009d 
0xda0 0x005000f6  0x0006005d  0x00000005  0xffec0000 
0xdb0 0xffb80000  0xffb8e000  0x00030000  0x1fbd0000 
0xdc0 0x00000000  0x03f10000  0x00000331  0x00000002 
0xdd0 0x0000001d  0x0000003c  0x00dc0923  0x000077d6 
0xde0 0x1fb80000  0x00000000  0x000201c9  0x00000000 
0xdf0 0x00000000  0x00d5803e  0x00000005  0x00000000 
0xe00 0x31313131  0x31313131  0x0390302d  0x00000000 
0xe10 0x2f2f2f2f  0x2f2f2f2f  0x2d2d2d2d  0x2d2d2d2d 
0xe20 0xc0000000  0x00100000  0x000000ff  0x00000000 
0xe30 0x01008c00  0x01008c00  0x82160000  0x281613ff 
0xe40 0x810a7c02  0x01004800  0xf8000000  0x0046a911 
0xe50 0x1000dc1f  0x10008c1f  0x02140102  0x28160d05 
0xe60 0x021400a0  0x281600a0  0x01c00056  0x01c00010 
0xe70 0x01c00010  0x02000010  0x02000010  0x02000010 
0xe80 0x02000010  0x01c00010  0x02000010  0x01c00010 
0xe90 0x000c57c8  0x010a0000  0x000011e4  0x00020000 
0xea0 0x001a8354  0x00fd0000  0x00005c90  0x07fc3000 
0xeb0 0x00000000  0x00000000  0x00000000  0x00000000 
0xec0 0x00000000  0x00000000  0x00000000  0x00000000 
0xed0 0x01c00010  0x01c00010  0x01c00010  0x00000010 
0xee0 0x00000010  0x015e5448  0x21555448  0x03c00010 
0xef0 0x83045700  0x00000001  0x00000000  0x00000000 
0xf00 0x00000300  0x00000000  0x00000000  0x00000000 
0xf10 0x00000000  0x00000003  0x00000000  0x00000000 
0xf20 0x00000000  0x00000000  0x00000000  0x00000000 
0xf30 0x00000000  0x00000000  0x00000000  0x00000000 
0xf40 0x00000000  0x00000000  0x00000000  0x00000000 
0xf50 0x00000000  0x00000000  0x00000000  0x00000000 
0xf60 0x00000000  0x00000000  0x00000000  0x00000000 
0xf70 0x00000000  0x00000000  0x00000000  0x00000000 
0xf80 0x000006cd  0x00003cce  0x0000ffff  0x00000000 
0xf90 0x07950166  0x7ec75795  0x00260001  0x00000000 
0xfa0 0xffff0000  0x0002e44a  0x00008607  0x4000501e 
0xfb0 0x00a0000a  0x00408f8f  0x00ad03df  0x00000000 
0xfc0 0x00000000  0x00000000  0x00000000  0x00000000 
0xfd0 0x00000000  0x00000000  0x00000000  0x00000000 
0xfe0 0x00000000  0x00000000  0x00000000  0x00000000 
0xff0 0x00000000  0x00000000  0x00000000  0x00000000 
=====BB_END=====
=====RF_BEGIN=====
======= RF REG =======
RF_Path(0)
0x00  0x000356b7  0x00000000  0x00000000  0x00000000 
0x04  0x00000000  0x00000000  0x00000000  0x00000000 
0x08  0x0000f800  0x00000000  0x00000000  0x00000000 
0x0c  0x00000000  0x00000000  0x00000000  0x00000000 
0x10  0x00000000  0x00000000  0x00000000  0x00000000 
0x14  0x00000000  0x00000000  0x00000000  0x00000000 
0x18  0x00000c04  0x00072d70  0x00058000  0x00000c6c 
0x1c  0x00000000  0x00080000  0x00000000  0x00000000 
0x20  0x00000000  0x00000000  0x00000000  0x00000000 
0x24  0x00000000  0x00000000  0x00000000  0x00000000 
0x28  0x00000000  0x00000000  0x00000000  0x00000000 
0x2c  0x00000000  0x00000000  0x00000000  0x0001e280 
0x30  0x00030000  0x0000000f  0x000f7ff2  0x00000000 
0x34  0x00000c48  0x00000290  0x00019064  0x00000000 
0x38  0x00000000  0x00000000  0x00000000  0x00010400 
0x3c  0x00000000  0x00000000  0x00000000  0x00028000 
0x40  0x00000000  0x00000000  0x000074c8  0x00000000 
0x44  0x00000000  0x00000000  0x00000000  0x00000000 
0x48  0x00000000  0x00000000  0x00000000  0x00000000 
0x4c  0x00000000  0x00000000  0x00000000  0x00000000 
0x50  0x00000000  0x000e8231  0x000fac88  0x00000141 
0x54  0x00015007  0x00082060  0x00051cf1  0x000d0000 
0x58  0x000c0160  0x00000000  0x00048000  0x00000000 
0x5c  0x00000000  0x00000000  0x00000000  0x00000000 
0x60  0x00000000  0x00000000  0x00000000  0x00000000 
0x64  0x00000000  0x00000000  0x00000000  0x00001552 
0x68  0x00000000  0x00000000  0x00000000  0x00000000 
0x6c  0x00000000  0x00000000  0x00000000  0x00000000 
0x70  0x00000000  0x00000000  0x00000000  0x00000000 
0x74  0x00000000  0x00000000  0x00000000  0x00000000 
0x78  0x00000000  0x00000000  0x00000000  0x00000000 
0x7c  0x00000000  0x00000000  0x00000000  0x00000082 
0x80  0x0000000e  0x0003fc00  0x0000f042  0x00000000 
0x84  0x00048000  0x00000000  0x0000002a  0x00000065 
0x88  0x00000000  0x00000000  0x00000000  0x00000000 
0x8c  0x00000000  0x00000000  0x00065540  0x00088000 
0x90  0x00000000  0x00000000  0x00000000  0x00000000 
0x94  0x00000000  0x00000000  0x00000000  0x00000000 
0x98  0x00000000  0x00000000  0x00000000  0x00000000 
0x9c  0x00000000  0x00000000  0x00000000  0x00000000 
0xa0  0x00000000  0x00000000  0x00000000  0x00000000 
0xa4  0x00000000  0x00000000  0x00000000  0x00000000 
0xa8  0x00000000  0x00000000  0x00000000  0x00000000 
0xac  0x00000000  0x00000000  0x00000000  0x00000000 
0xb0  0x000ffff0  0x00022218  0x00034c00  0x00000000 
0xb4  0x000c484b  0x0000112a  0x00000540  0x00010408 
0xb8  0x00010200  0x00080001  0x00040001  0x00000400 
0xbc  0x00000000  0x00000000  0x00000000  0x000c0000 
0xc0  0x00000000  0x00000000  0x00002400  0x00000009 
0xc4  0x0004207b  0x00013b13  0x0000008d  0x0008f820 
0xc8  0x00076c06  0x00000000  0x00000000  0x00000000 
0xcc  0x00000000  0x00000000  0x00000000  0x00000000 
0xd0  0x00000000  0x00000000  0x00000000  0x00000000 
0xd4  0x00000000  0x00000000  0x00000000  0x00000000 
0xd8  0x00000000  0x00000000  0x00000000  0x00000000 
0xdc  0x00000000  0x00000000  0x00000000  0x00000140 
0xe0  0x00000000  0x00000000  0x00000000  0x00000000 
0xe4  0x00000000  0x00000000  0x00000000  0x00000000 
0xe8  0x00000000  0x00000000  0x00000000  0x00000000 
0xec  0x00000000  0x00000000  0x00000000  0x00080000 
0xf0  0x00008000  0x00032b35  0x0003a148  0x00037500 
0xf4  0x00030000  0x00000800  0x00000000  0x00000000 
0xf8  0x00000000  0x00000000  0x00000000  0x00000000 
0xfc  0x00000000  0x00000000  0x00000000  0x00000000 
=====RF_END=====
=====SDIO_BEGIN=====
0x00 0x00000010  0x00002ee0  0x00000000  0x00000000 
0x10 0x00000000  0x00000001  0x60800010  0x00000000 
0x20 0x0002000c  0x00e30002  0x20200000  0x00000000 
0x30 0x086e7ed4  0x00000000  0x00000000  0x00000000 
0x40 0x00000000  0x00100000  0x00000000  0x00000000 
0x50 0x00000000  0x00000000  0x00000000  0x00000000 
0x60 0x00000000  0x00000000  0x00000000  0x00000000 
0x70 0x00000000  0x00000000  0x00000000  0x00000000 
0x80 0x00000007  0x00120100  0x0000000a  0x00000000 
0x90 0x00000000  0x00000000  0x00000000  0x00000000 
0xa0 0x00000000  0x00000000  0x00000000  0x00000000 
0xb0 0x00000000  0x00000000  0x00000000  0x00000000 
0xc0 0x00000001  0x00000000  0x00000000  0x00000000 
0xd0 0x00000000  0x00000000  0x00000000  0x00000000 
0xe0 0x00000000  0x00000000  0x00000000  0x00000000 
0xf0 0x00000000  0x00000000  0x00000000  0x00000000 
=====SDIO_END=====
=====F0_BEGIN=====
0x00 32 02 02 02 03 00 00 02 	13 00 10 00 00 00 00 00 
0x10 00 00 01 03 07 00 01 00 	00 00 00 00 00 00 00 00 
0x20 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x30 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x40 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x50 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x60 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x70 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x80 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x90 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xa0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xb0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xc0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xd0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xe0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xf0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
=====F0_END=====

## Финальный набор регистров приёма (живой чип, STA ch4)
0x100=206ff, 0x102=6ff0002, 0x10c=f5b4, 0x280=2000010f, 0x284=20000(RXDMA_IDLE),
0x290=e, 0x608=700060ce, 0x60a=60ce7000, 0x60c=4000418, 0x4ca=8ff0c1f,
0x4c7=4034080, 0x510=2800660f, 0x420=ff311f00, 0x444=10, 0x603=0, 0x116=ff83f7f(32-бит rxff_bndy).

## Параметры
rf_info cur_ch=4 bw=0, fwstate=0x9, mlmext_state=0x4402, tx_rate MCS7, rx rssi=-55.

## TRX / MAC-Q / F0 / HAL (живой чип)
=====TRX_BEGIN=====
os_queue[0]:waked
os_queue[1]:waked
os_queue[2]:waked
os_queue[3]:waked
free_xmitbuf_cnt=16, free_xmitframe_cnt=256
free_ext_xmitbuf_cnt=32, free_xframe_ext_cnt=32
free_recvframe_cnt=256
0, hwq.accnt=0
1, hwq.accnt=0
2, hwq.accnt=0
3, hwq.accnt=0
Rx: Counts of Packets Whose Seq_Num Less Than Reorder Control Seq_Num: 0
Rx: Reorder Time-out Trigger Counts: 0
Rx: Packet Loss Counts: 0
Rx: Duplicate Management Frame Drop Count: 0
Rx: AMPDU BA window shift Count: 0
Rx: Conflict MAC Address Frames Count: 0
=====TRX_END=====
=====MACQ_BEGIN=====
Q0 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:2
Q1 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:2
Q2 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q3 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q4 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q5 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q6 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
Q7 head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
MG head:0xff, tail:0xff, pkt_num:0, macid:1, ac:0
HI head:0xff, tail:0xff, pkt_num:0, macid:0, ac:0
BCN head:0xf8, pkt_num:0
=====MACQ_END=====
=====F0_BEGIN=====
0x00 32 02 02 02 03 00 00 02 	13 00 10 00 00 00 00 00 
0x10 00 00 01 03 07 00 01 00 	00 00 00 00 00 00 00 00 
0x20 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x30 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x40 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x50 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x60 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x70 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x80 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0x90 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xa0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xb0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xc0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xd0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xe0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
0xf0 00 00 00 00 00 00 00 00 	00 00 00 00 00 00 00 00 
=====F0_END=====
=====HAL_BEGIN=====
macid_num:16
sec_cap:0x00
sec_cam_ent_num:16
nss_num:1
band_cap:2G 
bw_cap:20M 40M 
wl_func:P2P MIRACAST TDLS 
=====HAL_END=====

## Свежий survey приёма (живой чип, RSSI -55, qual 96)
20+ BSSID, beacon реально принимаются на каналах 1,4,6,7,8,9,11.
ch1: midea_b8(-58), MGTS_GPON_d23898(-70), MTS_GPON_b4d378(-62)...
ch4: MTS_GPON_9290e0(-52, ассоциирован), MTS_GPON_929638(-66), MTS_GPON_9914(-68)...
ch6: Beeline_2G_F5554F(-59), MGTS_GPON_38AD(-61).
ch8: Netcraze-4479(-62), ch9: VIOL(-67), ch11: MGTS_GPON_C046(-64)...
=> эфир ПОЛОН на всех каналах скана bare-metal (1/6/11), сигнал до -52 дБм.


======================================================================
# ДОКУМЕНТ: rtl8189fs-lakka-live-dump.md
======================================================================

# Живой дамп чипа RTL8189FTV (Lakka 2.3.1, SSH через тот же чип)

> Снято 2026-10-16 по SSH `root@192.168.1.15` (держав).
> Режим: STA ассоциирован с `MTS_GPON_9290e0` (mac `54:c2:50:92:90:e1`),
> канал 4, BW20, RSSI -55 дБм, signal_qual 100. Чип реально принимает.
> Драйвер 8189fs v4.3.24_15589.20151023, ядро 4.14.18.
> Источник чтения: `/proc/net/rtl8189fs/wlan0/{bb,mac,rf}_reg_dump`, `efuse_map`.

Подтверждение живости RF:
- `rf_info`: cur_ch=4, cur_bw=0, oper_ch=4
- `fwstate`=0x9, `mlmext_state`=0x4402
- `rx_signal`: rssi=-55, signal_strength=45, signal_qual=100
- `ap_info`: SSID, aid=6, macid=0, raid=3, ampdu_enable=1
- `current_tx_rate`: MCS7 к роутеру 54:c2:50:92:90:e1

## Ключевые регистры приёма (важно для bare-metal)

| Адрес | Имя | Живой (RX OK) | Наш bare-metal (мёртвый RX) |
|---|---|---|---|
| 0x100 | REG_CR | 0x000206ff | 0x63F |
| 0x200 | RQPN | 0x00e7020c | 0x00e0020c (жестко) |
| 0x280 | RXDMA_AGG_PG_TH | 0x2000010f | 0x010f (был до W.242) |
| 0x284 | RXPKT_NUM | 0x00020000 | 0x00020000 |
| 0x290 | RXDMA_MODE | 0x0000000e | 0x0e |
| 0x608 | RCR | 0x700060ce | 0x7000604e (без CBSSID_BCN) |
| 0x116 | rxff_bndy (32-бит) | 0x3f7f0ff8 | 0x3f7f |

RF-регистры (живой, канал 4):
- RF 0x00 = 0x00035677 (наш fixed 0x33D95!)
- RF 0x18 = 0x00000c04 (канал 4, BW20: бит 11,10=0xC00)
- RF 0x0C = 0x00000000 (живой! низкий уровень AC)
- RF 0x30 = 0x00030000
- RF 0x52 = 0x000e8231, RF 0x56 = 0x00051cf1

BB-регистры (живой, на канале 4):
- 0x800 RFMOD = 0x83045700 (совпадает с нашим!)
- 0xC50 (IGI/AGC) = 0x69553432 (у нас 0x69553420!)
- 0xA50 (TRSSI) = 0x5b28b300 (у нас 0x0 — мёртв)
- 0xE30 (IQK) = 0x01008c00 (cвой после IQX, не 0x18008c1c)

## Полные дампы

### BB (0x800..0xFF0)
```
0x800 0x83045700  0x00000001  0x0000fccd  0x0000000a
0x810 0x10001331  0x020c3d10  0x00200385  0x00000000
0x820 0x01000100  0xa1390204  0x00000000  0x0c000000
0x830 0x00000000  0x00000000  0x00000000  0x00000000
0x840 0x042378c8  0x00000000  0x00000000  0x00000000
0x850 0x00030000  0x00000000  0x569a569a  0x569a569a
0x860 0x00100130  0x00000000  0x00000000  0x31313100
0x870 0x00000000  0x25004000  0x00000808  0x004f0201
0x880 0xb0000b1e  0x00000007  0x00000000  0xccc000c0
0x890 0xffff0900  0xc350fffe  0xffffff50  0xffffffff
0x8a0 0x00100000  0x00000000  0x0000000f  0x00000000
0x8b0 0x00000000  0x00000183  0x001078c8  0x00000000
0x8c0 0x03828491  0x00000059  0xb2b3b315  0xadb0b8b4
0x900 0x00000000  0x00000023  0x00000208  0x81121111
0x910 0x00000002  0x00000201  0x00000000  0x00000000
0x930 0x00000077  0x00000000  0x00000000  0x00000000
0x940 0x00000000  0x00000003  0x99000000  0x00000010
0x950 0x20003000  0x4a300000  0x4bc5d87a  0x04eb9b79
0x9a0 0x90001001  0x90001001  0xd0000001  0x00000000
0x9c0 0x00001de9  0x00000000  0x00000000  0x00310031
0xa00 0x00d047c8  0x80ff800c  0x8c038300  0x2e7f120f
0xa10 0x9500bb78  0x1114d028  0x00881117  0x89140f00
0xa20 0xd1d80000  0x5a7da0bd  0x0000223b  0x00d3a000
0xa50 0x5b28b300  0x003090ff  0x00005881  0x000000ae
0xa60 0x8080fd01  0x01fd00ae  0x00000000  0x00000000
0xa70 0x101fbf00  0x00000007  0x00008900  0x225b0606
0xa80 0x218075b1  0x00120000  0x040c0000  0x12345678
0xa90 0xabcdef00  0x001b1b89  0x05100000  0x3f000000
0xbd0 0x00000000  0x00000000  0x04100000  0x00000000
0xbe0 0x00000000  0x00000000  0x0001ff3c  0x00000000
0xc00 0x48071d40  0x03a05611  0x000000e4  0x6c6c6cec
0xc10 0x18800000  0x4000f0fd  0x08800000  0x40000100
0xc30 0x69e9cc4a  0x31000040  0x21688080  0x00001714
0xc40 0x0b78403f  0x00010036  0xec020107  0xa03e0346
0xc50 0x69553432  0x43bc0094  0x00013169  0x00250492
0xc60 0x00000000  0x7112848b  0x47c07bff  0x00000036
0xc70 0x2c7f000d  0x020600db  0x003f0001  0x00b91612
0xc80 0x47c20123  0x21f60000  0x40000100  0x20200000
0xc90 0x00091521  0x00000000  0x00121820  0x00007f7f
0xca0 0xf0000000  0x000300a0  0x00000000  0x00000000
0xcb0 0x00000000  0x00000000  0x00000000  0x28000000
0xcd0 0x00000000  0x00000000  0x64b22427  0x00766932
0xce0 0x00222222  0x10000000  0x37644302  0x2f97d40c
0xcf0 0x00000000  0x00310000  0x00000000  0x00000000
0xd00 0x04030740  0x40020401  0x0000907f  0x20010201
0xd10 0xa0633333  0x3333bc53  0x7a8f5b6f  0x00000000
0xd30 0x00000000  0x80608000  0x98000000  0x40127353
0xd40 0x06000000  0x00000000  0x00000000  0x00000000
0xd50 0x6437140a  0x00000000  0x00000282  0x30032064
0xd60 0x4653de68  0x04518a3c  0x00002101  0x2a201c16
0xd90 0x00000000  0x00000000  0x00000000  0x000a0013
0xda0 0x000a0059  0x0000000c  0x00000003  0xffb00000
0xdb0 0xffc80000  0xffc8e000  0x00050000  0x1fb60000
0xdc0 0x00000000  0x00090000  0x0000033a  0x0000000a
0xdd0 0x0000001d  0x0000003a  0x00cd6600  0x000077e5
0xde0 0x1fc80000  0x00000000  0x0002030b  0x00006807
0xdf0 0x00035c1e  0x00d56062  0x00000005  0x00000000
0xe00 0x31313131  0x31313131  0x0390312d  0x00000000
0xe10 0x2f2f2f2f  0x2f2f2f2f  0x2d2d2d2d  0x2d2d2d2d
0xe20 0xc0000000  0x00100000  0x000000ff  0x00000000
0xe30 0x01008c00  0x01008c00  0x82160000  0x281613ff
0xe40 0x810a7c02  0x01004800  0xf8000000  0x0046a911
0xe50 0x1000dc1f  0x10008c1f  0x02140102  0x28160d05
0xe60 0x021400a0  0x281600a0  0x01c00056  0x01c00010
0xe70 0x01c00010  0x02000010  0x02000010  0x02000010
0xe80 0x02000010  0x01c00010  0x02000010  0x01c00010
0xe90 0x000c57c8  0x010a0000  0x000011e4  0x00020000
0xea0 0x001a8354  0x00fd0000  0x00005c90  0x07fc3000
0xed0 0x01c00010  0x01c00010  0x01c00010  0x00000010
0xee0 0x00000010  0x015e5448  0x21555448  0x03c00010
0xef0 0x83045700  0x00000001  0x00000000  0x00000000
0xf00 0x00000300  0x00000000  0x00000000  0x00000000
0xf10 0x00000000  0x00000003  0x00000000  0x00000000
0xf80 0x00000c4c  0x00006b07  0x0000ffff  0x00000000
0xf90 0x0b5802b0  0xcce17b03  0x00450001  0x00000000
0xfa0 0xffff0000  0x0002030b  0x00006807  0x0a035c1e
0xfb0 0x0070000a  0x000ad0b4  0x00f0062b  0x00000000
```

### RF (0x00..0xFC), путь A
```
0x00  0x00035677  0x00000000  0x00000000  0x00000000
0x08  0x0000f800  0x00000000  0x00000000  0x00000000
0x18  0x00000c04  0x00072d70  0x00058000  0x00000c6c
0x1c  0x00000000  0x00080000  0x00000000  0x00000000
0x2c  0x00000000  0x00000000  0x00000000  0x00018340
0x30  0x00030000  0x0000000f  0x000f7ff2  0x00000000
0x34  0x00000c48  0x00000290  0x00019064  0x00000000
0x38  0x00000000  0x00000000  0x00000000  0x00010400
0x3c  0x00000000  0x00000000  0x00000000  0x00028000
0x40  0x00000000  0x00000000  0x000078c8  0x00000000
0x50  0x00000000  0x000e8231  0x000fac88  0x00000141
0x54  0x00015007  0x00082060  0x00051cf1  0x000d0000
0x58  0x000c0160  0x00000000  0x00048000  0x00000000
0x64  0x00000000  0x00000000  0x00000000  0x00001552
0x7c  0x00000000  0x00000000  0x00000000  0x00000082
0x80  0x0000000e  0x0003fc00  0x000bf014  0x00020000
0x84  0x00048000  0x00000000  0x0000002a  0x00000065
0x8c  0x00000000  0x00000000  0x00065540  0x00088000
0xb0  0x000ffff0  0x00022218  0x00034c00  0x00000000
0xb4  0x000c484b  0x0000112a  0x00000540  0x00010408
0xb8  0x00010200  0x00080001  0x00040001  0x00000400
0xbc  0x00000000  0x00000000  0x00000000  0x000c0000
0xc0  0x00000000  0x00000000  0x00002400  0x00000009
0xc4  0x0004207b  0x00013b13  0x0000008d  0x0008f820
0xc8  0x00076c06  0x00000000  0x00000000  0x00000000
0xdc  0x00000000  0x00000000  0x00000000  0x00000140
0xec  0x00000000  0x00000000  0x00000000  0x00080000
0xf0  0x00008000  0x00032b35  0x0003a148  0x00037500
0xf4  0x00030000  0x00000800  0x00000000  0x00000000
```

### MAC (ключевое, 0x100..0x6FF)
```
0x100 0x000206ff  0x00000110  0x00000000  0x0000f5b4
0x110 0x00000000  0x3f7f0ff8  0x00000000  0x28502850
0x200 0x00e7020c  0x00e7020c  0xd800f810  0x00fd0000
0x280 0x2000010f  0x00020000  0x00000000  0x00000000
0x290 0x0000000e  0x00003f80  0x00000000  0x00000000
0x400 0x017f80ff  0x017f80ff  0x007f80ff  0x007f80ff
0x420 0xff311f00  0xd730f8f8  0x3030100a  0x00004000
0x440 0x0000015f  0x00000010  0xfffff000  0x00000010
0x450 0x003ff000  0xf6700005  0x00003fff  0x1000f800
0x500 0x002f3222  0x005e4322  0x005ea42b  0x0000a44f
0x510 0x2800660f  0x100a100a  0x0914fe00  0x00000000
0x520 0x00003f0f  0x21ff4f0f  0x806c0a8f  0x00000000
0x540 0x80006404  0x00400180  0x00000000  0x00000000
0x550 0x00001009  0x00640064  0x00020202  0x0f0fff28
0x600 0x00000000  0x00003000  0x700060ce  0x04000418
0x610 0x27388102  0x0000cd88  0x9250c254  0x0000e190
0x620 0xffffffff  0xffffffff  0x00000000  0x00000000
0x630 0x00000000  0x00000000  0x100a2828  0x0a0a0808
0x6a0 0x0400ffff  0x0000ffff  0x00000000  0x00000000
0x6e0 0x9250c254  0x0000e190  0x9250c254  0x0000e190
```

## Расхождения наши (мёртвый RX) против живого (RX OK)

Кандидат №1 — **RF 0x00**: живой `0x00035677`, наш fixed `0x33D95`. RF 0x00 = LNA/RX-path. Различие верхних байтов может запирать приём.

Кандидат №2 — **RF 0x0C**: живой `0x00000000`, табличный `0x0008992b`. RF 0x0C — аналоговый путь.

Кандидат №3 — **BB 0xC50 (IGI/AGC init)**: живой `0x69553432`, наш (без LTE) `0x69553420`. Младший байт 0x32 vs 0x20 — IGI (младший) чувствительность.

Кандидат №4 — **MAC 0x510**: живой `0x2800660f`. Ранее ставили `0x513/0x516`. 

Кандидат №5 — **BB 0xA50 (TRSSI)**: живой `0x5b28b300`, наш (мёртвый) `0x0` — но это отчёт, не причине.

Это зафиксированный эталон для сверки. Дальнейшее — сравнение по каждому адресу при добавлении наших записей.

======================================================================
# ДОКУМЕНТ: rtl8189fs-lakka-live-rx-dump-w7.md
======================================================================

# RTL8189FS Lakka live RX dump (W.7 / 2026-10-17)

Снято через proc-интерфейс драйвера rtl8189fs (Lakka, WiFi связан, в сети).
Механизм: `echo "ADDR 4" > /proc/net/rtl8189fs/wlan0/read_reg; cat ...`
Фоновые дампы: `bb_reg_dump`, `rf_reg_dump`, `sdio_local_reg_dump`, `sd_f0_reg_dump`.

Состояние: fwstate=0x9 (linked), mlmext_state=0x4402, сеть активна (принимает beacon).

## Ключевые MAC-регистры (read_reg, 4 байта)

| Адрес | Значение | Комментарий |
|---|---|---|
| 0x0118 TRXFF_STATUS | 0x00000000 | |
| 0x011C RXFF_PTR | **0x1D001D00 / 0x37003700** (меняется) | живой приём, указатель RX FIFO |
| 0x0124 HISR | 0x00000080 | bit7 |
| 0x012C HISRE | 0x00000000 | |
| 0x0280 RXDMA_AGG | 0x2000010F | |
| 0x0284 RXPKT_NUM | **0x00020000** | = RXDMA_IDLE (bit17), как у нас |
| 0x010C TRXDMA_CTRL | 0x0000F5B4 | совпадает с нашим |
| 0x0114 TRXFF_BNDY | 0x3F7F0FF8 | совпадает с нашим |
| 0x0102 CR | 0x06FF0002 | = MACTXEN+MACRXEN+..., как у нас 0x0206FF |
| 0x0608 RCR | 0x700060CE | в сети (CBSSID_BCN|DATA); в scan был бы 0x...4E |
| 0x0604 RRSR | **0x00003000** | ! у нас 0x1F5F |
| 0x0600 RXFLTMAP0 | 0x00000000 | |
| 0x0610 RXFLTMAP1? | 0x27388102 | |
| 0x0618 RXFLTMAP2 | 0x9250C254 | у нас при scan 0x0 (ожидаемо) |
| 0x0300 FIFOPAGE | 0xEAEAEAEA | |
| 0x0650 LLT | 0x00EB0426 | |
| 0x0680 TX_RPT_CTRL | 0x0000010C | |
| 0x0690 WOW_CTRL | 0x000F0020 | |
| 0x0700 HWSEQ_CTRL | 0x87654321 | тестовый паттерн? |
| 0x06A0 | 0x0400FFFF | |
| 0x01C0 | 0x006D1001 | |
| 0x01D0 | 0x30000042 | H2C box |
| 0x052A | 0x0A8F806C | |
| 0x0550 BCN_CTRL | 0x00001009 | |
| 0x0551 | 0x09000010 | |
| 0x0554 | 0x64006400 | |
| 0x0558 | 0x00020202 | |
| 0x05A0 | 0x02020202 | |

## SDIO local (sdio_local_reg_dump)

```
0x00 0x00000010  0x00002ee0  0x00000000  0x00000000
0x10 0x00000000  0x00000001  0x60800010  0x00000000
0x20 0x0002000c  0x00e70002  0x20200000  0x00000000
0x30 0x3b11bd5e  0x00000000  0x00000000  0x00000000
0x40 0x00000000  0x001074e0  0x00000000  0x00000000
0x80 0x00000007  0x00120100  0x0000000a  0x00000000
0xc0 0x00000001  ...
```

Ключевое:
- local 0x20 = 0x0002000C → FREE_TXPG: HIQ=12 (у нас hiq_free=12, совпадает)
- local 0x24 = 0x00E70002 → FIFOPAGE (у нас 0xE7020C)
- **local 0x30 = 0x3B11BD5E → RX0_REQ_LEN ненулевой** (у нас rxlen=0)
- local 0x18 = 0x60800010 → SDIO HISR: биты TSF_BIT32/PSTIMEOUT/TXFOVW (статусные)

## BB (bb_reg_dump) — ключевые

```
0x800 0x83045700   (RFMOD — совпадает с нашим)
0x820 0x01000100   (PI-конфиг как после IQK)
0x850 0x00030000 | 0x569a569a...
0x860 0x00100130
0x874 0x25004000
0x930 0x00000077
0x948 0x99000000   (Path_SEL BB — как у нас)
0xa00 0x00d047c8
0xa04 0x80ff800c
0xa08 0x8c038300   (CCA/OFDM — СОВПАДАЕТ с нашим!)
0xa0c 0x2e7f120f
0xa10 0x9500bb78
0xa20 0xd1d80000
0xa24 0x5a7da0bd
0xa28 0x0000223b
0xa2c 0x00d3a000
0xa50 0x02d16f00   (RX signal/RSSI — ненулевой, у нас trssi=0)
0xa54 0x0830fefe   (rxrpt — ненулевой "живой", у нас 0x00300000/0x10300000)
0xa58 0x05005881
0xa5c 0x000000ff
0xc00 0x48071d40
0xc04 0x03a05611   (как у нас в IQK save)
0xc50 0x69553439 | 0x43bc0094 | 0x00013169 | 0x00250492  (AGC — совпадает с нашим)
0xc78 0x003f0001
0xc84 0x21f60000   (BB-таблица дефолт, у нас тоже 0x21F60000)
0xca8 0x00121820
0xcdc 0x64b22427
0xee0 0x83045700
0xf80 0x00000164 | 0x0000085f | 0x00007642
0xf90 0x01580005 | 0x21a01645
0xfa0 0xffff0000 | 0x000201c9 | 0x00006807 | 0x0a035c1e
0xfb0 0xf838000a | 0x000af933 | 0x00310210
```

## RF (rf_reg_dump, path 0)

```
0x00 0x00028017   (! у нас после LCK 0x33D93)
0x08 0x0000f800   (! у нас после LCK 0x85000 LOK result?)
0x18 0x00000c04 | 0x00072d70 | 0x00058000 | 0x00000c6c
0x1c 0x00000000 | 0x00080000 | ...
0x2c 0x00014380 (последнее слово)
0x30 0x00030000   (TX power? совпадает с нашим RF30=0x30000)
0x34 0x0000000f | ... (первое слово = RF31? )
0x38 0x000f7ff2   (RF32=0xF7FF2 — совпадает)
0x3c 0x00010400
0x50 0x000e8231 | 0x000fac88 | 0x00000141
0x54 0x00015007 | 0x00082060 | 0x00051cf1 | 0x000d0000
0x58 0x000c0160 | 0x00048000
0x64 0x00001552
0x7c 0x00000082
0x80 0x0000000e | 0x0003fc00 | 0x000bf000 | 0x00020000
0x84 0x00048000 | 0x0000002a | 0x00000065
0x8c 0x00065540 | 0x00088000
0xb0 0x000ffff0   (совпадает с нашим RF B0=0xFFFF0!)
0xb4 0x000c484b | 0x0000112a | 0x00000540 | 0x00010408
0xb8 0x00010200 | 0x00080001 | 0x00040001 | 0x00000400
0xbc 0x000c0000
0xc0 0x00002400 | 0x00000009
0xc4 0x0004207b | 0x00013b13 | 0x0000008c | 0x0008f820
0xc8 0x00076c06
0xdc 0x00000140
0xec 0x00080000
0xf0 0x00008000 | 0x00032b35 | 0x0003a148 | 0x00037500
0xf4 0x00030000 | 0x00000800
```

## Разбор против нашего мёртвого состояния (W.264)

Совпадает (подтверждение корректности нашего init):
- RFMOD 0x800=0x83045700, BB 0xA08=0x8C038300, AGC 0xC50=0x69553420, Path_SEL BB 0x948=0x99000000, RF 0xB0=0xFFFF0, RF 0x32=0xF7FF2, TRXDMA 0x10C=0xF5B4, TRXFF_BNDY=0x3F7F0FF8, RXPKT_NUM=0x20000, CR=0x6FF0002, SDIO local 0x20=0x2000C (HIQ=12).

Различается (потенциальные причины мёртвого RX):
1. **RXFF_PTR (0x11C): живой ненулевой (0x37003700), наш 0** — живой принимает beacon, наш ничего.
2. **SDIO local RX0_REQ_LEN (0x30): живой 0x3B11BD5E, наш rxlen=0** — FW у живого отдаёт пакеты, у нас нет.
3. **BB 0xA50/0xA54 (RX signal/rxrpt): живой 0x02D16F00/0x0830FEFE, у нас 0/0x00300000** — приёмник у живого реально декодирует, у нас «заглушка».
4. **RRSR 0x604: живой 0x3000, наш 0x1F5F** — различается! (возможно basic-rate/маска приёма)
5. **RF 0x00: живой 0x28017, наш 0x33D93; RF 0x08: живой 0xF800, наш 0x85000** — различие в RF-настройке/калибровке.

## Динамика RX-индикаторов (5 чтений по 0.5с, сеть активна)

```
        RXFF_PTR(0x11C)  0xA50(RX sig)   0xA54(rxrpt)   0xA08(CCA)   0x284(RXPKT_NUM)
t0      0x000a800a       0x0780b000      0x0830e7fe     0x8c038300   0x00020000
t1      0x0009c009c      0x07317200      0x0830f9fe     0x8c038300   0x00020000
t2      0x000150015      0x0838b500      0x0030fefe     0x8c038300   0x00020000
t3      0x0001d801d      0x0a20b000      0x0830df02     0x8c038300   0x00020000
t4      0x000266826      0x0820b000      0x0830ea02     0x8c038300   0x00020000
```

Наблюдения:
- **RXFF_PTR (0x11C) непрерывно движется** — RX FIFO реально заполняется приходящими кадрами (beacon каждые 100 мс).
- **0xA50 (RX signal / RSSI) и 0xA54 (rxrpt) живые** — меняются, BB-декодер работает.
- **0x284 (RXPKT_NUM) = 0x20000 стабилен** — это RXDMA_IDLE, пакеты НЕ накапливаются в счётчике. Значит у живого счётчик тоже 0 в этой точке — FW забирает пакеты из FIFO сразу, а регистр показывает только DMA-статус.
- 0xA08 (CCA/OFDM) = 0x8C038300 стабилен — как у нас.

Ключевой контраст с нашим мёртвым состоянием (W.264):
| Регистр | Живой (RX) | Наш (мёртвый) |
|---|---|---|
| 0x11C RXFF_PTR | движется 0xA80..0x2668 | **0** |
| 0xA50 RX signal | 0x07-0x0A млн (живой) | статичен |
| 0xA54 rxrpt | 0x0830E7FE (живой ритм) | 0x00300000/0x10300000 заглушка |
| 0xA08 CCA | 0x8C038300 | 0x8C038300 (совпадает!) |
| 0x284 RXPKT_NUM | 0x20000 | 0x20000 (совпадает) |

Вывод: у живого RX FIFO реально заполняется (RXFF_PTR движется, RX0_REQ_LEN ненулевой) — живой принимает beacon каждые 100 мс. У нас FIFO пуст, RX0_REQ_LEN=0 — чип не принял НИ ОДНОГО кадра, хотя BB 0xA08 (CCA) показывает ту же «занятость эфира». Разница на уровне «декодирования» (0xA50/0xA54) — у живого RX-статистика живая (декодер реально обрабатывает OFDM/CCK-сигнал), у нас статичная заглушка (декодер «слышит» энергию, но не валидные кадры).

======================================================================
# ДОКУМЕНТ: lakka-live/LAKKA-LIVE-SNAPSHOT-W537.md
======================================================================

# Живые замеры с работающей Lakka 2.3.1 (H3 + RTL8189FTV, тот же чип)

Дата: 2026-10-20. Получено напрямую по SSH с платы **10.72.75.55** (root/root), к которой подключена та же сеть **A05s пользователя М** (BSSID `9a:4d:56:7e:81:4f`, ch 1, WPA2-PSK) — ровно та, к которой подключается наш bare-metal.

Это эталонные значения регистров, CAM и security-конфигурации, снятые с **реально работающего** стека на этом железе. Использовать как primary source при сверке.

## Система

```
Linux Lakka 4.14.18 #1 SMP Fri Sep 13 14:36:07 CEST 2019 armv7l GNU/Linux
8189fs  987136  0
cfg80211 245760  1 8189fs
wlan0: inet 10.72.75.55/24, MAC 02:81:38:27:88:cd
Driver: v4.3.24_15589.20151023
```

## Security: CAM (главный ответ на «куда писать ключи»)

```
===/proc/net/rtl8189fs/wlan0/sec_cam===
max_num:16, used:0x00000030
reg_scr:0x010c

HW sec cam:
id ctrl   addr              key                              kid type MK GK valid
 0 0x0000 00:00:00:00:00:00 4c847c50...                       0 N/A  0  0  0
 1 0x0000 00:00:00:00:00:00 3d057864...                       0 N/A  0  0  0
 2 0x0000 00:00:00:00:00:00 8e232214...                       0 N/A  0  0  0
 3 0x0000 00:00:00:00:00:00 9c5eed2e...                       0 N/A  0  0  0
 4 0x8010 9a:4d:56:7e:81:4f 1f74306b43415fe984404ddbfc36c865   0 AES  0  0  1   <- pairwise (BSSID!)
 5 0x8051 9a:4d:56:7e:81:4f 6d6852f7ac066590331ed4dec354b2e1   1 AES  0  1  1   <- group GTK (BSSID!, keyid=1)
 6..15 ctrl=0x0000, invalid
```

SW-кэш (cache) — те же записи 4 и 5.

**Вывод:** для STA-режима (A05s, WPA2, GTK keyid=1):
- **pairwise**: CAM entry **4**, `ctrl=0x8010`, `addr = BSSID`.
- **group (GTK)**: CAM entry **5** (НЕ 1, НЕ 0), `ctrl=0x8051`, `addr = BSSID` (НЕ null!), `kid=1`, group-бит.
- `reg_scr = 0x010c` = `SCR_CHK_KEYID(BIT8)|SCR_RxDecEnable(BIT3)|SCR_TxEncEnable(BIT2)`.

`sec_info`:
```
auth_alg=0x2, enc_alg=0x4, auth_type=0x7, enc_type=0x6
hw_decrypted=1
aes_sw_dec_cnt=0  (аппаратный RX-decrypt работает)
```

## MAC-регистры (выборочно, ключевые)

Из `/proc/net/rtl8189fs/wlan0/mac_reg_dump`:

| Адрес | Значение | Комментарий |
|---|---|---|
| 0x600 | `0x000206ff` | CR = MACTXEN\|MACRXEN (наш `0x206FF`) ✔ |
| 0x608 | `0x700060ce` | **RCR (наш `0x700060CE`) ✔** |
| 0x610 | `0x27388102` | наш MAC 02:81:38:27:88:cd |
| 0x618 | `0x7e564d9a` | BSSID: 9a:4d:56:7e:81:4f ✔ |
| 0x630 | `0x100a2828` | rateset |
| 0x670 | `0x0000007d` | CAMCMD (последняя операция, bit17/read?) |
| 0x674 | `0x4d9a8051` | **CAMWRITE word0 = 0x8051\|9a<<16\|4d<<24 → entry 5 GTK BSSID** ✔ |
| 0x680 | `0x0000010c` | **REG_SECCFG = 0x10C** ✔ |
| 0x6a0 | `0x0400ffff` | RXFLTMAP |

## MLME/состояние

```
fwstate=0x9
pmlmeinfo->state=0x4402
qos_option=1, ht_option=1
macid_info: id=0 (9a:4d:56:7e:81:4f, state C AP), id=1 (ff:.. bmc RSVD)
current_tx_rate: macid 0 → MCS7
```

## Соответствие нашему bare-metal (W.537)

| Параметр | Lakka (эталон) | Наш код до W.537 | Наш код W.537 |
|---|---|---|---|
| pairwise CAM | entry 4, BSSID, 0x8010 | entry 4, BSSID, 0x8010 ✔ | ✔ |
| GTK CAM | entry 5, **BSSID**, 0x8051 | entry 1, **null**, 0x8051 ✘ | **entry 5, BSSID, 0x8051** ✔ |
| REG_SECCFG | 0x010C | 0x030C/0x034C ✘ | **0x010C** ✔ |
| RCR | 0x700060CE | 0x700060CE ✔ | ✔ |

## Полный MAC-дамп сохранён

Полный `mac_reg_dump` (0x000..0x7f0) — в файле `LAKKA-MACREG-FULL.txt` рядом.

======================================================================
# ДОКУМЕНТ: lakka-live/LAKKA-CAM-SECURITY.md
======================================================================

LAKKA 2.3.1 live, H3 + RTL8189FTV, A05s пользователя М (9a:4d:56:7e:81:4f)
Снято 2026-10-20 по SSH (10.72.75.55). Полный дамп для сверки с bare-metal.

==========================================================================
/proc/net/rtl8189fs/wlan0/sec_cam (HW CAM)
==========================================================================
sec_cap:0x00
flags:0x00000000
max_num:16
used:0x00000030
reg_scr:0x010c
HW sec cam:
id ctrl   addr              key                              kid type    MK GK valid
 0 0x0000 00:00:00:00:00:00 4c847c507b546458d02c180daf3bbe77   0 N/A      0  0     0
 1 0x0000 00:00:00:00:00:00 3d05786404fa39d0a8667724ed471467   0 N/A      0  0     0
 2 0x0000 00:00:00:00:00:00 8e2322141383b3577c0b4fa45ca98014   0 N/A      0  0     0
 3 0x0000 00:00:00:00:00:00 9c5eed2ece4fbe45ef26cd675a523a45   0 N/A      0  0     0
 4 0x8010 9a:4d:56:7e:81:4f 1f74306b43415fe984404ddbfc36c865   0 AES      0  0     1
 5 0x8051 9a:4d:56:7e:81:4f 6d6852f7ac066590331ed4dec354b2e1   1 AES      0  1     1
 6 0x0000 00:00:00:00:00:00 1c9fbc724a74ff2d9484a48b05f67b4f   0 N/A      0  0     0
 7 0x0000 00:00:00:00:00:00 c6043b60f16f8527cbcef8b301ad9f7a   0 N/A      0  0     0
 8 0x0000 00:00:00:00:00:00 030a9f47b72ed7a03948de228f2c3986   0 N/A      0  0     0
 9 0x0000 00:00:00:00:00:00 6104d9d660aa1c73bf135ffe510a7066   0 N/A      0  0     0
10 0x0000 00:00:00:00:00:00 b53b9642076d793342c7648f001d8bbb   0 N/A      0  0     0
11 0x0000 00:00:00:00:00:00 f52dbfee6f42040af74c56849a5896a2   0 N/A      0  0     0
12 0x0000 00:00:00:00:00:00 df0440b0367fd10c7679a0831e259506   0 N/A      0  0     0
13 0x0000 00:00:00:00:00:00 0302c43c75ca37071db6c83d02601f1d   0 N/A      0  0     0
14 0x0000 00:00:00:00:00:00 621d3a0191f9a6be12941f7ae6160362   0 N/A      0  0     0
15 0x0000 00:00:00:00:00:00 e9bd2231ac4c04514a1194bd779d7607   0 N/A      0  0     0

==========================================================================
/proc/net/rtl8189fs/wlan0/sec_cam_cache (SW CAM cache)
==========================================================================
SW sec cam cache:
id ctrl   addr              key                              kid type    MK GK valid
 4 0x8010 9a:4d:56:7e:81:4f 1f74306b43415fe984404ddbfc36c865   0 AES      0  0     1
 5 0x8051 9a:4d:56:7e:81:4f 6d6852f7ac066590331ed4dec354b2e1   1 AES      0  1     1

==========================================================================
/proc/net/rtl8189fs/wlan0/sec_info
==========================================================================
auth_alg=0x2, enc_alg=0x4, auth_type=0x7, enc_type=0x6
hw_decrypted=1
wep_sw_enc_cnt=0, 0, 0
wep_sw_dec_cnt=0, 0, 0
tkip_sw_enc_cnt=0, 0, 0
tkip_sw_dec_cnt=0, 0, 0
aes_sw_enc_cnt=0, 0, 2
aes_sw_dec_cnt=0, 0, 0

==========================================================================
/proc/net/rtl8189fs/wlan0/ap_info
==========================================================================
SSID=A05s пользователя М
sta's macaddr:9a:4d:56:7e:81:4f
cur_channel=1, cur_bwmode=0, cur_ch_offset=0
wireless_mode=0xb, rtsen=0, cts2slef=0
state=0x1, aid=8, macid=0, raid=3
qos_en=1, ht_en=1, init_rate=19
bwmode=0, ch_offset=0, sgi_20m=1,sgi_40m=0
ampdu_enable = 1
agg_enable_bitmap=0, candidate_tid_bitmap=0
ldpc_cap=0x0, stbc_cap=0x0, beamform_cap=0x0
tid=0, enable=1, ampdu_size=16, indicate_seq=236
tid=6, enable=0, ampdu_size=255, indicate_seq=2
tid=7, enable=1, ampdu_size=16, indicate_seq=65535

==========================================================================
/proc/net/rtl8189fs/wlan0/macid_info
==========================================================================
max_num:16
used:0x00000003
id  bmc if_g ch_g macaddr           status
  0   0    0   -1 9a:4d:56:7e:81:4f  C AP
  1   1   -1   -1 ff:ff:ff:ff:ff:ff  RSVD

==========================================================================
/proc/net/rtl8189fs/wlan0/current_tx_rate
==========================================================================
macid if_g macaddr           tx_rate
    0    0 9a:4d:56:7e:81:4f MCS7

==========================================================================
/proc/net/rtl8189fs/wlan0/fwstate / mlmext_state / qos_option / ht_option
==========================================================================
fwstate=0x9
pmlmeinfo->state=0x4402
qos_option=1
ht_option=1

======================================================================
# ДОКУМЕНТ: lakka-live/LAKKA-MACREG-FULL.txt
======================================================================

LAKKA 2.3.1 live, H3 + RTL8189FTV, A05s пользователя М (9a:4d:56:7e:81:4f)
Полный MAC-регистровый дамп, снят 2026-10-20 по SSH.
Любые значения нашего bare-metal можно сверять с этим списком.

======= MAC REG =======
0x000 0xfc1706fa  0x00020002  0x0020fca3  0x00000000
0x010 0x00417041  0x04000000  0x00400100  0x078ff900
0x020 0x0c0000c1  0x354107df  0x1c2cb1f1  0x00001c15
0x030 0x8020fdfb  0x3c000000  0x00000843  0x00002e22
0x040 0x00000008  0x00000003  0x0000f000  0x01028282
0x050 0x80000000  0x00000000  0x00000000  0x00000000
0x060 0xeaeaeaea  0x00800000  0x00002000  0x80020002
0x070 0x00030080  0x000000a0  0x94018041  0x00000003
0x080 0x070405c6  0x80000000  0x00000c0c  0x00000000
0x090 0xff000000  0xeaeaeaea  0xeaeaeaea  0x00000000
0x0a0 0x0200000f  0x36789abc  0xeaeaeaea  0xeaeaeaea
0x0b0 0x00000000  0x60d00000  0x00000000  0x00000200
0x0c0 0x06677889  0x0000000c  0x00000000  0x00068129
0x0d0 0x0001f802  0x00000000  0x00000000  0x00000000
0x0e0 0x00000000  0xeaeaeaea  0xeaeaeaea  0x98013f15
0x0f0 0x60441523  0x00000000  0xc2030000  0x6700000c
0x100 0x000206ff  0x00000110  0x00000000  0x0000f5b4
0x110 0x00000000  0x3f7f0ff8  0x00000000  0x32103210
0x120 0x00000000  0x00000080  0x00000000  0x00000000
0x130 0x32070012  0x00000009  0x62030106  0x10000200
0x140 0x00000000  0x00000000  0x00000000  0x00000000
0x150 0x00000000  0x00000000  0x00000000  0x00000000
0x160 0x00000000  0x00000001  0x00000000  0x00000000
0x170 0x00000000  0x00000002  0xffffffc0  0xffe00000
0x180 0x00000000  0x00000000  0x00000000  0x00000000
0x190 0x00000000  0x10000001  0x00000880  0x00000000
0x1a0 0x00000000  0x00000000  0x00000000  0x00000000
0x1b0 0x00000000  0x00000000  0x12340808  0x00000000
0x1c0 0x006d1001  0xfe00b75d  0x00000093  0x00000000
0x1d0 0x44000042  0x44000042  0x44000042  0x44000042
0x1e0 0x00000000  0x00200701  0x00070000  0x00000000
0x1f0 0x00000004  0x00000004  0x00000004  0x00000004
0x200 0x00e7020c  0x00e7020c  0x9900f810  0x00fd0000
0x210 0x00000000  0x00000202  0x00000000  0x00000000
0x220 0x00000000  0x00002020  0x0000f800  0x00000000
0x230 0x00000000  0x00000000  0x00000000  0x00000000
0x240 0x00000000  0x00000000  0x00000000  0x00000000
0x250 0x00000000  0x00000000  0x00000000  0x00000000
0x260 0x00000000  0x00000000  0x00000000  0x00000000
0x270 0x00000000  0x00000000  0x00000000  0x00000000
0x280 0x2000010f  0x00020000  0x00000000  0x00000000
0x290 0x0000000e  0x00003f80  0x00000000  0x00000000
0x2a0 0x00000000  0x00000000  0x00000000  0x00000000
0x2b0 0x00000000  0x00000000  0x00000000  0x00000000
0x2c0 0x00000000  0x00000000  0x00000000  0x00000000
0x2d0 0x00000000  0x00000000  0x00000000  0x00000000
0x2e0 0x00000000  0x00000000  0x00000000  0x00000000
0x2f0 0x00000000  0x00000000  0x00000000  0x00000000
0x300 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x310 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x320 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x330 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x340 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x350 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x360 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x370 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x380 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x390 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x3a0 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x3b0 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x3c0 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x3d0 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x3e0 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x3f0 0xeaeaeaea  0xeaeaeaea  0xeaeaeaea  0xeaeaeaea
0x400 0x014d8299  0x007f80ff  0x007f80ff  0x007f80ff
0x410 0x027f80ff  0x007f80ff  0x0ffe00f8  0x000000f8
0x420 0xff311f00  0x9a30f8f8  0x3030100a  0x00004000
0x430 0x00000000  0x04030201  0x04030201  0x08070504
0x440 0x0080015f  0x00000010  0xfffff000  0x00000010
0x450 0x003ff000  0xf6700005  0x00003fff  0x1000f800
0x460 0x03084444  0x00ff00ff  0x007f80ff  0x007f80ff
0x470 0x007f80ff  0x007f80ff  0x00000000  0x06060001
0x480 0x000c0400  0x00000000  0x00000000  0x00000000
0x490 0x00000000  0x00000000  0x00000000  0x00000000
0x4a0 0x00000000  0x00000000  0x00000000  0x12010200
0x4b0 0x40404040  0x00000000  0x00000000  0x000400c0
0x4c0 0x04000400  0x80040340  0x0c1f08ff  0x0201ff7f
0x4d0 0x00000000  0x00000000  0x00000000  0x000002bc
0x4e0 0x06000000  0x25240c0c  0x06060606  0x00000000
0x4f0 0x0000c100  0x00200001  0x00000000  0xffff0000
0x500 0x002f3222  0x005e4322  0x005ea42b  0x0000a44f
0x510 0x2800660f  0x100a100a  0x0914fe00  0x00000000
0x520 0x00003f0f  0x21ff4f0f  0x806c0a8f  0x00000000
0x530 0x00000000  0x00000000  0x00000000  0x00000000
0x540 0x80006404  0x00400180  0x00000000  0x00000000
0x550 0x00001009  0x00640064  0x00020202  0x0f0fff28
0x560 0x21f6007c  0x000000a6  0x1f77a1b7  0x00000000
0x570 0x000a0002  0x03ff1000  0x00000000  0x00000000
0x580 0x00000000  0x00000000  0x00000000  0x06200060
0x590 0x300f000f  0x00000000  0x00000000  0x00000000
0x5a0 0x02020202  0x00010202  0x00000000  0x00000000
0x5b0 0x00000000  0x00000000  0x00000000  0x00000000
0x5c0 0x00000000  0x00000000  0x00000000  0x11e21051
0x5d0 0x00000000  0x00000000  0x00000000  0x00000000
0x5e0 0x00001000  0x00003000  0x00001000  0x00000003
0x5f0 0x00000000  0x00000000  0x00000000  0xffff0000
0x600 0x00000000  0x00003000  0x700060ce  0x04000418
0x610 0x27388102  0x0000cd88  0x7e564d9a  0x00004f81
0x620 0xffffffff  0xffffffff  0x00000000  0x00000000
0x630 0x00000000  0x00000000  0x100a2828  0x0a0a0808
0x640 0x00401440  0x00000000  0x00000000  0x00000000
0x650 0x00eb0426  0x00000000  0xffffffff  0xffffffff
0x660 0x00b40010  0xa0000000  0x0e301000  0x00052000
0x670 0x0000007d  0x4d9a8051  0x07769d77  0x0000c004
0x680 0x0000010c  0x00000000  0x00000000  0x00000000
0x690 0x000f0020  0x000a0200  0x00000000  0x00000000
0x6a0 0x0400ffff  0x0000ffff  0x00000000  0x00000000
0x6b0 0x00000000  0x00000000  0x00010008  0x00000000
0x6c0 0x00000000  0x00000000  0x00000000  0x00000000
0x6d0 0x00000000  0x00000000  0x0000003f  0x00000000
0x6e0 0x7e564d9a  0x00004f81  0x7e564d9a  0x00004f81
0x6f0 0x00011404  0x00000000  0x00000000  0x00000000
0x700 0x87654321  0x00000000  0x87654321  0x00000000
0x710 0x00000000  0x00000000  0x20000200  0x00000000
0x720 0x00000000  0x00000000  0x00000000  0x00000000
0x730 0x00000000  0x00000000  0x00000000  0x00000000
0x740 0x00000000  0x00000000  0x00000000  0x00000000
0x750 0x00000000  0x00000000  0x00000000  0x00000000
0x760 0x00000000  0x00000000  0x00000000  0x00000000
0x770 0x00000000  0x00000000  0x00000000  0x00000000
0x780 0x00000000  0x00000000  0x00000000  0x00000000
0x790 0x00000000  0x00000000  0x00000000  0x00000000
0x7a0 0x00000000  0x00000000  0x00000000  0x00000000
0x7b0 0x00000000  0x00000000  0x00000000  0x00000000
0x7c0 0x00000000  0x00000000  0x00000000  0x00000000
0x7d0 0x00000000  0x00000000  0x00000000  0x00000000
0x7e0 0x00000000  0x00000000  0x00000000  0x00000000
0x7f0 0x00000000  0x00000000  0x00000000  0x00000000

======================================================================
# ДОКУМЕНТ: W233-SDIO-TRACE.md
======================================================================

# SDIO-трейс W.233 (полный доступ к чипу RTL8189FTV, bare-metal H3)

> Снято прошивкой W.233 (WIFI_SDIO_TRACE в src/h3_sdc.c) с Orange Pi Lite
> (FEL), чип RTL8189FTV на SDC1 (PG0-PG5). Каждая строка — одна CMD52/CMD53:
>   <52R f{fn} r0x{reg} ->0x{val} rc0   (CMD52 чтение)
>   <52W f{fn} r0x{reg} =0x{val} rc0    (CMD52 запись)
>   <53R f{fn} b0x{addr} l{len} rc0     (CMD53 чтение, byte-mode)
>   <53W f{fn} b0x{addr} l{len} rc0     (CMD53 запись, byte-mode)
>   суффикс "ni" = noinc (без автоинкремента).
> Назначение: сравнение с эталонным поадресным логом Armbian (Linux + rtl8189fs)
> — где наша последовательность расходится (шина/такт/регистры).

## Ключевые точки лога (что проверить в Armbian)

- Ширина шины: весь лог на **1-bit** (CCCR 0x07=0x00, CTYPE=0; после FW 4-bit
  отключён — в W.230 переход на 4-bit ломал data-фазу).
- Такт: 6 МГц (24M/2/2, N=0,M=2) — не 400 кГц init-частоты Linux.
- pwon: 0x05/0x06/0x05/0x27 — WRITE-шаги (поллинги НЕ сходятся на этом чипе).
- fwdl: 5 страниц (0x1000..0x4950), tail l2; MCUFWDL 0x80: 5->4->6->C6 (FW READY).
- BB: 209 рег. (0x100, 0x800..0xF4C); AGC: 66 рег. 0xC78/0xC50;
  RF: 136 рег. через LSSI (0x840 write, 0x824 edge, 0x8B8/0x8A0 read).
- LCK: 50 мс (по трейсу), RF 0x18=0x401, RF 0x00=0x33D95.
- RX: IGI 0x1E (0xC50), MSR=NOLINK, H2C 0x20 (0x1D0/0x1F0/0x1CC), CCCR IENM/FBR,
  RCR 0x7000604E, RRSR=0xFFF, RXFLTMAP, RQPN=0x80E0020C, LLT, RDG.
- Свип: каналы 1/6/11, но cca=0x8CCD8300 СТАТИЧЕН, rxlen/pkt=0, hisr=0.
- TX probe: hiq_free=0 -> rc=-2 (TX FIFO HIQ не выделен).

## Что ищем в эталоне (Armania/8189fs)
1. Реальную ширину/частоту шины после init (dtb bus-width=<4>, но фазы/такm).
2. pwon: поллинги или WRITE?
3. Есть ли где-то в Linux путь 0x0800, 0x0874 (RFMOD), 0x0522 (TXPAUSE) ДО LCK.
4. Инициализация TX FIFO (rsvd pages / HIQ) до probe.
5. Что ставится в 0x0A08 (cca), 0x0C4C (ofdm) — наши ли регистры.

======================================================================
# ДОКУМЕНТ: WIFI-FW-DOWNLOAD.md
======================================================================

# RTL8189FTV: PowerSeq и загрузка прошивки (справка для bare-metal)

> Источники: драйвер `tools/wifi/rtl8189fs/` (Realtek vendor v4.3.24, `CONFIG_RTL8188F`+`CONFIG_SDIO_HCI`),
> образ `fw_nic.bin`, код `src/h3_sdc.c`.
> Дата: 2026-10-16. Все утверждения ниже сверены с исходниками, ссылки даны в виде `файл:строка`.
> Связанный документ: `HARDWARE-WIFI.md` (схема, SDC1, WIFI_EN=PL7).

## 1. Адресация и примитивы

```
SDIO_LOCAL_DEVICE_ID = 0   маска 0x0FFF    (SDIO_LOCAL_BASE 0x10250000)
WLAN_IOREG_DEVICE_ID = 8   маска 0x7FFF    (MAC-регистры)
адрес для CMD52/53 = (device_id << 13) | (offset & mask)
```
Определения: `include/hal_com_reg.h:1563-1585`.

| Что | Адрес | Как в драйвере | Как в `src/h3_sdc.c` |
|---|---|---|---|
| MAC-регистр (0x80, 0x82) | `(8<<13)`&#124;`reg` | `sd_cmd52_*` → `sdio_readb/writeb` (`os_dep/linux/sdio_ops_linux.c:115-264`) | `pseq_read8/pseq_write8` base=0 (`:430-445`) |
| 32-бит чтение/запись MAC | `(8<<13)`&#124;`reg` | `sd_read32/sd_write32` → `sdio_readl/writel` (`:369-503`, `:572-700`) | `h3_sdc_read_wlan_reg32/write_wlan_reg32` (`:361-381`) |
| Блочный доступ (FIFO) | `(4..7<<13)`&#124;`off` | `sd_read/sd_write` → `sdio_memcpy_fromio/toio` (`:709-909`) | `h3_sdc_cmd53_read/write` (`:2181`, `:2196`) |
| SDIO-local (0x86, 0x100 в PwrSeq) | `(0<<13)`&#124;`off` | `SdioLocalCmd52Read1Byte/Write1Byte` (`hal/rtl8188f/sdio/sdio_ops.c:1051, :1113`) | `pseq_read8/pseq_write8` base=3 |

Идентификатор SDIO-устройства драйвера — `0x024c:0xF179` (`os_dep/linux/sdio_intf.c:69`), блок Fn1 ставится 512 байт (`:269`), `psdio->func` — это **function 1** (`:382`).

> Расхождение: драйвер и к SDIO-local ходит через function 1 (тот же `sd_cmd52_*` с `psdio->func`).
> В нашем коде зафиксировано (`src/h3_sdc.c:428`), что SDIO-local регистры отвечают только на function 0
> (проверено на 4-bit/6 МГц шине).

## 2. Power-on: CARDEMU → ACT

Вход в драйвере: `_InitPowerOn_8188FS` (`hal/rtl8188f/sdio/sdio_halinit.c:153`) → `CardEnable` (`:186`) +
`PowerOnCheck` (`:202`); `hal_power_on = &_InitPowerOn_8188FS` назначен в `:1837`.
`CardEnable` сначала пишет `REG_RSV_CTRL (0x1C) = 0x00` — разблокировка ISO/CLK/Power (`:45`), затем парсит
`rtl8188F_card_enable_flow` = `CARDDIS_TO_CARDEMU` + `CARDEMU_TO_ACT` + `END` (`:49`, `hal/rtl8188f/Hal8188FPwrSeq.c:44-48`).

`RTL8188F_TRANS_CARDEMU_TO_ACT` — 7 шагов (`include/Hal8188FPwrSeq.h:40-49`):

| # | offset | base | cmd | mask | value | смысл |
|---|---|---|---|---|---|---|
| 1 | 0x0005 | MAC | WRITE | BIT2 | 0 | disable SW LPS (0x04[10]=0) |
| 2 | 0x0006 | MAC | POLLING | BIT1 | BIT1 | ждать power ready (0x04[17]) |
| 3 | 0x0005 | MAC | WRITE | BIT7 | 0 | disable HWPDN (0x04[15]) |
| 4 | 0x0005 | MAC | WRITE | BIT3 | 0 | disable WL suspend (0x04[11]) |
| 5 | 0x0005 | MAC | WRITE | BIT0 | BIT0 | 0x04[8]=1 |
| 6 | 0x0005 | MAC | POLLING | BIT0 | 0 | ждать сброса бита |
| 7 | 0x0027 | MAC | WRITE | 0xFF | 0x35 | xtal_qsel=1, меньше RF-шума |
| — | 0xFFFF | — | END | — | — | конец |

Семантика интерпретатора (`hal/HalPwrSeqCmd.c:48-181`):
- WRITE — `v = read8(off)`, `v = (v & ~mask) | (value & mask)`, `write8`;
- POLLING — цикл до `(v & mask) == (value & mask)`, пауза 10 мкс, лимит **5000** итераций;
- DELAY — `udelay(offset)` при `value = PWRSEQ_DELAY_US`, иначе `offset * 1000` мкс;
- BASE=SDIO читается/пишется через SDIO-local (device id 0), остальное — MAC (device id 8);
- шаг пропускается, если не совпали FAB / CUT / Interface (у нас всегда `PWR_CUT_ALL` / `PWR_FAB_ALL` / `PWR_INTF_SDIO`).

## 3. Загрузка прошивки

Тело драйвера: `rtl8188f_FirmwareDownload` (`hal/rtl8188f/rtl8188f_hal_init.c:1077`).

```
0) предпроверка: если 0x80 & RAM_DL_SEL(bit7) → 0x80 = 0x00; _8051Reset8188()        :1241-1244
1) _FWDownloadEnable(TRUE)                                                          :29-63
   a) 0x03 (REG_SYS_FUNC_EN+1) |= 0x04            /* 8051 enable */
   b) 0x80 |= 0x01; поллинг 0x80.bit0 (msleep 1 мс, до 100 попыток)
   c) 0x82 (REG_MCUFWDL+2) &= 0xF7                /* 8051 reset  */
2) сброс контрольной суммы: 0x80 |= FWDL_ChkSum_rpt(bit2)                           :1251
3) _WriteFW → постранично, страница MAX_DLFW_PAGE_SIZE = 4096                       :216-254
   0x82 = (read8(0x82) & 0xF8) | (page & 0x07)     /* выбор страницы RMAC/IROM */   :190-191
   _BlockWrite (адреса от FW_8188F_START_ADDRESS = 0x1000):                         :65-177
     P1: 4-байтные слова rtw_write32(FW_START + i*4)                                :102-112
     P2: 8-байтные блоки — ТОЛЬКО под CONFIG_USB_HCI, для SDIO фаза отсутствует     :146-153
     P3: остаток (1..3 байта) — побайтно rtw_write8(FW_START + offset + i)          :156-174
4) поллинг chksum: rtw_read32(0x80) & FWDL_ChkSum_rpt (min 5 попыток / 50 мс)       :303-332, :1257
   шаги 2-4 повторяются: до 3 раз или 500 мс                                        :1248-1260
5) _FWDownloadEnable(FALSE): 0x80 &= ~0x01                                           :58-62, :1324
6) _FWFreeToGo (min 10 попыток / 200 мс)                                            :334-373, :1326
   a) 0x80 |= MCUFWDL_RDY(bit1); 0x80 &= ~WINTINI_RDY(bit6)
   b) _8051Reset8188 (порядок ровно такой): 0x1D &= ~BIT0 → 0x03 &= ~BIT2
                                           → 0x1D |= BIT0 → 0x03 |= BIT2            :264-301
   c) поллить 0x80 до (MCUFWDL_RDY | FWDL_ChkSum_rpt | WINTINI_RDY | RAM_DL_SEL)
7) успех: 0x80 == want-маска; при FAIL драйвер снимает 0x80 биты 0 и 1                :1333-1338
```

Константы и биты:

| Имя | Значение | Где |
|---|---|---|
| `REG_MCUFWDL` | 0x0080 | `include/hal_com_reg.h:90`, `hal/phydm/rtl8188f/hal8188freg.h:71` |
| `REG_SYS_FUNC_EN` | 0x0002 (регистр 8051 — +1 = 0x03) | `include/hal_com_reg.h:43` |
| `REG_RSV_CTRL` | 0x001C (IO wrapper — +1 = 0x1D) | `include/hal_com_reg.h:55` |
| `MCUFWDL_RDY` | BIT(1) | `hal_com_reg.h:1212`, `hal8188freg.h:603` |
| `FWDL_ChkSum_rpt` | BIT(2) | `hal_com_reg.h:1213`, `hal8188freg.h:604` |
| `WINTINI_RDY` | BIT(6) | `hal_com_reg.h:1217`, `hal8188freg.h:608` |
| `RAM_DL_SEL` | BIT(7) | `hal_com_reg.h:1218`, `hal8188freg.h:609` |
| `MAX_DLFW_PAGE_SIZE` | 4096 | `include/hal_com.h:205` |
| `FW_8188F_START_ADDRESS` | 0x1000 | `include/rtl8188f_hal.h:72` |
| `FW_8188F_SIZE` | 0x8000 | `include/rtl8188f_hal.h:71` |

## 4. Заголовок образа (проверено на `fw_nic.bin`)

32 байта, `IS_FW_HEADER_EXIST_8188F` = `(Signature & 0xFFF0) == 0x88F0` (`include/rtl8188f_hal.h:75, :91-117`);
заголовок в IRAM не пишется, драйвер сдвигает указатель на 32 байта (`rtl8188f_hal_init.c:1225-1230`).

Данные нашего `fw_nic.bin` (18802 байта, первые 40 байт):

```
f1 88 10 00 01 00 07 00 10 22 17 28 52 49 02 00 e7 26 00 00 00 00 00 00 00 00 00 00 00 00 00 00 | 02 86 ad 02 ...
```

| Поле | Байты | Значение |
|---|---|---|
| Signature | 0 f1 88 | 0x88F1 → маска 0x88F0, заголовок есть |
| Category / Function | 2 .. 3 | 0x10 / 0x00 |
| Version / Subversion | 4 .. 7 | 0x0001 / 0x0007 |
| Month / Date / Hour / Minute | 8 .. 11 | 0x10 / 0x22 / 0x17 / 0x28 (BCD: 10-22, 17:28) |
| RamCodeSize | 12 .. 13 | 0x4952 = **18770** |
| SvnIdx | 16 .. 19 | 0x000026e7 |

Контроль: `18802 − 32 = 18770` = `RamCodeSize`, и `18770 = 4 * 4096 + 2386`, `18770 mod 4 = 2` — то есть хвост
последней страницы составляет **2 байта** (пишутся байтовым путём P3 на адрес `0x1000 + 2384`).

## 5. Расхождения в `src/h3_sdc.c` (что поправить)

1. `h3_wifi_download_fw` (`:1953`) — хвост страницы. Драйвер пишет остаток 1..3 байта через `rtw_write8`
   (CMD52 побайтно, `rtl8188f_hal_init.c:156-174`), а наш код дополняет нулями и пишет 4 байта CMD53
   (`src/h3_sdc.c:2077-2088`). Для `fw_nic.bin` это ровно 2 байта в последней странице — первый кандидат на
   `rc=-19/-39` на границе страниц и на несовпадение контрольной суммы.
2. Нет цикла повтора записи+chksum: у драйвера до 3 попыток / 500 мс (`:1248-1260`).
3. Отсутствует шаг 0 (`RAM_DL_SEL` → `0x80=0x00` + `_8051Reset8188`) — нужен при повторной загрузке
   (после reset, второй прогон, «FW exist before power on»).
4. `h3_wifi_power_on_chip` (`:515`) — оба POLLING-шага (№2 `0x06.bit1`, №6 `0x05.bit0`) заменены на WRITE
   (`:526-527`, `:534-535`). Лучше вернуть POLLING с ограниченным таймаутом: ветка уже есть в `pseq_exec`
   (`:485-502`) вместе с диагностикой в `sdc_diag_pseq_*`.
5. `pseq_read8/pseq_write8` для base=3 используют function 0 (`:432, :441`), драйвер — function 1
   (проверено на 4-bit/6 МГц шине: SDIO-local отвечают на function 0).

## 6. Диагностика

- Перед PowerSeq полезно проверить состояние питания чипа, как драйвер (`rtl8188f_hal_init.c:1115-1130`):
  `0xA3[2:0] = 0b010`, затем `0xA0[1:0] == 0x01`. Если не совпало — чип не в CARDEMU/ACT, и шаги 2/6
  вернуться не могут (проблема в `WIFI_EN`/питании, а не в PwrSeq).
- Шина: в драйвере `sdio_set_block_size(func, 512)` (`sdio_intf.c:269`) — 512 соответствует нашему коду.
- После загрузки: `0x80` должен содержать `RDY|ChkSum|WINTINI|RAM_DL_SEL`; иначе смотреть `sdc_diag_fw_mcufwdl`.

## 7. Чек-лист последовательности

```
1.  WIFI_EN (PL7) = 1, пауза, такт SDC1
2.  CMD5 → CMD3 → CMD7; CMD52 fn0: CCCR 0x07 (ширина шины), CIS
3.  Fn1 block size = 512 (CMD52 fn0 0x110 = 0x00, 0x111 = 0x02)
4.  PowerSeq: 0x1C = 0x00, затем CARDEMU→ACT (7 шагов + END)
5.  Контроль питания: 0xA3[2:0] = 0b010, 0xA0[1:0] = 0x01
6.  RAM_DL_SEL pre-check: 0x80 &= ~BIT(7) + _8051Reset8188
7.  _FWDownloadEnable(1): 0x03 |= 0x04; 0x80 |= 0x01 (поллинг bit0); 0x82 &= 0xF7
8.  0x80 |= BIT(2); страницы по 4096: 0x82 = (read & 0xF8) | page,
    P1 — 4-байтные слова от 0x1000, хвост — побайтно CMD52
9.  Поллинг 0x80.bit2 (chksum); при неудаче повторить запись (до 3 раз / 500 мс)
10. _FWDownloadEnable(0): 0x80 &= ~0x01
11. FreeToGo: 0x80 |= BIT(1), &= ~BIT(6); reset 8051 (0x1D.bit0, 0x03.bit2); поллинг want-маски
12. Далее: efuse (MAC не читать из efuse — см. HARDWARE-WIFI.md), MAC/BB/RF init
```


======================================================================
# ДОКУМЕНТ: I2S-REFERENCE.md
======================================================================

# I2S и звук в эталоне Lakka (Allwinner) — путь целиком

> Эталон: распакованный образ `ref/Lakka-Allwinner.orangepi_lite.arm-2.3.1.image`.
> Дополнительно: `ref/Lakka-LibreELEC-Lakka-v6.1` — исходник сборочной системы
> (патчи/опции, по нему смотрели путь ALSA/звука).
> Формулы делителей живут в драйвере ядра
> (`sound/soc/sunxi/sun4i-i2s.c`, `drivers/clk/sunxi-ng/*`), которого в дереве сборки нет: в эталоне
> тактирование задаётся через DTS + CCU-фреймворк, а не константами в этом репозитории.

## 1. Где это живёт (состав пути)

| Звено | Файл | Что задаёт |
|---|---|---|
| Опции дистрибутива | `distributions/Lakka/options:165-166` | `ALSA_SUPPORT="yes"` |
| | `distributions/Lakka/options:101-102` | `PULSEAUDIO_SUPPORT="no"` |
| | `distributions/Lakka/options:104-105` | `PIPEWIRE_SUPPORT="no"` |
| Ядро (Allwinner) | `projects/Allwinner/linux/linux.arm.conf:4317` | `CONFIG_SND=y` |
| | `…/linux.arm.conf:4504-4510` | `SND_SUN4I_CODEC`, `SND_SUN8I_CODEC`, `SND_SUN8I_CODEC_ANALOG`, `SND_SUN4I_I2S`, `SND_SUN4I_SPDIF`, `SND_SUN8I_ADDA_PR_REGMAP` = y; `SND_SUN50I_DMIC` — нет |
| | `…/linux.arm.conf:4662` | `CONFIG_SND_SOC_SPDIF=y` |
| | `…/linux.arm.conf:4621` | `# CONFIG_SND_SOC_MAX98357A is not set` (внешних I2S-ЦАП нет) |
| DTS-патчи | `projects/Allwinner/patches/linux/0019…0023` | звуковые карты: HDMI (H3/H5, A64, R40, H6) и аналоговый кодек (R40) |
| ALSA-профили | `projects/Allwinner/filesystem/usr/share/alsa/cards/` | `H3_Audio_Codec.conf`, `simple-card.conf`, `sun50i-a64-audi.conf` |
| Пакеты | `packages/audio/alsa-lib`, `alsa-utils`, `alsa-topology-conf`, `alsa-ucm-conf` | библиотека, утилиты, UCM/topology |
| Настройка микшера | `packages/audio/alsa-utils/scripts/soundconfig` | `alsactl restore` / набор `amixer sset` при появлении карты |
| | `packages/audio/alsa-utils/udev.d/90-alsa-restore.rules`, `alsa-utils/package.mk` | своё правило вместо штатного (штатное удаляется) |
| Приложение | `packages/lakka/retroarch_base/retroarch/config/retroarch.cfg:16-40` | `audio_driver="alsathread"`, `audio_device=""`, `audio_out_rate="48000"`, `audio_latency="64"`, `audio_resampler="sinc"`, `audio_sync="true"` |

## 2. Тактирование

Задаётся в DTS (патчи эталона), а не в build-опциях:

- I2S-контроллер: `clocks = <&ccu CLK_BUS_I2S2>, <&ccu CLK_I2S2>; clock-names = "apb", "mod"; resets = <&ccu RST_BUS_I2S2>`
  (`0019-HACK-h3-h5-Add-HDMI-sound-card.patch:42-46`) — то есть шинный (APB) и модульный такт + сброс из CCU.
- Экземпляр DAI: H3/H5/A64/R40 → `i2s2` (`i2s@1c22800`), H6 → `i2s1` (`i2s@5091000`, `0023:31,41`).
- HDMI-карта: `simple-audio-card,mclk-fs = <128>` (`0019:23`) — MCLK = 128 × fs (6.144 МГц при 48 кГц), плюс
  `frame-inversion` (`0019:24`) и TDM-слоты `dai-tdm-slot-num = <2>`, `dai-tdm-slot-width = <32>` (`0019:32-33`).
- Аналоговый кодек: `clocks = <&ccu CLK_BUS_CODEC>, <&ccu CLK_CODEC>`, `resets = <&ccu RST_BUS_CODEC>`,
  DMA `dmas = <&dma 19>, <&dma 19>`, `dma-names = "rx","tx"` (`0021-dts-enable-analog-codec.patch:44-49`).
- SPDIF: `CONFIG_SND_SUN4I_SPDIF=y` (`linux.arm.conf:4508`), карта описана профилем `simple-card.conf`
  («On-board SPDIF» → `sun50i-h6-spdif`).
- DMA I2S-карты: `dmas = <&dma 27>, <&dma 27>; dma-names = "rx","tx"` (`0019:44-49`) — канал DMAC 27, отдельные
  дескрипторы на приём и передачу.

## 3. Последовательность (от опции до звука)

```
сборка: options (ALSA_SUPPORT) → CONFIG_SND_* в linux.<arch>.conf → DTS-патчи (sound{}-узлы)
        → пакеты audio/* → ALSA-профили в /usr/share/alsa/cards/
загрузка: DT → драйверы (sun4i-i2s как DAI, sun8i-codec / HDMI как codec)
        → ASoC `sound {}` (simple-audio-card) регистрирует карту
        → ALSA core создаёт PCM-устройства
        → udev-правило запускает soundconfig → alsactl restore / amixer sset (микшер и маршрутизация)
        → приложение открывает PCM (RetroArch: `alsathread`, 48 кГц)
```

Порядок внутри драйверов ядра (probe → clk enable → regmap → DAI ops) в дереве сборки не представлен:
эталон только выбирает, какие узлы включены в DTS и какие опции ядра собраны.

## 4. Что играет роль «идентификатора» потока

- Карта: имя из DTS — `simple-audio-card,name = "allwinner-hdmi"` (`0019:22`); аналоговая карта описывается
  профилем alsa-lib с именем файла (`H3_Audio_Codec.conf`).
- Связка DAI↔codec: `simple-audio-card,cpu { sound-dai = <&i2s2>; … }` и
  `simple-audio-card,codec { sound-dai = <&hdmi>; }` (`0019:26-34`); для аналога —
  `allwinner,codec-analog-controls = <&codec_analog>` (`0021:48`).
- Микшер-элементы как «подпись» карты: `Line Out Playback Volume/Switch` для H3-аналога,
  `IEC958 Playback Default` для HDMI/SPDIF (`H3_Audio_Codec.conf`, `simple-card.conf`).

## 5. Настройки — где и какие

| Уровень | Где принимается | Примеры |
|---|---|---|
| Дистрибутив | `distributions/Lakka/options` | `ALSA_SUPPORT=yes`, `PULSEAUDIO_SUPPORT=no`, `PIPEWIRE_SUPPORT=no` |
| Ядро | `projects/Allwinner/linux/linux.{arm,aarch64}.conf` | `SND_SUN4I_I2S=y`, `SND_SUN8I_CODEC=y`, `SND_SUN4I_SPDIF=y`, MAX98357A выключен |
| DTS | `projects/Allwinner/patches/linux/00{19..23}*.patch` | `format="i2s"`, `mclk-fs`, `frame-inversion`, TDM-слоты, `sound-dai`, `audio-routing`, `dmas`/`dma-names`, `status` |
| ALSA-профили | `…/filesystem/usr/share/alsa/cards/*.conf` | `hooks.0.ctl_elems`: `Line Out Playback Volume=31`, `Line Out Playback Switch=[on on]`, `IEC958 Playback Default` |
| Микшер при загрузке | `packages/audio/alsa-utils/scripts/soundconfig` | если есть `/storage/.config/sound.conf` → `alsactl restore -f`; иначе набор `amixer sset` (для Allwinner H3 Analog: `Line Out 0db on`) |
| Приложение | `packages/lakka/retroarch_base/retroarch/config/retroarch.cfg` | `audio_driver="alsathread"`, `audio_out_rate="48000"`, `audio_latency="64"`, `audio_resampler="sinc"` |
| Отдельные проекты/устройства | `projects/L4T/devices/Switch/packages/alsa-utils/*`, `projects/NXP/devices/iMX6/filesystem/usr/bin/rdu-audio`, `projects/Rockchip/…/headphone-sense.c` | примеры, как проект переопределяет пакет и добавляет свои действия со звуком |

## 6. Слои взаимодействия

```
железо: I2S-контроллер SoC / HDMI-APHY / аналоговый кодек
 └ CCU: CLK_BUS_I2Sn (apb) + CLK_I2Sn (mod) + RST_BUS_I2Sn         [DTS: clocks/resets]
    └ драйвер DAI sun4i-i2s (sound/soc/sunxi, вне дерева сборки)
       └ ASoC: sound{} = simple-audio-card (codec <&hdmi|&codec>, cpu <&i2s2|&i2s1>)
          └ ALSA core: PCM-устройства hw:CARD,DEV
             └ alsa-lib + профили cards/*.conf  →  приложение (RetroArch: alsathread, 48 кГц)
      DMA: dmas = <&dma 27> (I2S2), <&dma 19> (кодек), dma-names "rx","tx"
      udev → soundconfig → amixer/alsactl: микшер и маршрутизация после появления карты
```

## 7. Проверено / не проверено

Проверено по исходникам эталона:
- опции Lakka (ALSA включён, PulseAudio и PipeWire выключены) — `distributions/Lakka/options:101-105,165-166`;
- состав звуковых опций ядра Allwinner и отсутствие MAX98357A — `linux.arm.conf:4504-4510,4621,4662`;
- содержимое DTS-патчей 0019 (полностью) и 0021 (узлы `codec`/`codec_analog`, `audio-routing`);
- профили `H3_Audio_Codec.conf` и `simple-card.conf` (HDMI/IEC958, SPDIF H6);
- `alsa-utils`: удаление штатного `90-alsa-restore.rules` и установка своего `soundconfig` (`package.mk`, `post_makeinstall_target`), плюс сохранённые для Lakka утилиты (`aplay`, `arecord`, `alsamixer`, `iecset` и др.);
- аудио-настройки RetroArch в дистрибутиве Lakka.

Не проверено (нет исходников в дереве сборки): конкретные формулы делителей MCLK/BCLK/LRCK в
`sun4i-i2s.c` и `sunxi-ng`; поведение HDMI-APHY; точное соответствие имени карты и профиля alsa-lib
для аналогового кодека H3 (профиль назван по файлу — сверить на живой системе через `aplay -l`).


======================================================================
# ДОКУМЕНТ: NEXT-CHAT-PROMPT.md
======================================================================

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
