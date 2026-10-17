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