# HISTORY.md — история проекта (этапы и ключевые вехи)

> Компактная хроника вместо разрозненных «промптов-для-следующего-чата» и
> личных архивов. Детали реализации — в `HARDWARE.md` и `DEVELOPER-GUIDE.md`.
> Ревизия: 2026-10-21.

## Этап 1 — Звук (I2S0 → MAX98357A), W.000-W.099

> Нумерация сборок: **с этапа эмуляторов (WU.1.000) формат заменён с `W.<minor>` на `WU.<major>.<minor>`**. Скрипт `tools/scripts/bump_version.sh` инкрементирует `FW_VERSION_MINOR` (3 знака), при переполнении 999 — `FW_VERSION_MAJOR+1`, minor сброс.

- С нуля переписан I2S/DMA по даташиту H3 V1.2 (§8.6) и драйверу sun8i-h3-i2s.c.
- Ключевые находки: I2SPCM0_CLK mux=11 (PLL_AUDIO 1X), TXIM=0 (данные в MSB),
  секции .coherent/.dma_buf на 1МБ-границах, **H3 не кэш-когерентен для DMA
  → clean D-cache перед стартом** (не перестройка MMU!).
- DMA: канал выбирается smart-стартом (5..11 → 0..4), осиротевшие каналы U-Boot
  гасятся канальными регистрами (dma_reap_stray).
- Теги: restore-W099-fw-ok (звук + базовый каркас).

## Этап 2 — WiFi: SDIO-инициализация, W.100-W.230

- Написан SDC1-драйвер (src/h3_sdc.c): CMD5/3/7, CMD52/53, CIS.
- Находки: data-команды ждут DATA_DONE (не CMD_DONE); MMC1 OLD mode;
  SDIO-адресация (device 0/8/7); CMD53 byte-mode с явными хвостами; CRC ответа
  не проверять; PowerSeq CARDEMU→ACTIVE до Fn1.
- Загрузка прошивки чипа (MCUFWDL, fwdl rc=0), eFuse пуст, программный MAC.
- Теги: w119-wifi-init, w139-rf-lck.

## Этап 3 — RF: LCK/IQK и RX, W.230-W.340

- BB-конфиг (209 рег.), AGC-таблица (66), RF-таблица RadioA (136, с пропуском 0x18).
- LSSI-доступ к RF (0x824+edge → 0x8A0); LCK и IQK по драйверу.
- RX-приём заработал (W.316): 13 каналов, парсер beacon на off+24, чтение RX FIFO
  по 1 блоку (host не тянет многоблок), RPWM/CPWM/HIMR через fn1 (SDIO-local).
- Теги: w171-rx-diag, w181-rx-h2c, w210-rx-fifo-iqk, w269-rx-c2h-absent,
  w307/w309/w319-rx-scan-working.

## Этап 4 — Ассоциация и WPA2, W.340-W.520

- JOINBSS-движок: host шлёт Auth/AssocReq, ждёт ответ в RX.
- Криптоядро host-supplicant: SHA-1/HMAC/PBKDF2/PRF, AES-128, RFC3394 unwrap.
- Полный 4-way handshake: msg1→msg2 (принят AP)→msg3 (GTK unwrap)→msg4 (secure).
- Ключи в CAM: pairwise slot 4, GTK slot 5 (по живой Lakka), REG_SECCFG=0x010C.

## Этап 5 — TX и DHCP, W.520-W.669

- TX-дескриптор: AGG_BREAK, checksum, 8-байт CCMP IV, USE_RATE 1M.
- Корень зависания 3-стр кадров: EN_AMPDU_RTY_NEW в 0x420 (не 0x818);
  3-стр (DHCP) через HIQ.
- DHCP: DISCOVER→OFFER→REQUEST (опции 50/54/61, ciaddr), OFFER принят;
  **ACK — в работе** (W.669).
- Ошибки аудита 2026-10-21: вычищен мёртвый код (aes encrypt-путь, extern без
  определений, ручной CMD5-probe, jrc), диагностика гейтирована под флаги
  (WIFI_VERBOSE_LOG/TX_DIAG/RX_DIAG) — раньше MMIO-чтения выполнялись при
  выключенном флаге; фикс повторного скана (rx_seq, W.664); комментарии
  приведены к коду (boot.s, частота SDIO, место вызова set_clk_50mhz).
