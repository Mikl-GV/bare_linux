#include "wifi_net.h"
#include "h3_sdc.h"
#include "uart.h"
#include "sys.h"

static uint32_t dhcp_xid = 0x12345678;
static const uint8_t bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* Состояние между OFFER и ACK (SELECTING). */
static uint8_t dhcp_offer_ip[4];
static uint8_t dhcp_server_id[4];
static uint8_t dhcp_offer_seen;

/* W.650: последний известный адрес (из OFFER/ACK) — для отображения на экране. */
static uint8_t net_ip[4];
static uint8_t net_mask[4];
static uint8_t net_gw[4];
static uint8_t net_have;      /* 1 = адрес виден (есть OFFER) */
static uint8_t net_have_ack;  /* 1 = адрес подтверждён DHCP ACK */

/* W.650: вернуть текущий адрес для UI. ip/mask/gw заполняются, если адрес
   известен (OFFER получен); *have_ack=1 только после DHCP ACK.
   Возвращает 1 при известном адресе. */
int net_dhcp_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4], int *have_ack)
{
    if (!net_have) return 0;
    for (int i = 0; i < 4; i++) {
        ip[i] = net_ip[i];
        mask[i] = net_mask[i];
        gw[i] = net_gw[i];
    }
    if (have_ack) *have_ack = net_have_ack;
    return 1;
}

