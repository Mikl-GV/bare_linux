#ifndef H3_H
#define H3_H

#include <stdint.h>

/* Карта периферии Allwinner H3 (Orange Pi Lite). */

/* Совместимость с проектом-источником (sega_pad.c не правим): __IO = volatile. */
#ifndef __IO
#define __IO volatile
#endif
#ifndef __I
#define __I  volatile const
#endif
#ifndef _CAST
#define _CAST(t) (t)
#endif

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
#define H3_LCD_BASE       0x01C0C000u
#define H3_HDMI_BASE      0x01EE0000u
#define H3_HDMI_PHY_BASE  (H3_HDMI_BASE + 0x10000u)   /* HDMI PHY (0x01EF0000) */

/* SD/MMC хост-контроллеры (WiFi чип RTL8189FTV сидит на SDC1). */
#define H3_SDC0_BASE      0x01C0F000u
#define H3_SDC1_BASE      0x01C10000u   /* SDC1 — WiFi (PG0-PG5) */
#define H3_SDC2_BASE      0x01C11000u

#define DRAM_BASE         0x40000000u

/* TCON LCD0 (0x01C0C000) — регистры, используемые драйвером дисплея. */
#define H3_LCD0_TCON1_CTL     (*(volatile uint32_t *)(H3_LCD_BASE + 0x090))
#define H3_LCD0_TCON1_BASIC0  (*(volatile uint32_t *)(H3_LCD_BASE + 0x094))
#define H3_LCD0_TCON1_BASIC1  (*(volatile uint32_t *)(H3_LCD_BASE + 0x098))
#define H3_LCD0_TCON1_BASIC2  (*(volatile uint32_t *)(H3_LCD_BASE + 0x09C))
#define H3_LCD0_TCON1_BASIC3  (*(volatile uint32_t *)(H3_LCD_BASE + 0x0A0))
#define H3_LCD0_TCON1_BASIC4  (*(volatile uint32_t *)(H3_LCD_BASE + 0x0A4))
#define H3_LCD0_TCON1_BASIC5  (*(volatile uint32_t *)(H3_LCD_BASE + 0x0A8))
#define H3_LCD0_GCTL          (*(volatile uint32_t *)(H3_LCD_BASE + 0x000))
#define H3_LCD0_GINT0         (*(volatile uint32_t *)(H3_LCD_BASE + 0x004))
#define H3_LCD0_TCON0_DCLK    (*(volatile uint32_t *)(H3_LCD_BASE + 0x044))
#define H3_LCD0_TCON0_IO_TRI  (*(volatile uint32_t *)(H3_LCD_BASE + 0x08C))
#define H3_LCD0_TCON1_IO_TRI  (*(volatile uint32_t *)(H3_LCD_BASE + 0x0F4))

/* HDMI PHY (0x01EF0000) — регистры PHY (black-magic значения из BSP). */
#define H3_HDMI_PHY_POL       (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x00))
#define H3_HDMI_PHY_READ_EN   (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x10))
#define H3_HDMI_PHY_UNSCRAMBL (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x14))
#define H3_HDMI_PHY_CTRL      (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x20))
#define H3_HDMI_PHY_UNK1      (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x24))
#define H3_HDMI_PHY_UNK2      (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x28))
#define H3_HDMI_PHY_PLL       (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x2C))
#define H3_HDMI_PHY_CLK       (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x30))
#define H3_HDMI_PHY_UNK3      (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x34))
#define H3_HDMI_PHY_STATUS    (*(volatile uint32_t *)(H3_HDMI_PHY_BASE + 0x38))

/* DW-HDMI (0x01EE0000) — байтовые регистры ядра (см. dw_hdmi.h). */
#define H3_HDMI_BYTE(off)     (*(volatile uint8_t *)(H3_HDMI_BASE + (off)))

/* High-Speed Timer (0x01C60000) — структура как в проекте-источнике
 * (нужна sega_pad.c/h3_hs_timer.h, драйвер не правим). */
#define H3_HS_TIMER_BASE  0x01C60000u
typedef struct {
    __IO uint32_t IRQ_EN;      /* 0x00 */
    __IO uint32_t IRQ_STAS;    /* 0x04 */
    __I  uint32_t RES1[2];     /* 0x08-0x0C */
    __IO uint32_t CTRL;        /* 0x10 */
    __IO uint32_t INTV_LO;     /* 0x14 */
    __IO uint32_t INTV_HI;     /* 0x18 */
    __IO uint32_t CURNT_LO;    /* 0x1C */
    __IO uint32_t CURNT_HI;    /* 0x20 */
} H3_HS_TIMER_TypeDef;
#define H3_HS_TIMER  (_CAST(H3_HS_TIMER_TypeDef *)(H3_HS_TIMER_BASE))

#endif /* H3_H */