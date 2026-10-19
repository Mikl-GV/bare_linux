#include "wifi_net.h"
#include "h3_sdc.h"
#include "uart.h"

static uint32_t dhcp_xid = 0x12345678;

static uint16_t csum(const uint8_t *p, uint16_t len)
{
    uint32_t s = 0;
    uint16_t i;
    for (i = 0; i + 1 < len; i += 2) s += (uint16_t)((p[i] << 8) | p[i+1]);
    if (i < len) s += (uint16_t)(p[i] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* Построить Ethernet+IPv4+UDP+DHCP-DISCOVER и отправить через wifi TX data.
   src_ip=0.0.0.0, dst=255.255.255.255:67. */
int net_dhcp_discover(struct net_state *st)
{
    uint8_t pkt[400];
    uint32_t p = 0;
    uint8_t m[6];
    uint16_t i;
    (void)st;   /* st заполняется из OFFER/ACK в net_handle_ipv4 */

    /* IPv4 header (20) */
    pkt[p++] = 0x45;                 /* v4, IHL 5 */
    pkt[p++] = 0x00;                 /* TOS */
    pkt[p++] = 0x01; pkt[p++] = 0x48;/* total len 328 (заполним ниже) */
    pkt[p++] = 0x00; pkt[p++] = 0x01;/* id */
    pkt[p++] = 0x00; pkt[p++] = 0x00;/* flags/frag */
    pkt[p++] = 0x40;                 /* TTL 64 (один байт, offset 8) */
    pkt[p++] = 0x11;                 /* protocol = UDP (offset 9) */
    pkt[p++] = 0x00;                 /* ip csum hi (рассчитаем ниже) */
    pkt[p++] = 0x00;                 /* ip csum lo (рассчитаем ниже) */
    /* src IP 0.0.0.0 */
    pkt[p++] = 0; pkt[p++] = 0; pkt[p++] = 0; pkt[p++] = 0;
    /* dst 255.255.255.255 */
    pkt[p++] = 0xFF; pkt[p++] = 0xFF; pkt[p++] = 0xFF; pkt[p++] = 0xFF;

    /* UDP header (8): src 68, dst 67 */
    pkt[p++] = 0x00; pkt[p++] = 0x44;   /* src 68 */
    pkt[p++] = 0x00; pkt[p++] = 0x43;   /* dst 67 */
    pkt[p++] = 0x01; pkt[p++] = 0x28;   /* udp len 296 */
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* udp csum */

    /* DHCP header (фиксированная часть 236 байт, RFC 2131) */
    pkt[p++] = 0x01; pkt[p++] = 0x01;   /* op=BOOTREQUEST, htype=eth */
    pkt[p++] = 0x06;                    /* hlen=6 */
    pkt[p++] = 0x00;                    /* hops=0 */
    /* xid */
    pkt[p++] = (uint8_t)(dhcp_xid >> 24); pkt[p++] = (uint8_t)(dhcp_xid >> 16);
    pkt[p++] = (uint8_t)(dhcp_xid >> 8);  pkt[p++] = (uint8_t)dhcp_xid;
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* secs=0 */
    /* W.487: broadcast-флаг СНЯТ (0x0000). С флагом 0x8000 AP отвечала OFFER
       broadcast-ом, а расшифровка group-кадра зависела от RXBCUSEDK/GTK.
       Без флага AP шлёт OFFER unicast на наш MAC → расшифровка проверенной
       pairwise-парой (cam4). */
    pkt[p++] = 0x00; pkt[p++] = 0x00;   /* flags: unicast reply */
    /* ciaddr, yiaddr, siaddr, giaddr = 0 (16 байт) — до chaddr */
    for (i = 0; i < 16; i++) pkt[p++] = 0;
    /* chaddr (16 байт): наш MAC + 10 нулей */
    h3_wifi_get_mac(m);
    for (i = 0; i < 6; i++) pkt[p++] = m[i];
    for (i = 0; i < 10; i++) pkt[p++] = 0;
    /* sname (64) + file (128) = нули */
    for (i = 0; i < 192; i++) pkt[p++] = 0;

    /* cookie */
    pkt[p++] = 0x63; pkt[p++] = 0x82; pkt[p++] = 0x53; pkt[p++] = 0x63;
    /* DHCP message type = 1 (DISCOVER), options end */
    pkt[p++] = 53; pkt[p++] = 1; pkt[p++] = 1;
    /* request params: 1 (mask), 3 (gw), 6 (dns), 15 (domain) */
    pkt[p++] = 55; pkt[p++] = 4; pkt[p++] = 1; pkt[p++] = 3; pkt[p++] = 6; pkt[p++] = 15;
    pkt[p++] = 255;                  /* END */

    /* total length поля: по реальному размеру пакета p (не фикс 328). */
    pkt[2] = (uint8_t)(p >> 8); pkt[3] = (uint8_t)p;
    /* W.407 [P3]: UDP length тоже по реальному размеру (p - IP header 20),
       а не зашитый 296. DHCP-сервер (телефон) может дропать пакет, где
       UDP len не совпал с фактическим размером дейтаграммы. */
    pkt[22] = (uint8_t)((p - 20) >> 8); pkt[23] = (uint8_t)(p - 20);
    /* IP checksum (20 байт до UDP) */
    pkt[10] = (uint8_t)(csum(pkt, 20) >> 8);
    pkt[11] = (uint8_t)(csum(pkt, 20) & 0xFF);

    /* W.407 [P2]: диагностика DISCOVER — первые 32 байта (IP-заголовок +
       начало UDP/DHCP) + ключевые поля. Сверять с эталоном: IP proto=11,
       src 0.0.0.0, dst 255.255.255.255, DHCP op=1, flags broadcast 0x80 0x00. */
    uart_print("  dhcp DISCOVER len=");
    uart_print_int((int)p);
    uart_print(" IP.total=");
    uart_print_hex((uint32_t)((pkt[2] << 8) | pkt[3]));
    uart_print(" UDP.len=");
    uart_print_hex((uint32_t)((pkt[22] << 8) | pkt[23]));
    uart_print(" proto=0x");
    uart_print_hex((uint32_t)pkt[9]);
    uart_print(" src=");
    uart_print_hex((uint32_t)((pkt[12]<<24)|(pkt[13]<<16)|(pkt[14]<<8)|pkt[15]));
    uart_print(" dst=");
    uart_print_hex((uint32_t)((pkt[16]<<24)|(pkt[17]<<16)|(pkt[18]<<8)|pkt[19]));
    uart_print(" op=0x");
    uart_print_hex((uint32_t)pkt[28]);
    uart_print(" flags=0x");
    uart_print_hex((uint32_t)((pkt[38] << 8) | pkt[39]));
    uart_print("\r\n");

    return h3_wifi_tx_data(0x0800, pkt, p);
}

/* Входящий IPv4 (уже после 802.11 data + LLC/SNAP + ethertype на входе).
   Парсим UDP/DHCP: если OFFER/ACK (msg type 2/5) на наш xid — берём yiaddr
   как выданный AP адрес напрямую (не подставляем .199). */
void net_handle_ipv4(const uint8_t *pkt, uint16_t len, struct net_state *st)
{
    /* pkt указывает на начало IPv4-заголовка. Минимум 20+8+dhcp. */
    if (!st || len < 20 + 8 + 240) return;
    uint8_t ihl = (uint8_t)((pkt[0] & 0x0F) * 4);
    if (pkt[9] != 0x11) return;      /* не UDP */
    uint16_t udp_off = ihl;
    uint16_t udp_len = (uint16_t)((pkt[udp_off+4] << 8) | pkt[udp_off+5]);
    if (udp_len < 8) return;
    if (pkt[udp_off+0] != 0x00 || pkt[udp_off+3] != 0x43) return;   /* src 67 */

    uint16_t dh = udp_off + 8;
    if ((dh+4) <= len && (pkt[dh] == 0x02 || pkt[dh] == 0x05)) { /* BOOTREPLY/OFFER/ACK */
        uint8_t yiaddr[4] = { pkt[dh+16], pkt[dh+17], pkt[dh+18], pkt[dh+19] };
        if (yiaddr[0] == 0) return;
        st->have = 1;
        for (int k = 0; k < 4; k++) { st->ip[k] = yiaddr[k]; st->net[k] = yiaddr[k]; }
        uart_print("  net: DHCP OK ip=");
        uart_print_int((int)st->ip[0]); uart_print(".");
        uart_print_int((int)st->ip[1]); uart_print(".");
        uart_print_int((int)st->ip[2]); uart_print(".");
        uart_print_int((int)st->ip[3]);
        uart_print("\r\n");
    }
}