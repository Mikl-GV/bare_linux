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