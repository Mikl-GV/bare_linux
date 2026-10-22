#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/* HDMI + DE2 + TCON1, вывод на 1024x600 (XRGB8888).
   Шина: DE MIXER0 -> TCON_LCD0 -> HDMI. */

/* Разрешение */
#define FB_W 1024
#define FB_H 600
#define FB_STRIDE (FB_W * 4)   /* XRGB8888 */

/* Общий фреймбуфер. */
extern volatile uint32_t *fb;
extern uint32_t fb_pitch;   /* байт на строку (обычно FB_STRIDE) */

/* Инициализация: берём готовый FB у U-Boot (или свой из линкера). */
int display_init(void);

/* Сбросить D-cache для фреймбуфера (после записи, DE видит свежие строки). */
void fb_clean(void);

/* Простейший рендер для теста: заливка цветом и сетка. */
void fb_fill(uint32_t color);

/* Нарисовать закрашенный прямоугольник. */
void fb_rect(int x0, int y0, int x1, int y1, uint32_t color);

/* Нарисовать текст (8x8 bitmap) в позицию. */
void fb_text(int x, int y, const char *s, uint32_t color, uint32_t bg);

#endif /* DISPLAY_H */