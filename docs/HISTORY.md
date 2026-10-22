# HISTORY.md — история проекта (этапы и ключевые вехи)

> Компактная хроника вместо разрозненных «промптов-для-следующего-чата» и
> личных архивов. Детали реализации — в `HARDWARE.md` и `DEVELOPER-GUIDE.md`.
> Ревизия: 2026-10-21.

## Этап 1 — Звук (I2S0 → MAX98357A), W.000-W.099

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

## Этап 8 — Репро-меню, SD (SDC0) + FAT32, SMP, W.779-W.78x

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
  (типы 0x0B/0x0C/0x06, приоритет раздела с папкой `/roms`; системный
  раздел с boot.scr пропускается), BPB, каталоги 8.3+LFN, цепочки
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