- Точка восстановления: **restore-2026-10-21** (заменяет все старые теги).

## Этап 6 — DHCP ACK найден, W.670-W.771

- **Корень «OFFER есть, ACK никогда» (W.770, подтверждён pcap на канале 1):
  Sequence Control для QoS-data писался только в 802.11-заголовок, а чип
  RTL8189FTV при HWSEQ_EN=0 берёт seq из TX-дескриптора
  (SET_TX_DESC_SEQ_8188F: pTxDesc+36, биты 12..23 = байты 37/38). Байты
  37/38 оставались 0 → все data-кадры уходили с seq=0 → AP отбрасывала
  REQUEST как дубликат (SA, TID0, seq), OFFER был, ACK — никогда.**
  Фикс: `buf[37]=(seq&0xF)<<4`, `buf[38]=seq>>4`, `g_qos_seq++` (сброс в join).
  Результат W.770: **DHCP ACK получен** (ip=10.66.97.32, msg=5, bc=1, dec=1).
- RCR приведён к эталону 0x700060CE (W.763, A/B после фикса BSSID) —
  `rx dec fail` 93→0, дескрипторы читаются верно.
- Полнота инициализации чипа по эталону (W.765-768): _InitBurstPktLen_8188FS
  (0x4C7/0x60C/0x4CA/0x512), rtl8188f_InitBeaconParameters (0x540/0x510/0x559),
  _InitPABias (eFuse 0x1FA). Диагностика этих блоков выведена в постоянный лог.
- DHCP-чистка: opt61 убрана из REQUEST (W.766, консистентный client-id,
  длина 299→290).
- Аудит 2026-10-22: удалён мёртвый код (net_arp_probe, net_dhcp_set_bssid,
  h3_wifi_target_is_set/captured, h3_wifi_get_bssid, sdc_diag_pseq_cmd,
  i2s_position/underruns, h3_dma_audio_chan); TXDIAG camx → печать RA+CAM;
  комментарий WIFI_TX_DIAG приведён к коду.
- Чистка RX-диагностики (W.771): static-лимиты dbg_n/arp_n/dbg_cnt вынесены
  в file-static и сбрасываются на новую IP-сессию/join.
- Проверка L2 (W.772-W.773): unicast-приём подтверждён исходящим ARP-probe
  к шлюзу — `net: ARP REPLY from BSSID ip=10.66.97.230` (uni=1 dec=1).
  Диагностика `h3_wifi_rx_poll` упрощена: печатаются только ARP и unicast;
  мёртвые счётчики g_poll_dbg_n/g_poll_arp_n удалены.
- Точка восстановления: **restore-2026-10-22**. Обновлено: статус L2/ping,
  комментарии приведены к поведению (W.722/W.743/W.773), README/NEXT/HISTORY/
  HARDWARE синхронизированы.

## Этап 7 — Пинг с ПК работает, W.772-W.778

- **Корень «ПК не может пинговать» (W.778): MSR (REG_MSR 0x0102) писался
  0x01 = _HW_STATE_ADHOC_ вместо 0x02 = _HW_STATE_STATION_ (эталон
  rtw_mlme_ext.h:57-60). При ADHOC чип отфильтровывал broadcast/FromDS
  от других STA — пинг с ПК не шёл, хотя плата↔шлюз работала.
  Фикс: `h3_sdc_write_wlan_reg8(0x0102, 0x02)`, проверка `ok=(msr==0x02)`,
  `msr&0x01`→`msr&0x03` в трёх местах (связанность).**
- Диагностика L2 (W.773-W.776): ARP-probe к шлюзу, самопинг ICMP, счётчики
  потерь s_cnt_*, печать ARP SPA — подтвердили: unicast и ICMP со шлюзом
  работают, broadcast bc=0 постоянно (до W.778).
- Эксперимент ADF|AAP в RCR (W.777) откачен — не дал broadcast; RCR остаётся
  эталонным 0x700060CE.
- Результат W.778 (лог): `msr=0x00000002 OK`; с ПК 10.66.97.92 — **ping 0%
  потерь** (мин 6 мс, среднее 14 мс), полный ARP/ICMP-обмен ПК↔плата.
