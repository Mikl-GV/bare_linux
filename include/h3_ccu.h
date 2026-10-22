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

/* SDC0 (слот SD-карты) — тактирование/сброс в CCU.
   GATING0 bit8 = MMC0 gate, RST0 bit8 = SD/MMC0 reset. */
#define H3_CCU_GATE_MMC0     (1u << 8)
#define H3_CCU_RST_MMC0      (1u << 8)

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

/* SDMMC0_CLK_REG (0x88) — слот SD-карты. Формат битов тот же, что у
   SDMMC1_CLK (SCLK_GATE=31, MODE_SELECT=30, CLK_SRC_SEL[25:24],
   DIV_N[17:16], DIV_M[3:0]). */
#define H3_CCU_SDMMC0_CLK    (*(volatile uint32_t *)(H3_CCU_BASE + 0x088))
#define H3_SDMMC0_SCLK_GATE  (1u << 31)
#define H3_SDMMC0_MODE_NEW   (1u << 30)
#define H3_SDMMC0_CLK_SRC_OSC24M   (0u << 24)
#define H3_SDMMC0_CLK_SRC_PLLPERI0 (1u << 24)
#define H3_SDMMC0_CLK_DIV_N(v)     ((v) << 16)
#define H3_SDMMC0_CLK_DIV_M(v)     ((v) << 0)

/* Port L always-on: APB0 gating (в R_PRCM). */
#define H3_APB0_CLK_GATING   (*(volatile uint32_t *)(H3_R_PRCM_BASE + 0x28))

/* ==== Драйвер дисплея: CCU (GATING1/RST1, PLL_DE, DE/TCON0/HDMI такты) ==== */

/* BUS_CLK_GATING1 (0x064), BUS_SOFT_RESET1 (0x2C4). */
#define H3_CCU_GATING1   (*(volatile uint32_t *)(H3_CCU_BASE + 0x064))
#define H3_CCU_RST1      (*(volatile uint32_t *)(H3_CCU_BASE + 0x2C4))

/* PLL_VIDEO (0x010) и PLL_DE (0x048) — контрольные регистры. */
#define H3_CCU_PLL_VIDEO_CTRL (*(volatile uint32_t *)(H3_CCU_BASE + 0x010))
#define H3_CCU_PLL_DE_CTRL    (*(volatile uint32_t *)(H3_CCU_BASE + 0x048))

/* DE (Display Engine) такт 0x104, TCON0 такт 0x118, HDMI такты 0x150/0x154. */
#define H3_CCU_DE_CLK     (*(volatile uint32_t *)(H3_CCU_BASE + 0x104))
#define H3_CCU_TCON0_CLK  (*(volatile uint32_t *)(H3_CCU_BASE + 0x118))
#define H3_CCU_HDMI_CLK   (*(volatile uint32_t *)(H3_CCU_BASE + 0x150))
#define H3_CCU_HDMI_SLOW_CLK (*(volatile uint32_t *)(H3_CCU_BASE + 0x154))

/* Биты GATING1 (bus gate). */
#define H3_CCU_GATING1_TCON0  (1u << 3)
#define H3_CCU_GATING1_HDMI   (1u << 11)
#define H3_CCU_GATING1_DE     (1u << 12)

/* Биты RST1 (bus soft reset). */
#define H3_CCU_RST1_TCON0   (1u << 3)
#define H3_CCU_RST1_HDMI2   (1u << 10)
#define H3_CCU_RST1_HDMI    (1u << 11)
#define H3_CCU_RST1_DE      (1u << 12)

/* PLL_DE: EN(31), LOCK(28), integer mode(24), N[14:8], M[3:0]. */
#define H3_PLL_DE_ENABLE        (1u << 31)
#define H3_PLL_DE_LOCK          (1u << 28)
#define H3_PLL_DE_INTEGER_MODE  (1u << 24)
#define H3_PLL_DE_N(n)          ((((n) - 1) & 0x7F) << 8)
#define H3_PLL_DE_M(n)          ((((n) - 1) & 0x0F) << 0)

/* DE_CLK (0x104): sclk gate(31), CLK_SRC_SEL[25:24] (1=PLL_DE). */
#define H3_DE_CLK_GATE        (1u << 31)
#define H3_DE_CLK_SRC_MASK    (3u << 24)
#define H3_DE_CLK_SRC_PLL_DE  (1u << 24)

/* TCON0_CLK (0x118): sclk gate(31), M[3:0] divider. */
#define H3_TCON0_CLK_GATE (1u << 31)
#define H3_TCON0_CLK_M(n) ((((n) - 1) & 0x0F) << 0)

/* HDMI_CLK (0x150): sclk gate(31), PLL_SRC_SEL[25:24] (0=PLL_VIDEO). */
#define H3_HDMI_CLK_GATE       (1u << 31)
#define H3_HDMI_CLK_PLL_MASK   (3u << 24)
#define H3_HDMI_CLK_PLL_VIDEO  (0u << 24)

/* HDMI_SLOW_CLK (0x154): DDC gate(31). */
#define H3_HDMI_SLOW_DDC_GATE  (1u << 31)

/* PLL_VIDEO (0x010): EN(31), LOCK(28), integer(24), N[14:8], M[3:0]. */
#define H3_PLL_VIDEO_EN        (1u << 31)
#define H3_PLL_VIDEO_LOCK      (1u << 28)
#define H3_PLL_VIDEO_INTEGER   (1u << 24)
#define H3_PLL_VIDEO_N(n)      ((((n) - 1) & 0x7F) << 8)
#define H3_PLL_VIDEO_M(n)      ((((n) - 1) & 0x0F) << 0)

#endif /* H3_CCU_H */