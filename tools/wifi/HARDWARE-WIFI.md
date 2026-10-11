# WiFi (RTL8189FTV) на Orange Pi Lite — аппаратная справка

> Источник: схема платы `Orange_pi-lite-v1_1.pdf` (в корне проекта) + даташит H3.
> Дата: 2026-10-11. Этап проекта: поднимаем WiFi.

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

Для общения с чипом по SDIO нужен хост-контроллер SDC1 (0x01C11000):
- Такт/сброс: CCU GATING/RST биты SDC1 (проверить в даташите H3).
- Регистры хоста — как у SD/MMC (CMD, ARG, STATUS, RCTL, BLK, DT, FIFO).
- Перед чтением чипа: подать WIFI_EN=1 (PL7), дать время на стабилизацию,
  затем выполнить SDIO CMD52/CMD53 (шина, функция 0/1).

Далее: чтение CCID (Card Information) чипа по SDIO — первый контрольный результат,
что чип отвечает. Затем — загрузка firmware (halhwimg8188f_fw.c) и ассоциация.

---

## Статус работ (2026-10-11)

### Готово
- Схема платы изучена: чип RTL8189FTV, SDC1 (SD/MMC1 = 0x01C10000), PG0-5, WIFI_EN=PL7.
- Скачан драйвер rtl8189fs (Linux) в `tools/wifi/rtl8189fs/` — источник прошивки,
  power-seq и регистров чипа.
- Написан базовый драйвер SD/MMC-хоста `src/h3_sdc.c` + `include/h3_sdc.h`:
  инициализация тактов/пинов/сброса, низкоуровневые команды,
  блочная передача данных. Сборка чистая.

### Следующий шаг
- Подключить `h3_sdc1_init()` в kmain, подать WIFI_EN, выполнить SDIO CMD5
  (IO_SEND_OP_COND) и CMD52/CMD53 — прочитать ID чипа (первый контрольный выстрел).

### Статус работ (2026-10-11) — обновлено
- Готово меню: главное меню (Sound test / Settings) -> подменю частот (440/697/1209).
  Индикатор WIFI ON/OFF на главном экране, значок в шапке. Автоинкремент версии
  (FW vW.011).
- SDC1 инициализация + SDIO CMD5/CMD3/CMD7/CMD52 реализованы в коде.
- ПЕРВЫЙ ТЕСТ: `sdio_init rc=-1` — CMD5 не ответил. Надо отладить: скорость SDC1,
  питание чипа (WIFI_EN/PL7), повод. тайминги, порядок CMD5. Начать с чтения
  состояния регистров SDC1 после h3_sdc1_init() через JTAG, и сравнить с эталоном
  после U-Boot (GCTL/CKCR/CTYPE/STAR).

### Снимок JTAG 2026-10-11 — после U-Boot (SD), до нашей инициализации
- SDC1: GCTL(0x00)=0x20000000, CKCR=0, TMOR=0xffffff40, BLKSZ=0x200, BYCNT=0x200.
- Порт G: cfg0=0x77222222 (PG0-3 уже function 2 SDC1!), cfg1=0x00777700 (PG4-5 = 0, не SDC1).
- Порт L: data=0x400 (PL10 LED вкл), **WIFI_EN (PL7)=0 -> WiFi-чип выключен**.
- SDMMC1_CLK (0x8C)=0x80500000 (SCLK_GATING on, CLK_SRC=PLL_PERIPH0), GATING0 bit9=1, RST0 bit9=1.
- Вывод: U-Boot уже настроил PG0-3 (SDC1) и такт MMC1, но НЕ включил WiFi-чип (PL7=0).
  Наша инициализация обязана: включить WIFI_EN (PL7=1), проверить/настроить PG0-5,
  такт/сброс MMC1 — не полагаться на сделанное U-Boot.

### Снимок JTAG (FEL, jtagonly) 2026-10-11 — чистое состояние
- SDC1 (0x01C10000) все регистры = 0 (контроллер не инициализирован).
- Порт G: cfg0=0x77777777, cfg1=0x00777777, data=0 (PG0-5 в function 7, не SDC1).
- Порт L: cfg0=0x77777722 (PL0-3 = fn2, PL7 = fn7, не GPIO), data=0 -> **WIFI_EN (PL7)=0, чип выключен**.
- SDMMC1_CLK (0x08C)=0, GATING0 bit9 (MMC1)=0, RST0 bit9=0 — такты MMC1 выключены.
- Вывод: всё в сбросе (чистое состояние). Для работы WiFi нужно: PL7=1 (WIFI_EN),
  настроить PG0-5 на function 2, включить такты/сброс MMC1, затем читать чип по SDIO.

### Драйвер rtl8189fs (структура, что брать)
- `hal/phydm/rtl8188f/halhwimg8188f_fw.c` — загрузочная прошивка чипа (голые байты).
- `include/Hal8188FPwrSeq.h` + `hal/rtl8188f/Hal8188FPwrSeq.c` — power sequence
  (аргументы: offset, маска, значение, команда write/poll/delay).
- `hal/rtl8188f/rtl8188f_hal_init.c` — основная инициализация чипа.
- `hal/rtl8188f/sdio/sdio_ops.c`, `sdio_halinit.c` — SDIO-протокол (CMD52/53, адреса FIFO).
- `include/hal8188freg.h` — регистры чипа.

### SDIO-команды (напоминание)
- CMD52 — байтовый доступ к функциям (R/W register).
- CMD53 — блочный обмен (данные чипа).
- CMD5 — I/O send op cond (детект SDIO-карты).
- CMD3/7 — выбор карты (SEL/DESELECT).