#ifndef WIFI_LAYER_H
#define WIFI_LAYER_H

#include <stdint.h>

/* Отдельный слой WiFi (SDIO RTL8189FTV). Вызывается из пункта меню
   "Transfer": прошивка/калибровка идёт только когда пользователь вошёл
   в этот экран, а не при каждом старте. */

/* Инициализировать и запустить WiFi-слой: включить чип (WIFI_EN=PL7),
   инициализировать SDC1, SDIO-карту, загрузку FW, MAC, калибровку LCK,
   RX-тест. При успехе выставляет wifi_active=1. Возвращает 0 при успехе. */
int wifi_layer_start(void);

/* Уже был запущен в этой сессии? */
int wifi_layer_started(void);

#endif /* WIFI_LAYER_H */