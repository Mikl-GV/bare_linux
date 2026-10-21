# NEXT-QUESTIONS.md — текущее состояние и ближайшие шаги

> Замена `NEXT-CHAT-PROMPT.md` (личный промпт удалён). Ревизия: 2026-10-21 (W.669).

## 1. Актуальный статус (W.669)

- **WiFi-слой полностью работает**: SDIO 50 МГц/4-bit/HS, пwon/fwdl/MAC/LCK/IQK,
  скан 13 каналов (повторные rescan работают, W.664), host-ассоциация, WPA2
  4-way handshake (msg1→msg4, PTK/GTK в CAM), DHCP DISCOVER→OFFER→REQUEST.
- **Открыто**: DHCP ACK на A05s не приходит (OFFER есть, REQUEST с опциями 50/54/61
  и ciaddr уходит, сервер молчит). ARP/ICMP работают (fallback IP из OFFER).
- Цель этапа: WebDAV-сервер в прошивке → доступ к файлам на SD из проводника Windows.

## 2. DHCP ACK — диагностика и гипотезы (W.658-W.669)

Симптом (лог W.668): `net: DHCP OFFER ip=10.66.97.228` → `REQUEST sent` →
`REQUEST len=299 opt61=1 opt50=1 opt54=1` → retry #1/#2 → `WPA2 pending (eapol=2),
no DHCP/IP` → `poll: IP accepted from OFFER (no ACK)`.

Сделано (W.650-W.669):
- `dhcp_xid` новый на каждую сессию; `dhcp_offer_seen` не сбрасывается в DISCOVER,
  сброс на старте join; REQUEST только на первый OFFER; опции 61 (client-id),
  50 (requested-ip), 54 (server-id); ciaddr=предложенный IP + src IP (стиль RENEW);
  REQUEST broadcast L2 → dst_ip=server; ретраи +3с/+6с; диагностика входящих DHCP
  (net: dhcp ports→op→msg) теперь **безусловная** (W.669) — станет видно, приходит
  ли ACK/NAK.

Гипотезы (очередность проверки):
1. ACK приходит, но отбрасывается на xid/parse — видно по новой диагностике `net: dhcp`.
2. REQUEST не выгружается из TX FIFO — проверка `txpost empty/fq` (W.669).
3. Сервер не отвечает на REQUEST с ciaddr (dnsmasq A05s) — вариант: вернуть
   SELECTING-форму (ciaddr=0), убрать опцию 61, unicast на server.
4. Порядок: OFFER→REQUEST должен держать тот же xid, а опции 50/54 обязаны
   совпадать с OFFER (проверить в логе).

## 3. После DHCP ACK

1. Пинг с ПК (критерий связности).
2. WebDAV/HTTP-сервер на SD-карте (доступ из проводника Windows как сетевое
   расположение). SMB отклонён (тяжёлый для bare-metal).
3. Параллельно: слой эмуляторов ретроконсолей (не зависит от сети).

## 4. Известные ограничения

- Host SDIO не тянет многоблочный read CMD53 (1 блок за раз).
- RX0_REQ_LEN (0x1C) = накопленный объём, не один кадр.
- C2H-ABSENT на scan/NOLINK — ассоциация host-кадрами, не H2C.
- DHCP ACK на A05s — в работе (см. §2).
- `g_test_plain` (W.586) — тестовый флаг «слать без шифрования», по умолч. 0.

## 5. Развитие (идеи, требуют решений владельца)

- Подключение прерываний (GIC IRQ) для SDIO RX вместо поллинга в главном цикле —
  для потоковых данных WebDAV. Отдельный шаг после стабилизации DHCP.
- PIO gate (GATING0 bit0) в gpio_init для чистого FEL — проверить на стенде.
- Проверка CAM slot 5 на других WPA2-AP (MTS_GPON).
- Прошивка чипа с SD-карты вместо встраивания (для компактности образа).