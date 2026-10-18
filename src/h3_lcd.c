#include <stdint.h>

#include "h3.h"
#include "display_timing.h"

/* TCON0/LCD0 (0x01C0C000) — режим TCON1 для HDMI.
   Источник: ref/pico-retro-new-gen h3_lcd.c (Skrabec/van Vught). */

#define LCDC_CTRL_TCON_ENABLE           (1u << 31)
#define LCDC_X(x)                       (((x) - 1) << 16)
#define LCDC_Y(y)                       ((y) - 1)
#define LCDC_TCON1_CTRL_CLK_DELAY(n)    (((n) & 0x1F) << 4)
#define LCDC_TCON1_CTRL_INTERLACE_ENABLE (1u << 20)
#define LCDC_TCON1_CTRL_ENABLE          (1u << 31)
#define LCDC_TCON1_TIMING_H_BP(n)       (((n) - 1) << 0)
#define LCDC_TCON1_TIMING_H_TOTAL(n)    (((n) - 1) << 16)
#define LCDC_TCON1_TIMING_V_BP(n)       (((n) - 1) << 0)
#define LCDC_TCON1_TIMING_V_TOTAL(n)    ((n) << 16)

void h3_lcdc_enable(void)
{
    H3_LCD0_GCTL |= LCDC_CTRL_TCON_ENABLE;
}

static uint32_t lcdc_get_clk_delay(const struct display_timing *mode)
{
    uint32_t delay = mode->vfront_porch.typ + mode->vsync_len.typ
                     + mode->vback_porch.typ;

    if (mode->flags & DISPLAY_FLAGS_INTERLACED)
        delay /= 2;

    delay -= 2;
    return (delay > 30) ? 30 : delay;
}

void h3_lcdc_tcon1_mode_set(const struct display_timing *mode)
{
    uint32_t bp, clk_delay, total, yres;

    clk_delay = lcdc_get_clk_delay(mode);

    H3_LCD0_TCON1_CTL = LCDC_TCON1_CTRL_ENABLE
        | ((mode->flags & DISPLAY_FLAGS_INTERLACED) ? LCDC_TCON1_CTRL_INTERLACE_ENABLE : 0)
        | LCDC_TCON1_CTRL_CLK_DELAY(clk_delay);

    yres = mode->vactive.typ;
    if (mode->flags & DISPLAY_FLAGS_INTERLACED)
        yres /= 2;

    H3_LCD0_TCON1_BASIC0 = LCDC_X(mode->hactive.typ) | LCDC_Y(yres);
    H3_LCD0_TCON1_BASIC1 = LCDC_X(mode->hactive.typ) | LCDC_Y(yres);
    H3_LCD0_TCON1_BASIC2 = LCDC_X(mode->hactive.typ) | LCDC_Y(yres);

    bp    = mode->hsync_len.typ + mode->hback_porch.typ;
    total = mode->hactive.typ + mode->hfront_porch.typ + bp;
    H3_LCD0_TCON1_BASIC3 = LCDC_TCON1_TIMING_H_TOTAL(total) | LCDC_TCON1_TIMING_H_BP(bp);

    bp    = mode->vsync_len.typ + mode->vback_porch.typ;
    total = mode->vactive.typ + mode->vfront_porch.typ + bp;
    if (!(mode->flags & DISPLAY_FLAGS_INTERLACED))
        total *= 2;
    H3_LCD0_TCON1_BASIC4 = LCDC_TCON1_TIMING_V_TOTAL(total) | LCDC_TCON1_TIMING_V_BP(bp);
    H3_LCD0_TCON1_BASIC5 = LCDC_X(mode->hsync_len.typ) | LCDC_Y(mode->vsync_len.typ);
}

void h3_lcdc_init(void)
{
    H3_LCD0_GCTL = 0;
    H3_LCD0_GINT0 = 0;
    H3_LCD0_TCON0_DCLK &= ~(0xFu << 28);   /* disable TCON0 DCLK */
    H3_LCD0_TCON0_IO_TRI = 0xFFFFFFFF;     /* tristate */
    H3_LCD0_TCON1_IO_TRI = 0xFFFFFFFF;
}