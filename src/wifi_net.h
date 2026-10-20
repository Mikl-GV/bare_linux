#ifndef WIFI_NET_H
#define WIFI_NET_H

#include <stdint.h>

/* Состояние IPv4 после DHCP. */
struct net_state {
    uint8_t have;      /* 1 = адрес подтверждён DHCP ACK */
    uint8_t ip[4];     /* наш IP (из yiaddr OFFER'а) */
    uint8_t net[4];    /* сеть (не заполняется) */
    uint8_t mask[4];   /* маска подсети (DHCP опция 1) */
    uint8_t gw[4];     /* шлюз (DHCP опция 3) */
};

/* Отправить DHCP DISCOVER (broadcast). Возвращает 0 при успешной отправке. */
int net_dhcp_discover(struct net_state *st);

/* W.592: 1 если DHCP OFFER уже получен (стадия SELECTING, ждём ACK).
   Используется циклом DHCP, чтобы не слать повторный DISCOVER. */
int net_dhcp_offer_pending(void);

/* W.594: сброс состояния DHCP на старте нового join (dhcp_offer_seen=0). */
void net_dhcp_reset(void);

/* Входящий IPv4 (после 802.11 + LLC/SNAP + ethertype).
   src_mac — L2-адрес отправителя из 802.11-заголовка.
   Обрабатывает DHCP (OFFER -> REQUEST, ACK -> фиксация адреса) и ICMP
   echo request (отправляет echo reply). */
void net_handle_ipv4(const uint8_t *pkt, uint16_t len, const uint8_t *src_mac,
                     struct net_state *st);

/* Входящий ARP: отвечает на request, адресованный нашему IP. */
void net_handle_arp(const uint8_t *pkt, uint16_t len, const uint8_t *src_mac,
                    struct net_state *st);

#endif /* WIFI_NET_H */
