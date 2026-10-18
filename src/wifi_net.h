#ifndef WIFI_NET_H
#define WIFI_NET_H

#include <stdint.h>

/* Состояние IPv4 после DHCP (задача «пинг с компа»). */
struct net_state {
    uint8_t have;      /* 1 = IP получен */
    uint8_t ip[4];     /* наш IP (x.x.x.199) */
    uint8_t net[4];    /* сеть (например 192.168.43.0) */
    uint8_t mask[4];   /* маска подсети (из DHCP) */
    uint8_t gw[4];     /* шлюз (опция 3, если есть) */
};

/* Главная точка: отправить DHCP DISCOVER и подождать OFFER.
   При успехе заполняет *st (have=1, ip=net+0.199). Возвращает 0 при успехе. */
int net_dhcp_discover(struct net_state *st);

/* Реакция на входящий data-фрейм с ethertype=0x0800 (IPv4).
   Парсит DHCP OFFER/ACK и настраивает IP. Вызывается из RX-обработчика data. */
void net_handle_ipv4(const uint8_t *pkt, uint16_t len, struct net_state *st);

#endif /* WIFI_NET_H */