static uint16_t csum(const uint8_t *p, uint16_t len)
{
    uint32_t s = 0;
    uint16_t i;
    for (i = 0; i + 1 < len; i += 2) s += (uint16_t)((p[i] << 8) | p[i+1]);
    if (i < len) s += (uint16_t)(p[i] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* Собрать и отправить DHCP-пакет: msg_type 1=DISCOVER, 3=REQUEST.
   Для REQUEST передаются requested IP (опция 50) и server id (опция 54).
   Для unicast REQUEST: dst_mac = BSSID AP, dst_ip = server (10.72.75.230),
   flags=0 (ответ unicast). Для DISCOVER: broadcast на 255.255.255.255. */
static int dhcp_send(uint8_t msg_type, const uint8_t *req_ip, const uint8_t *srv_id,
                     const uint8_t *dst_mac, const uint8_t *dst_ip, uint16_t bcast_flag)
{
    uint8_t pkt[400];
    uint32_t p = 0;
    uint8_t m[6];
    uint16_t i;

    /* IPv4 (20) */
    pkt[p++] = 0x45; pkt[p++] = 0x00;
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* total len — ниже */
    pkt[p++] = 0x00; pkt[p++] = 0x01;   /* id */
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* flags/frag */
    pkt[p++] = 0x40;                    /* TTL */
    pkt[p++] = 0x11;                    /* UDP */
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* csum — ниже */
    pkt[p++] = 0; pkt[p++] = 0; pkt[p++] = 0; pkt[p++] = 0;          /* src 0.0.0.0 */
    pkt[p++] = dst_ip ? dst_ip[0] : 255;    /* dst IP: broadcast или server id */
    pkt[p++] = dst_ip ? dst_ip[1] : 255;
    pkt[p++] = dst_ip ? dst_ip[2] : 255;
    pkt[p++] = dst_ip ? dst_ip[3] : 255;

    /* UDP (8): 68 -> 67 */
    pkt[p++] = 0x00; pkt[p++] = 0x44;
    pkt[p++] = 0x00; pkt[p++] = 0x43;
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* udp len — ниже */
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* udp csum = 0 (IPv4 допускает) */

    /* BOOTP/DHCP фиксированная часть 236 */
    pkt[p++] = 0x01; pkt[p++] = 0x01;   /* op=BOOTREQUEST, htype=eth */
    pkt[p++] = 0x06;                    /* hlen */
    pkt[p++] = 0x00;                    /* hops */
    pkt[p++] = (uint8_t)(dhcp_xid >> 24); pkt[p++] = (uint8_t)(dhcp_xid >> 16);
    pkt[p++] = (uint8_t)(dhcp_xid >> 8);  pkt[p++] = (uint8_t)dhcp_xid;
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* secs */
    pkt[p++] = (uint8_t)(bcast_flag >> 8); pkt[p++] = (uint8_t)(bcast_flag & 0xFF); /* flags */
    for (i = 0; i < 16; i++) pkt[p++] = 0;          /* ciaddr/yiaddr/siaddr/giaddr */
    /* W.663: для REQUEST ставим ciaddr (BOOTP [40..43]) = предлагаемый IP и
       src IP (IP-заголовок [12..15]) = тот же адрес — стиль dhclient RENEW.
       Многие dnsmasq отвечают ACK именно на REQUEST с ciaddr, а на форму
       с ciaddr=0 (SELECTING) могут молчать. */
    if (msg_type == 3 && req_ip) {
        pkt[12] = req_ip[0]; pkt[13] = req_ip[1];
        pkt[14] = req_ip[2]; pkt[15] = req_ip[3];
        pkt[40] = req_ip[0]; pkt[41] = req_ip[1];
        pkt[42] = req_ip[2]; pkt[43] = req_ip[3];
    }
    h3_wifi_get_mac(m);
    for (i = 0; i < 6; i++) pkt[p++] = m[i];        /* chaddr */
    for (i = 0; i < 10; i++) pkt[p++] = 0;
    for (i = 0; i < 192; i++) pkt[p++] = 0;         /* sname + file */

    pkt[p++] = 0x63; pkt[p++] = 0x82; pkt[p++] = 0x53; pkt[p++] = 0x63;  /* cookie */
    pkt[p++] = 53; pkt[p++] = 1; pkt[p++] = msg_type;
    /* W.656: опция 61 (client-id) — dnsmasq (Android-хотспот) держит лизинг
       по client-id; без него OFFER шлёт, а REQUEST молча игнорирует.
       Формат: тип 1 (ethernet) + 6 байт MAC. */
    pkt[p++] = 61; pkt[p++] = 7; pkt[p++] = 0x01;
    for (i = 0; i < 6; i++) pkt[p++] = m[i];
    if (msg_type == 3 && req_ip && srv_id) {
        pkt[p++] = 50; pkt[p++] = 4;
        for (i = 0; i < 4; i++) pkt[p++] = req_ip[i];
        pkt[p++] = 54; pkt[p++] = 4;
        for (i = 0; i < 4; i++) pkt[p++] = srv_id[i];
    }
    pkt[p++] = 55; pkt[p++] = 4; pkt[p++] = 1; pkt[p++] = 3; pkt[p++] = 6; pkt[p++] = 15;
    pkt[p++] = 255;

    pkt[2] = (uint8_t)(p >> 8); pkt[3] = (uint8_t)p;
    /* W.531: UDP length — поле pkt[24..25], НЕ pkt[22..23].
       pkt[22..23] = UDP dst port (67), его перезапись на (p-20)=258
       уводила DISCOVER на порт 258 — DHCP-сервер на 67 не получал
       пакет, OFFER не приходил. Длина 258 = p - 20 (IP payload). */
    pkt[24] = (uint8_t)((p - 20) >> 8); pkt[25] = (uint8_t)(p - 20);
    pkt[10] = (uint8_t)(csum(pkt, 20) >> 8);
    pkt[11] = (uint8_t)(csum(pkt, 20) & 0xFF);

    #if WIFI_TX_DIAG
    TXDIAG(msg_type == 1 ? "  dhcp DISCOVER len=" : "  dhcp REQUEST len=");
    TXDIAG_INT((int)p);
    TXDIAG(" IP.total=0x");
    TXDIAG_HEX((uint32_t)((pkt[2] << 8) | pkt[3]));
    TXDIAG(" UDP.len=0x");
    TXDIAG_HEX((uint32_t)((pkt[24] << 8) | pkt[25]));
    TXDIAG(" sport=");
    TXDIAG_INT((pkt[20] << 8) | pkt[21]);
    TXDIAG(" dport=");
    TXDIAG_INT((pkt[22] << 8) | pkt[23]);
    TXDIAG(" proto=0x");
    TXDIAG_HEX((uint32_t)pkt[9]);
    TXDIAG(" flags=0x");
    TXDIAG_HEX((uint32_t)((pkt[38] << 8) | pkt[39]));
    TXDIAG("\r\n");
#endif
    /* W.656: для REQUEST всегда печатаем критичное — длину и присутствие
       опций client-id(61) / requested-ip(50) / server-id(54). */
    if (msg_type == 3) {
        int has_61 = 0, has_50 = 0, has_54 = 0;
        /* DHCP-опции: IP(20) + UDP(8) + BOOTP фикс.(236) + cookie(4) = 268. */
        uint32_t obase = 20 + 8 + 236 + 4;
        for (uint32_t oi = obase; oi + 2 < (uint32_t)p; ) {
            uint8_t c = pkt[oi];
            if (c == 255) break;          /* end */
            if (c == 0) { oi++; continue; }   /* pad */
            uint8_t ol = pkt[oi + 1];
            if (c == 61) has_61 = 1;
            if (c == 50) has_50 = 1;
            if (c == 54) has_54 = 1;
            oi += 2 + ol;
        }
        uart_print("  net: REQUEST len=");
        uart_print_int((int)p);
        uart_print(" opt61=");
        uart_print_int(has_61);
        uart_print(" opt50=");
        uart_print_int(has_50);
        uart_print(" opt54=");
        uart_print_int(has_54);
        uart_print("\r\n");
    }

    return h3_wifi_tx_data(0x0800, dst_mac, pkt, (uint16_t)p);
}

int net_dhcp_discover(struct net_state *st)
{
    /* W.592 [корень]: НЕ сбрасываем dhcp_offer_seen здесь. Раньше циклический
       retry DISCOVER обнулял флаг после того, как OFFER уже пришёл, и
       пришедший затем ACK отбрасывался (`if (!dhcp_offer_seen) return`).
       Теперь флаг хранит факт «OFFER получен» до ACK/NAK. */
    (void)st;
    return dhcp_send(1, 0, 0, bcast_mac, 0, 0x8000);
}

/* W.592: 1 = OFFER получен (в стадии SELECTING), ACK ещё не пришёл.
   Цикл DHCP в h3_sdc.c использует это, чтобы НЕ слать повторный DISCOVER
   и ждать ACK. */
int net_dhcp_offer_pending(void)
{
    return dhcp_offer_seen ? 1 : 0;
}

/* W.648: MAC точки доступа (BSSID) — для unicast REQUEST в SELECTING.
   Устанавливается из h3_wifi_join_ap перед DHCP-циклом. */
static uint8_t dhcp_srv_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
void net_dhcp_set_bssid(const uint8_t *bssid)
{
    for (int i = 0; i < 6; i++) dhcp_srv_mac[i] = bssid[i];
}

/* W.594 [корень]: сброс состояния DHCP на старте нового join. Раньше
   dhcp_offer_seen (static) не сбрасывался между join — после успешного
   OFFER во 2-й сессии флаг оставался =1, и третий join не отправлял
   DISCOVER (`!net_dhcp_offer_pending()` == false). Третий join зависал
   без DHCP. Вызывается в начале h3_wifi_join_ap. */
void net_dhcp_reset(void)
{
    dhcp_offer_seen = 0;
    net_have = 0;
    net_have_ack = 0;
    /* W.646: НОВЫЙ xid на каждый DHCP-цикл. Раньше xid был жёстко
       0x12345678 во всех сессиях — Android-хотспот (dnsmasq) мог считать
       транзакцию повтором уже обработанной и не отвечать ACK. */
    dhcp_xid = ((uint32_t)micros() | 1u);
    WLOG("  net: dhcp xid=0x");
    WLOG_HEX(dhcp_xid);
    WLOG("\r\n");
}

/* W.663: DHCP REQUEST — broadcast L2, dst IP = сервер, flags=0x8000,
   ciaddr = предлагаемый IP (см. dhcp_send). dhclient-стиль RENEW. */
int net_dhcp_request(void)
{
    return dhcp_send(3, dhcp_offer_ip, dhcp_server_id,
                     bcast_mac, dhcp_server_id, 0x8000);
}

/* ICMP echo request -> echo reply. */
static void net_icmp_reply(const uint8_t *pkt, uint16_t len, uint8_t ihl,
                           const uint8_t *src_mac, const struct net_state *st)
{
    uint8_t rep[20 + 1500];
    uint16_t icmp_len, ip_len, ic;
    int k;

    if (!st->have || len < (uint16_t)(ihl + 8)) return;
    if (pkt[ihl] != 8 || pkt[ihl+1] != 0) return;    /* только echo request */
    for (k = 0; k < 4; k++) if (pkt[16+k] != st->ip[k]) return;   /* не нам */

    ip_len = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (ip_len <= ihl) return;
    icmp_len = (uint16_t)(ip_len - ihl);
    if (icmp_len > len - ihl) icmp_len = (uint16_t)(len - ihl);
    if (icmp_len < 8 || icmp_len > 1500) return;

    for (uint16_t i = 0; i < icmp_len; i++) rep[20+i] = pkt[ihl+i];
    rep[20] = 0; rep[21] = 0;                         /* type=0 (reply), code=0 */
    rep[22] = 0; rep[23] = 0;                         /* checksum */
    ic = csum(&rep[20], icmp_len);
    rep[22] = (uint8_t)(ic >> 8); rep[23] = (uint8_t)ic;

    for (k = 0; k < 20; k++) rep[k] = 0;
    rep[0] = 0x45;
    rep[2] = (uint8_t)((20 + icmp_len) >> 8); rep[3] = (uint8_t)(20 + icmp_len);
    rep[8] = 64; rep[9] = 1;
    for (k = 0; k < 4; k++) { rep[12+k] = pkt[16+k]; rep[16+k] = pkt[12+k]; }
    ic = csum(rep, 20);
    rep[10] = (uint8_t)(ic >> 8); rep[11] = (uint8_t)ic;

    h3_wifi_tx_data(0x0800, src_mac, rep, (uint16_t)(20 + icmp_len));
}

/* Входящий IPv4 (после 802.11 + LLC/SNAP). src_mac — L2-адрес отправителя
   из 802.11-заголовка (addr3 для FromDS, addr2 для ToDS). */
void net_handle_ipv4(const uint8_t *pkt, uint16_t len, const uint8_t *src_mac,
                     struct net_state *st)
{
    uint8_t ihl, proto;
    uint16_t udp_off, udp_len, dh, opt, end;
    int k;

    if (!st || len < 20) return;
    ihl = (uint8_t)((pkt[0] & 0x0F) * 4);
    if (ihl < 20 || ihl > len) return;
    proto = pkt[9];

    if (proto == 1) {                 /* ICMP */
        net_icmp_reply(pkt, len, ihl, src_mac, st);
        return;
    }
    if (proto != 17) return;          /* не UDP */
    if (len < (uint16_t)(ihl + 8 + 240)) return;

    udp_off = ihl;
    /* W.590 [корень]: входящий OFFER/ACK приходит от ДХКП-сервера: src=67,
       dst=68 (обратно DISCOVER/REQUEST 68→67). Раньше проверяли только
       src=68/dst=67 и каждый OFFER отбрасывался — OFFER приходил (W.589
       rx etype=0x0800 plen=386), но net_handle_ipv4 его дропал.
       Принимаем оба направления. */
    int is_dhcp_srv = (pkt[udp_off+0] == 0x00 && pkt[udp_off+1] == 0x43 &&
                       pkt[udp_off+2] == 0x00 && pkt[udp_off+3] == 0x44);  /* 67->68 */
    int is_dhcp_cli = (pkt[udp_off+0] == 0x00 && pkt[udp_off+1] == 0x44 &&
                       pkt[udp_off+2] == 0x00 && pkt[udp_off+3] == 0x43);  /* 68->67 */
    if (!is_dhcp_srv && !is_dhcp_cli) return;
    udp_len = (uint16_t)((pkt[udp_off+4] << 8) | pkt[udp_off+5]);
    if (udp_len < 8) return;

    dh = (uint16_t)(udp_off + 8);     /* BOOTP */
    if ((uint16_t)(dh + 240) > len) return;
    if (pkt[dh] != 0x02) return;      /* op = BOOTREPLY */

    /* W.594→W.669: краткая диагностика входящего DHCP-сообщения (порты, op,
       msg_type из опции 53). Печатаем БЕЗУСЛОВНО (не WLOG): без неё не видно,
       приходит ли ACK/NAK после REQUEST — это и есть текущая неясность
       (OFFER есть, ACK нет). Строка короткая и не в hot-path задержек. */
    {
        int di_u0 = (pkt[udp_off+0]<<8)|pkt[udp_off+1];
        int di_u1 = (pkt[udp_off+2]<<8)|pkt[udp_off+3];
        int di_mt = -1;
        for (int oi = 0; oi + 2 <= udp_len; ) {
            uint8_t c = pkt[dh+240+oi];
            if (c == 0) { oi++; continue; }
            if (c == 255) break;
            uint8_t ol = pkt[dh+240+oi+1];
            if (c == 53 && ol == 1) di_mt = pkt[dh+240+oi+2];
            oi += 2 + ol;
        }
        uart_print("  net: dhcp ");
        uart_print_int(di_u0); uart_print("->"); uart_print_int(di_u1);
        uart_print(" op="); uart_print_int((int)pkt[dh]);
        uart_print(" msg="); uart_print_int(di_mt);
        uart_print("\r\n");
    }

    /* xid: чужой reply игнорируем. */
    if (pkt[dh+4] != (uint8_t)(dhcp_xid >> 24) ||
        pkt[dh+5] != (uint8_t)(dhcp_xid >> 16) ||
        pkt[dh+6] != (uint8_t)(dhcp_xid >> 8)  ||
        pkt[dh+7] != (uint8_t)dhcp_xid) return;

    /* Разбор опций: 53 (тип), 1 (маска), 3 (шлюз), 54 (server id). */
    uint8_t msg_type = 0;
    uint8_t mask[4] = {0,0,0,0}, gw[4] = {0,0,0,0}, srv[4] = {0,0,0,0};
    opt = (uint16_t)(dh + 240);       /* cookie + фикс. часть 236 */
    end = (uint16_t)(dh + (udp_len - 8));
    if (end > len) end = len;
    while (opt + 2 <= end) {
        uint8_t code = pkt[opt];
        if (code == 0) { opt++; continue; }            /* pad */
        if (code == 255) break;                        /* end */
        uint8_t olen = pkt[opt + 1];
        if (opt + 2 + olen > end) break;
        if (code == 53 && olen == 1) {
            msg_type = pkt[opt + 2];
        } else if (code == 1 && olen == 4) {
            for (k = 0; k < 4; k++) mask[k] = pkt[opt + 2 + k];
        } else if (code == 3 && olen >= 4) {
            for (k = 0; k < 4; k++) gw[k] = pkt[opt + 2 + k];
        } else if (code == 54 && olen == 4) {
            for (k = 0; k < 4; k++) srv[k] = pkt[opt + 2 + k];
        }
        opt = (uint16_t)(opt + 2 + olen);
    }

    if (msg_type == 2) {              /* OFFER -> DHCPREQUEST */
        if (pkt[dh+16] == 0 && pkt[dh+17] == 0 && pkt[dh+18] == 0 && pkt[dh+19] == 0)
            return;
        for (k = 0; k < 4; k++) dhcp_offer_ip[k] = pkt[dh+16+k];
        for (k = 0; k < 4; k++) dhcp_server_id[k] = srv[k];
        /* W.650: показать адрес на экране сразу (по OFFER), ACK обновит флаг. */
        for (k = 0; k < 4; k++) {
            net_ip[k] = dhcp_offer_ip[k];
            net_mask[k] = mask[k];
            net_gw[k] = gw[k];
        }
        net_have = 1;
        net_have_ack = 0;
        uart_print("  net: DHCP OFFER ip=");
        for (k = 0; k < 4; k++) { uart_print_int((int)dhcp_offer_ip[k]); if (k<3) uart_print("."); }
        uart_print("\r\n");
        uart_print("  net:   server=");
        for (k = 0; k < 4; k++) { uart_print_int((int)dhcp_server_id[k]); if (k<3) uart_print("."); }
        uart_print("\r\n");
        /* W.621: REQUEST — ТОЛЬКО на ПЕРВЫЙ OFFER (по эталону dhclient:
           клиент в SELECTING отвечает REQUEST один раз и игнорирует
           повторные OFFER; каждый повторный OFFER от сервера не должен
           плодить новый REQUEST — это флуд с одним xid, от которого
           сервер может игнорировать все REQUEST). */
        if (!dhcp_offer_seen) {
            dhcp_offer_seen = 1;
            uart_print("  net: REQUEST sent\r\n");
            int rrc = net_dhcp_request();
            WLOG("  net: REQUEST rc=");
            WLOG_INT(rrc);
            WLOG("\r\n");
        }
        return;
    }

    if (msg_type == 6) {              /* NAK: адрес не выдан */
        uart_print("  net: DHCP NAK\r\n");
        dhcp_offer_seen = 0;
        return;
    }

    if (msg_type == 5) {              /* ACK: фиксируем адрес */
        if (!dhcp_offer_seen) return;
        st->have = 1;
        for (k = 0; k < 4; k++) st->ip[k] = dhcp_offer_ip[k];
        for (k = 0; k < 4; k++) { st->mask[k] = mask[k]; st->gw[k] = gw[k]; st->net[k] = 0; }
        net_have_ack = 1;   /* W.650: адрес подтверждён — на экране пометка ACK */
        uart_print("  net: DHCP ACK\r\n");
        uart_print("  net:   ip=");
        for (k = 0; k < 4; k++) { uart_print_int((int)st->ip[k]); if (k<3) uart_print("."); }
        uart_print("\r\n");
        uart_print("  net:   mask=");
        for (k = 0; k < 4; k++) { uart_print_int((int)st->mask[k]); if (k<3) uart_print("."); }
        uart_print("\r\n");
        uart_print("  net:   gw=");
        for (k = 0; k < 4; k++) { uart_print_int((int)st->gw[k]); if (k<3) uart_print("."); }
        uart_print("\r\n");
    }
}

/* Входящий ARP: отвечаем на request, адресованный нашему IP.
   src_mac — L2-адрес отправителя (addr3/addr2 из 802.11-заголовка). */
void net_handle_arp(const uint8_t *pkt, uint16_t len, const uint8_t *src_mac,
                    struct net_state *st)
{
    uint8_t rep[28];
    uint8_t m[6];
    int k;

    if (!st || !st->have || len < 28) return;
    if (pkt[1] != 0x01 || pkt[2] != 0x08 || pkt[3] != 0x00) return;  /* eth/IPv4 */
    if (pkt[4] != 6 || pkt[5] != 4) return;                          /* MAC/IP */
    if (pkt[6] != 0x00 || pkt[7] != 0x01) return;                    /* request */
    for (k = 0; k < 4; k++) if (pkt[24+k] != st->ip[k]) return;      /* tpa != наш IP */

    rep[0] = 0x00; rep[1] = 0x01;   /* htype = Ethernet */
    rep[2] = 0x08; rep[3] = 0x00;   /* ptype = IPv4 */
    rep[4] = 6; rep[5] = 4;
    rep[6] = 0x00; rep[7] = 0x02;   /* op = reply */
    h3_wifi_get_mac(m);
    for (k = 0; k < 6; k++) rep[8+k] = m[k];          /* SHA = наш MAC */
    for (k = 0; k < 4; k++) rep[14+k] = st->ip[k];    /* SPA = наш IP */
    for (k = 0; k < 6; k++) rep[18+k] = pkt[8+k];     /* THA = запросивший */
    for (k = 0; k < 4; k++) rep[24+k] = pkt[14+k];    /* TPA = его IP */

    uart_print("  net: ARP reply\r\n");
    h3_wifi_tx_data(0x0806, src_mac, rep, 28);
}
