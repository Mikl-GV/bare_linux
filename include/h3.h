#ifndef H3_H
#define H3_H

#include <stdint.h>

/* Карта периферии Allwinner H3 (Orange Pi Lite). */

#define H3_CCU_BASE       0x01C20000u
#define H3_PIO_BASE       0x01C20800u
#define H3_R_PIO_BASE     0x01F02C00u   /* always-on port L */
#define H3_R_PRCM_BASE    0x01F01400u
#define H3_UART0_BASE     0x01C28000u
#define H3_DMA_BASE       0x01C02000u
#define H3_I2S0_BASE      0x01C22000u
#define H3_I2S0_TX_FIFO   0x01C22020u   /* регистр TX FIFO (sun8i) */
#define H3_HSTMR_BASE     0x01C60000u
#define H3_DE_BASE        0x01000000u
#define H3_HDMI_BASE      0x01EE0000u

/* SD/MMC хост-контроллеры (WiFi чип RTL8189FTV сидит на SDC1). */
#define H3_SDC0_BASE      0x01C0F000u
#define H3_SDC1_BASE      0x01C10000u   /* SDC1 — WiFi (PG0-PG5) */
#define H3_SDC2_BASE      0x01C11000u

#define DRAM_BASE         0x40000000u

#endif /* H3_H */