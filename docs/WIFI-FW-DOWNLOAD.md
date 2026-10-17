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