- Чистка 2026-10-22 (W.779): удалена временная диагностика W.773-W.778 —
  самопинг-циклы (ARP-probe + ICMP-request к шлюзу и ПК), счётчики потерь
  s_cnt_*, ARP SPA-печать в h3_wifi_rx_poll, запоминание gw_mac/peer_mac;
  штатный ARP/ICMP (ответ на request/echo) остаётся. Комментарии RCR/MSR
  приведены к финалу. README/NEXT/HISTORY/HARDWARE
  синхронизированы (статус: пинг работает).

## Этап 8 — Ретро-меню, SD (SDC0) + FAT32, SMP, W.795

- **SMP (все 4 ядра Cortex-A7)**: новый `h3_smp.c`/`h3_smp.h` — трамплин в
  SRAM A1 (0x00000000), release через R_CPUCFG (0x01F01C00, RST_CTRL 0→3),
  PSCI-фолбэк (smc, FDI 0x84000003) в non-secure; ядра CPU1..CPU3 уходят в
  idle-цикл `wfi` (задел под эмуляторы). `h3_smp_boot()` вызывается первым
  в kmain.
- **Драйвер SDC0 (слот SD-карты)**: `h3_sdc0.c`/`h3_sdc0.h` — полный подъём
  с нуля: CCU gate MMC0 (GATING0 bit8, без CCU reset — «убиваем зомби»
  после U-Boot локально, GCTL_RESET+HWRST), SDMMC0_CLK (OSC24M, init-такт),
  пины PC0..PC5 (function 2), CMD0/CMD8/CMD55+ACMD41(HCS)/CMD2/CMD3/CMD7/
  ACMD6(4-bit)/CMD16/CMD58(SDSC/SDHC). Чтение/запись сектора PIO (1 блок).
- **FAT32 (чтение)**: `fat.c`/`fat.h` — поиск целевого раздела из MBR
  (типы 0x0B/0x0C, приоритет раздела с папкой `/roms`; теперь — первый
  валидный FAT32, если `/roms` нет), BPB, каталоги 8.3+LFN, цепочки
  кластеров, `fat_read_file`. Запись — отдельным этапом (системный раздел
  сейчас только чтение).
- **Ретро-меню** (`retro_menu.c`/`retro_menu.h`): главный экран — список
  систем по группам (Portable/Consoles/Arcade/Computers, из
  `resources/emulation_h3_v2.xlsx` в `systems.h`, статусы `Потянет H3`);
  выбор системы → браузер ромов из `/roms/<система>` на SD (8.3+LFN);
  builtin-системы — диалог «Встроенный ром / Загрузить с SD»; запуск
  эмулятора — заглушка (диспетчер по id в будущем). Служебные пункты
  Settings/Help/About; в Settings — экраны **Transfer** (WiFi-скан/connect,
  перенос из старого main.c) и **Sound test** (440/697/1209 Гц).
- Старое текстовое меню main.c (Sound/Transfer/Settings) удалено —
  ретро-меню стало основным экраном (kmain вызывает `retro_menu_run()`,
  которое само крутит `wifi_layer_poll()`).

## Этап 9 — Меню: папки/[OK]/описания/сортировка, PA15, индикатор жизни, W.814-W.816

- **Двухфазный init SDC0** (забрать карту у U-Boot): фаза A — наследование
  (только gate + CMD17, без сброса/CMD0), фаза B — полный подъём (FEL).
  Карта подхвачена: `sd0: legacy (U-Boot) ok`, `fat part lba=131072`, `sd: ready`.
- **Структура SD = как корень проекта**: папки систем в `roms/<система>/`
  (в корне раздела; системный раздел с boot.scr не трогаем). 40 недостающих
  папок создано в корне проекта (`roms/`).
- **Меню**: `[OK]` рядом с пунктом, если папка системы на SD; пункт белого цвета
  только при «папка есть И эмулятор реализован» (эмуляторов пока нет — всё серое);
  описание платформы (проц / ОЗУ / частота / год) внизу экрана;
  **сортировка по годам** внутри групп (Portable/Consoles/Arcade/Computers/Laptops);
  не-ASCII символы (`·`, `μ`) в описаниях заменены на ASCII (шрифт 8x8 не имеет).
