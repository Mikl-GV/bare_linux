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
