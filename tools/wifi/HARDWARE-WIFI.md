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

Для общения с чипом по SDIO нужен хост-контроллер SDC1 (0x01C10000):
- Такт/сброс: CCU GATING/RST биты SDC1 (проверить в даташите H3).
- Регистры хоста — как у SD/MMC (CMD, ARG, STATUS, RCTL, BLK, DT, FIFO).
- Перед чтением чипа: подать WIFI_EN=1 (PL7), дать время на стабилизацию,
  затем выполнить SDIO CMD52/CMD53 (шина, функция 0/1).

Далее: чтение CCID (Card Information) чипа по SDIO — первый контрольный результат,
что чип отвечает. Затем — загрузка firmware (halhwimg8188f_fw.c) и ассоциация.

---

## Статус работ (2026-10-11) — актуально, FW vW.040+

### Готово (проверено на стенде)
- Схема платы изучена: чип RTL8189FTV, SDC1 (SD/MMC1 = 0x01C10000), PG0-5, WIFI_EN=PL7.
- Драйвер rtl8189fs (Linux) — в `tools/wifi/rtl8189fs/` (референс, не в репозитории):
  прошивка, power-seq, регистры чипа.
- Собственный SDC1-драйвер `src/h3_sdc.c` + `include/h3_sdc.h`.
- SDIO-обмен работает: CMD5→CMD3→CMD7, CMD52 (обе функции), CMD53-блок-запись.
- Чтение CIS: адрес 0x1000, тюпл 0x20/0x02 → Manufacturer 0x024C (Realtek).
- 4-bit режим: CCCR 0x07 [2:0]=010 + SDC_CTYPE=1.
- Power-on чипа: PowerSeq CARDEMU→ACTIVE, чип просыпается (sdio100=0x07).
- Первая загрузка прошивки: протокол MCUFWDL реализован (`h3_wifi_download_fw`),
  прошивка встроена в бинарь (`src/fw_blob.S` + `fw_nic.bin`).

### В работе
- Довести загрузку прошивки до конца (W.046 давал `rc=-19` на записи первой
  страницы — разбираемся). Ключевые наблюдения:
  - Для data-команд SDC1 не взводит CMD_DONE — ждать DATA_DONE.
  - Шина MMC1 — OLD mode (бит30=0), иначе карта не отвечает.
  - SDIO-local регистры — device id 0, WLAN IOREG — device id 8.
- Далее: инициализация MAC/RF, 802.11, WPA2, TCP/IP, SMB/HTTP.

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