- **PA15 = EN_SD** (LED_EN_SD, активный уровень 1): горит ТОЛЬКО на время
  операций с картой (токены `SD0_LED_ON/OFF` в `h3_sdc0.c`), постоянного
  включения в меню нет (`sd_enable` из меню убран).
- **Индикатор жизни процессора**: мигание PL10 (~500 мс) вызывается из `udelay`
  (sys.c) — работает во ВСЕХ слоях, а не только в меню.
- **«Insert SD»** в браузере ромов при отсутствии карты (вместо «Folder is empty»).
- **Компактный WiFi-лог**: убраны лишние успешные диагностики (macid_re, a0_post_fw),
  оставлено важное.
- **Реестр систем дополнен** из pico-retro (только пункты меню): Atari 5200,
  FB Neo, Cave, Atari 800, Enterprise.
- Документация: README (таблица «система ↔ папка», структура SD), HISTORY (этап 9),
  DEVELOPER-GUIDE (PA15, индикатор жизни, [OK], сортировка).

## Этап 10 — Аудит комментариев/доков, PA15-инверсия, Transfer-IP, компактный лог, W.822-W.824

- **Аудит комментариев и документации (W.822, коммит e5086f2)**: комментарии
  приведены к фактическому коду (кулдаун WiFi 200 мс, opt61 убрана из описания
  REQUEST, fat_mkdir реализован, двухфазный init SDC0, CMD9, PC0..PC5, PLL_VIDEO
  288 МГц, fb_text void, MCLK=PLL/2, DMA-канал 4, PTK 48 байт, AIV 4 байта,
  reg8 всегда CMD52, LCK-последовательность, RRSR 0x000FFFF1, CAM group slot5,
  AGC 67, seccfg 0x010C). Доки: README (W.821, удалены ghost-системы
  atari8/cave/pc98/pc286/pc386, Vectrex=vectrex, C64=Computer, cps2, ~113 КБ),
  DEVELOPER-GUIDE (ревизия, PA15-токены), NEXT-QUESTIONS (W.821), HISTORY
  (этапы 8/9, PA15), LAKKA-ETALON (fw_nic.bin = 18802).
- **PA15 (LED_EN_SD) — активный уровень 1 (W.823, уточнено W.825)**: LED
  светится при логической 1 на PA15. `SD0_LED_ON()` пишет 1 (карта
  используется → горит), `SD0_LED_OFF()` — 0 (гаснет). Экранная правка W.823
  с акт. уровнем 0 давала постоянное свечение (выход по умолчанию 1);
  на этой плате LED_EN_SD активен по высокому уровню.
- **Экран Transfer перерисовывается при получении IP (W.824)**: в главном цикле
  меню `transfer_ip_state()` (свёрнутый IP+ACK) сравнивается каждую итерацию;
  при изменении ставится `dirty=1` — IP появляется на экране БЕЗ нажатия кнопки
  (раньше только после нажатия).
- **Компактный WiFi-лог**: убраны развёрнутые печати `net: chaddr=...` (MAC в
  BOOTP), `net: REQHEX ...` (полный hex-дамп DHCP-кадра, ~300+ байт на REQUEST),
  `join: bssid wr=... rd=...` (12 байт MAC) — осталась одна строка
  `join: msr=0x2 OK` (проверка STA, W.778).
- **Наработки каркаса эмуляторов (задел, не активированы)**:
  - `mem.c`/`mem.h` — динамический аллокатор (first-fit) для слоя эмулятора +
    зоны: куча, ROM-зона (до 128 МБ) с запасом между ними (предварительный
    бюджет: куча ~256 МБ, ROM ~128 МБ, стек от верха); `mem_init/alloc/free/
    rom_reserve/release`.
  - `mailbox.c`/`mailbox.h` — почта CPU0↔CPU1 (кольцо сообщений в uncached
    секции `.mailbox`, флаги, mb_send/mb_recv); секция добавлена в linker.ld
    и помечается uncached в i2s.c.
  - Архитектура ядер (предварительно): CPU0 — эмулятор/меню/WiFi, CPU1 — звук
    (DMA), CPU2/CPU3 — потушены (перспектива: SPI-экран/резерв).

## Этап 11 — USB-host (OHCI), клавиатура в меню, звуковой тест, W.839-W.896

