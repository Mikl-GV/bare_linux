#ifndef H3_CCU_H
#define H3_CCU_H

#include <stdint.h>
#include "h3.h"

/* Clock Control Unit - тактирование/сбросы периферии Allwinner H3.
   Только используемые в проекте регистры/биты. */

#define H3_CCU_PLL_AUDIO_CTRL  (*(volatile uint32_t *)(H3_CCU_BASE + 0x008))
#define H3_CCU_PLL_AUDIO_PAT   (*(volatile uint32_t *)(H3_CCU_BASE + 0x284))

#define H3_CCU_GATING0      (*(volatile uint32_t *)(H3_CCU_BASE + 0x060))
#define H3_CCU_GATING2      (*(volatile uint32_t *)(H3_CCU_BASE + 0x068))
#define H3_CCU_GATING3      (*(volatile uint32_t *)(H3_CCU_BASE + 0x06C))

#define H3_CCU_RST0         (*(volatile uint32_t *)(H3_CCU_BASE + 0x2C0))
#define H3_CCU_RST3         (*(volatile uint32_t *)(H3_CCU_BASE + 0x2D0))
#define H3_CCU_RST4         (*(volatile uint32_t *)(H3_CCU_BASE + 0x2D8))

#define H3_CCU_I2SPCM0_CLK  (*(volatile uint32_t *)(H3_CCU_BASE + 0x0B0))

/* I2S0 module clock: по даташиту 4.3.5.28.
 * Смещение 0x0B0, биты [17:16] CLK_SRC_SEL:
 *   00 = PLL_AUDIO (8X)
 *   01 = PLL_AUDIO(8X)/2
 *   10 = PLL_AUDIO(8X)/4
 *   11 = PLL_AUDIO          <- нам нужен основной 1X (24.576M)
 * bit31 = SCLK_GATING.
 * BCLKDIV внутри I2S (CLKD reg, 8=val5) даёт BCLK=24.576M/8=3.072M для 48k.
 * (Ранее ставили 0<<16 = 8X - неверно, BCLK получался не 3.072M.) */
#define H3_I2SPCM0_CLKSRC_MASK  (3u << 16)
#define H3_I2SPCM0_MUX_PLLAUDIO (3u << 16)   /* 11 -> PLL_AUDIO 1X */
#define H3_I2SPCM0_GATE         (1u << 31)

/* Биты GATING0/RST0 - DMA (бит6). */
#define H3_CCU_GATE_DMA      (1u << 6)
#define H3_CCU_RST_DMA       (1u << 6)

/* SDC1 (WiFi) — тактирование/сброс в CCU.
   GATING0 bit9 = MMC1 gate, RST0 bit9 = SD/MMC1 reset. */
#define H3_CCU_GATE_MMC1     (1u << 9)
#define H3_CCU_RST_MMC1      (1u << 9)

/* SDMMC1_CLK_REG (0x8C): bit31=SCLK_GATING, bit30=MMC1_MODE_SELECT(new),
   bits[25:24]=CLK_SRC_SEL (00=OSC24M, 01=PLL_PERIPH0), bits[22:20]=SAMPLE_PHASE,
   bits[17:16]=CLK_DIV_N, bits[10:8]=OUTPUT_PHASE, bits[3:0]=CLK_DIV_M. */
#define H3_CCU_SDMMC1_CLK    (*(volatile uint32_t *)(H3_CCU_BASE + 0x08C))
#define H3_SDMMC1_SCLK_GATE  (1u << 31)
#define H3_SDMMC1_MODE_NEW   (1u << 30)
#define H3_SDMMC1_CLK_SRC_OSC24M   (0u << 24)
#define H3_SDMMC1_CLK_SRC_PLLPERI0 (1u << 24)
#define H3_SDMMC1_CLK_DIV_N(v)     ((v) << 16)
#define H3_SDMMC1_CLK_DIV_M(v)     ((v) << 0)

/* Port L always-on: APB0 gating (в R_PRCM). */
#define H3_APB0_CLK_GATING   (*(volatile uint32_t *)(H3_R_PRCM_BASE + 0x28))

#endif /* H3_CCU_H */