#ifndef H3_DISPLAY_H
#define H3_DISPLAY_H

#include "display_timing.h"

/* Самостоятельная инициализация дисплея H3: DE2 + TCON0(LCD0)/TCON1 + HDMI.
   Источник референса: pico-retro-new-gen/h3_bare/platform/fb/. */

/* DE2: включить такты (PLL_DE 432M), reset, настроить MUX0 + UI-канал. */
void de2_composer_init(void);
void de2_mode_set(const struct display_timing *mode, uint32_t bpp,
                  uint32_t address);

/* TCON0/LCD0 — режим TCON1 для HDMI. */
void h3_lcdc_init(void);
void h3_lcdc_tcon1_mode_set(const struct display_timing *mode);
void h3_lcdc_enable(void);

/* HDMI: PHY (black-magic) + ядро DW-HDMI. */
int h3_hdmi_probe(void);
int h3_hdmi_enable(const struct display_timing *edid, uint32_t bpp);

/* Полный подъём дисплея: probe HDMI -> DE2 -> TCON1 -> enable HDMI.
   FB должен быть XRGB8888 1024x600. Возвращает 0 при успехе. */
int h3_display_init(const struct display_timing *timing, uint32_t fbbase);

#endif /* H3_DISPLAY_H */