- **USB-host с нуля (persist до W.896)**: FEL+SPL не наследует USB (U-Boot его
  не инициализирует) — полный подъём CCU GATING0/RST0 (биты 25/29) +
  USBPHY_CFG (биты 1/9/17) + SIDDQ снят (pmu1 0x01C1B810 bit1=0), passby/ICR
  0x701 (0x01C1B800, 0x01C1B000+0x800). Порт работает через **OHCI** (эталон
  pico-retro usb_ohci.c): per-port пулы ED/TD/HCCA в `.coherent`, root-port
  PPS→PRS→CCS/PES, control через ED/TD+CLF+D-cache и ctrlhead/ctrlcur.
- **OHCI per-port (W.839-w): PHY1** (USB1 0x01C1B400, флешка) и **PHY2**
  (USB2 0x01C1C400, клавиатура) поднимаются и enum-ятся независимо; лог
  `usb: ohci(idx) rev/port`, `DEVICE on OHCI`, `port after reset=0x303`.
- **Клавиатура AVATTO i8 (0603:0002, донгл) enum-ится (W.844-w)**: GET_DESC(8)
  → mps=8 (Low-Speed) → SET_ADDRESS(1) → полный config → парсинг boot-keyboard
  (3,1,1) + EP IN → SET_CONFIGURATION + SET_PROTOCOL(boot). Лог:
  `usb-kbd: vid=0x00000603 pid=0x00000002 mps=8`, `-> KEYBOARD (ep=...) ls=1`.
- **Interrupt-IN (W.86x)**: периодический ED/TD в HCCA-таблицу (slot0),
  PLE, переарм по снятию пакета (по эталону pico-retro usb_ohci.c:
  `usb_ohci_intr_in_start/poll`).
- **Баг «kbd: silent» (W.894)**: парсер config-дескриптора брал EP IN мыши
  (0x82, интерфейс 1) вместо клавиатурных стрелок (0x81, интерфейс 0) —
  композитный донгл (2 интерфейса). Исправление: сброс `type` на каждом
  interface-дескрипторе (W.895). После фикса клавиатурный EP=0x81, отчёты
  идут: `kbd rep: 00 00 51 ...` (0x51=Down, 0x52=Right, 0x4F=Left).
- **Баг «постоянные автоповторы» (W.896)**: отпускание (пустой отчёт) не
  сбрасывало `g_last_btn` — меню «прыгало». Исправление: пустой отчёт →
  `g_last_btn=0`, автоповтор стрелок (первый через 350 мс, далее 90 мс);
  документировано в
  `docs/avatto-i8-pro-hid.md` (донгл шлёт отчёт только при изменении,
  без повторов при удержании).
- **Флешка (USB1) — STALL на OHCI (W.89x)**: GET_DESCRIPTOR при addr 0 падает
  `data cc=0x08` (STALL). Причина: флешка High-Speed, а OHCI не ведёт HS;
  результат из лога W.892, до переноса на EHCI. Задел EHCI (SIDDQ снят,
  `usb_ehci_init_bus/ctrl/bulk`) — задача отдельного шага (задача 5).
- **Sound test — физические кнопки (W.89x, собрано 125892)**: UP/DOWN
  переключают частоту (440/697/1209 Гц), A=play/stop, B=back; кнопки PCF8574.
- **Доки по клавиатуре добавлены в docs/**: `avatto-i8-pro-full.md`,
  `avatto-i8-pro-hid.md` (протокол донгла 0603:0002, тайминги, алгоритм
  short/long press), `keyboard-autorepeat-howto.md`.

## Этап 12 — Аудит USB/FAT32 + фиксы (план W.9xx)

- **FAT**: убрана бесконечная рекурсия `blk_read/blk_write` (источник по
  умолчанию — SD); контекст тома вместо глобалов для честного поиска
  раздела по `/roms`; только FAT32 (типы 0x0B/0x0C); total_sectors16,
  «..»=0 для корня; запись: `fat_write_file`, `fat_delete`, расширение
  полного каталога; кэш FAT-сектора (вдвое меньше чтений).
- **USB**: EHCI error mask (Halted/DBE/Babble), SET_CONFIGURATION перед
  MSD, статический `usb_msd_dev` (ушёл висячий g_dev), qTD-разбиение по
  4К, bounce-буфер .coherent для MSD/HID, EP0 MPS из дескриптора
  (usb_ohci_set_mps), приоритет источника SD→USB (флешка только при
  неудаче SD), чистка мёртвого кода, отладочные принты под
  USB_VERBOSE_LOG.
- **mem**: ROM-зона больше не перекрывает стеки (резерв 2 МБ от верха).
- Доки синхронизированы: README/DEVELOPER-GUIDE/NEXT-QUESTIONS/HARDWARE
  (нумерация PHY1/PHY2, биты CCU 0x0CC, SIDDQ в pmuN+0x10, автоповтор
  350/90 мс).

## Этап 13 — USB Mass Storage на OHCI: флешка работает (W.926–W.929)

- **Главная причина «тихих» отказов MSD**: `usb_msd_activate()` вызывался
  после INQUIRY/TUR/READ CAPACITY. До активации `usb_bulk_*` возвращают -1
  без печати — в логе оставались только `INQUIRY fail`/`TUR fail` без шага и
  CSW. Теперь активация идёт сразу после `usb_msd_init()`, до любых команд.
- **MPS эндпоинтов — пер-устройство**: глобальная таблица EP→MPS затиралась
  клавиатурой (EP1/EP2 = 8 после её enum). MPS хранится в `usb_msd_dev`
  (`ep_in_mps/ep_out_mps`) и передаётся в каждый `usb_bulk_*`.
- **Bulk-ED на OHCI**: направление задаётся в ED (D=IN/OUT), а не «из TD» —
  как в Linux и эталоне pico-retro.
- **CLEAR_FEATURE(HALT)**: wIndex = номер EP с битом направления (было
  0x02 — несуществующий EP2 OUT).
- **Порт не важен**: класс устройства определяется по interface-дескрипторам
  конфигурации (MSD 8/6/0x50 или HID boot 3/1/1); флешка и клавиатура
  работают в любом из двух USB-разъёмов. Общий enum — `usb_ohci_enum_common`,
  драйверы — `usb_ohci_msd_start` / `usb_kbd_init_on`.
- **TUR с ретраями** (`usb_msd_wait_ready`, 3×100 мс): сразу после
  SET_CONFIGURATION носитель отвечает CHECK CONDITION (CSW status=1).
- **Диагностика**: `ohci-bulk: TIMEOUT …` (edhead/edtail/cmdst),
  `msd: op=… CBW/DATA-IN/CSW fail`, дамп CSW; в рабочей сборке
  `USB_VERBOSE_LOG=0`, `MSD_VERBOSE_LOG=0`.
- Итоговый лог W.929: `INQUIRY ok: Generic Flash Disk`, `block=512
  last_lba=31334399`, `MBR sig=0xAA55`, FAT: `part lba=131072 sec/clu=16
  root_clu=2`, `sd: ready`.
- **W.930**: штатный `CHECK CONDITION` у TUR (носитель ещё не готов) больше
  не печатает CSW-дамп — он остаётся только для остальных команд; строка
  порта печатается целиком после reset. Рабочий лог стал чистым.
- **W.931**: запись/удаление на активном источнике проверяются из меню
  (Settings → Storage test): `fat_selftest_write()` пишет `WTEST.TXT` в
  корень, читает обратно, сверяет 512 байт и удаляет; в логе
  `fat: selftest write rc=N`, на экране `Storage: OK/FAIL`. Коды: -1 запись,
  -2 поиск, -3 чтение, -4 сверка, -5 удаление. (Прогон на железе — за
  владельцем.)
- **W.932**: первая версия Storage test падала с `rc=-1`: `fat_write_file`/
  `fat_delete` отвергали корень `""` как каталог (проверка требовала
  непустое имя). Для записи/удаления достаточно `d.size == 0` — корень
  валиден. Прогон на железе — за владельцем.- **W.933**: удаление рома из браузера — кнопка Select (Backspace): первый
  нажат — диалог «Delete this file?», второй (Enter) — fat_delete; B/Select
  — отмена. Лог m: /roms/<система>/<файл> rc=N. Подсказка внизу
  браузера «A=load  Select=delete  B=back». (Прогон на железе — за владельцем.)

## Этап 14 — Эмуляторы. Atari Lynx (WU.1.000)

- **Нумерация сборок сменилась**: с этого этапа формат `W.<minor>` заменён на
  `WU.<major>.<minor>` (bump-скрипт обновлён, первая сборка — WU.1.000).
- **Слой Lynx (ядро Handy, порт libretro-handy)**: независимый C++-слой
  (`src/lynx/*.cpp` + `src/blip/*`) + glue `src/lynx_emu.cpp`. C++ компилируется
  в bare-metal (CXX, new/delete/malloc/floor/str* в `_support.cpp`).
  - Ром: из FAT (SD или USB) в ROM-зону, BIOS — HLE (без lynxboot.img);
  - Экран: оригинальное разрешение 160x102 (портрет), поворот 90° в
    fb 1024x600, пиксель XRGB8888;
  - Звук: `FetchAudioSamples()` → `gAudioBuffer` (uncached `.coherent`) →
    почта `MB_MSG_AUDIO_DATA` → CPU1 (`audio_cpu1.c`) → `i2s_write` (DMA);
  - Кнопки: PCF8574 → биты Handy (SetButtonData); выход — длинное нажатие START.
- **Фикс загрузки рома**: `emu_load_rom` больше не делает лишний
  `fat_find("roms", name)` — ищет файл напрямую `fat_find(dir, name)`.
- Сборка слоя возможна только чистой (`make clean && make -j8 bin`).
- Прогон на железе — за владельцем (нужен .lnx-ром в `/roms/lynx/`).

## Этап 14b — Sega-джойстик и тест пада (WU.1.001-WU.1.006)

- **Драйвер джойстика SEGA скопирован 1 к 1** из `pico-retro-new-gen`
  (`src/sega_pad.c/h`, контроль по SHA-256) — протокол чтения 6-btn НЕ
  трогается; внутри файла сохранено предупреждение «НЕ ЛАЗИТЬ БЕЗ СТЕНДА»
  (тайминги подобраны эмпирически, r147..r157). Кнопки: PCF8574@0x27,
  джойстик: PCF8574@0x20, шина PG9/PG8 (bit-bang), адреса не пересекаются.
- **Зависимости обеспечены без правок драйвера**: в проект добавлены
  `include/pcf8574_bb.h` и `include/h3_hs_timer.h` (1 к 1 из источника),
  в `include/h3.h` — `H3_HS_TIMER`/`H3_HS_TIMER_TypeDef`/`__IO`, в
  `src/lynx/_support.cpp` — глобальный `printf` (ва-обёртка над uart).
  Условие: отсутствие на шине любого устройства (0x20 или 0x27) не должно
  ломать программу — `sega_pad_scan()` возвращает 0 без ACK, `pcf8574_read()`
  — 0 при ошибке чтения.
- **Settings → «Sega pad test» (WU.1.00x)**: экран рисует джойстик
  Mega Drive 2 (крестовина, A/B/C + X/Y/Z, Start/Mode) и подсвечивает
  нажатые кнопки; показывает ACK/наличие пада/6-btn и hex-маску.
  Прогон на железе — за владельцем.
- **WU.1.007*: причина «джой не работает» — `sega_pad_init()` не вызывался
  (в логе не было `sega_pad: init OK`). Инициализация добавлена в `main.c`
  после `pcf8574_init()` (драйвер не тронут). **Управление меню джойстиком**: в
  `wait_btn()` джой-скан подмешивается к кнопкам — крестовина → стрелки,
  A/B → A/B, Mode → Select, Start → Start; X/Y/Z/C в меню не мапятся
  (решение владельца). Прогон на железе — за владельцем.
- **WU.1.008+ (зависание при входе в меню)**: `sega_pad_scan()` в `wait_btn()`
  вызывал `h3_hs_timer_lo_us()`, а тот (в скопированном `h3_hs_timer.h`)
  крутил цикл чтения CURNT_HI в ожидании стабильности. На ЭТОЙ плате
  CURNT_HI нестабилен (задокументировано в sys.c) → бесконечный цикл →
  зависание на первом входе в меню. Фикс: `h3_hs_timer_lo_us()` читает
  только CURNT_LO (как `micros()`); короткие паузы скана (20 мкс) wrap в
  ~44 с не пересекают. Драйвер sega_pad.c не тронут.
