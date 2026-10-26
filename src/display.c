#include "display.h"
#include "h3.h"
#include "sys.h"
#include "h3_mmu.h"
#include "font8x8.h"
#include "h3_display.h"
#include "display_timing.h"
#include <string.h>

/* Самостоятельная инициализация дисплея: DE2 + TCON1 + HDMI (референс
   pico-retro-new-gen/h3_bare/platform/fb/). U-Boot больше не настраивает
   DE/HDMI при FEL-загрузке, поэтому инициализируем сами. 
   FB — своя секция из линкера (_fb_hdmi_start, 1024x600 XRGB8888). */

volatile uint32_t *fb;
uint32_t fb_pitch = FB_STRIDE;

extern unsigned char _fb_hdmi_start[];

/* Принудительно сбросить D-cache для диапазона (после записи в FB). */
void fb_clean(void)
{
    uint32_t base = (uint32_t)(uintptr_t)fb & ~0x1Fu;
    uint32_t end  = base + FB_W * FB_H * 4;
    for (uint32_t a = base; a < end; a += 32)
        __asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a)); /* DCCIMVAC */
    __asm volatile("dsb" ::: "memory");
}

int display_init(void)
{
    struct display_timing timing;

    /* Тайминги 1024×600 — взяты из pico-retro-new-gen main.c. */
    memset(&timing, 0, sizeof(timing));
    timing.hdmi_monitor = 0;
    timing.pixelclock.typ  = 51200000;
    timing.hactive.typ     = 1024; timing.hfront_porch.typ = 40;
    timing.hback_porch.typ = 248;  timing.hsync_len.typ = 32;
    timing.vactive.typ     = 600;  timing.vfront_porch.typ = 1;
    timing.vback_porch.typ = 31;   timing.vsync_len.typ = 3;
    timing.flags = (DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_HIGH);

    fb = (volatile uint32_t *)(uintptr_t)_fb_hdmi_start;
    fb_pitch = FB_STRIDE;

    /* DE читает физическую память: свой буфер должен быть uncached. */
    mmu_mark_uncached((uint32_t)(uintptr_t)fb);
    fb_clean();

    return h3_display_init(&timing, (uint32_t)(uintptr_t)fb);
}

/* ================= Рендер (в найденный FB) ================= */
void fb_fill(uint32_t color)
{
    uint32_t pitch_w = fb_pitch / 4;
    for (uint32_t y = 0; y < FB_H; y++) {
        volatile uint32_t *row = fb + y * pitch_w;
        for (uint32_t x = 0; x < FB_W; x++)
            row[x] = color;
    }
}

void fb_rect(int x0, int y0, int x1, int y1, uint32_t color)
{
    uint32_t pitch_w = fb_pitch / 4;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= FB_W) x1 = FB_W - 1;
    if (y1 >= FB_H) y1 = FB_H - 1;
    for (int y = y0; y <= y1; y++) {
        volatile uint32_t *row = fb + (uint32_t)y * pitch_w;
        for (int x = x0; x <= x1; x++)
            row[x] = color;
    }
}

/* Текст. Панель физически отражает по X — рисуем глиф зеркально, чтобы после
   отражения панелью буквы стали нормальными. Позиция (x) НЕ зеркалится —
   просто глиф разворачиваем по горизонтали. */

void fb_text(int x, int y, const char *s, uint32_t color, uint32_t bg)
{
    uint32_t pitch_w = fb_pitch / 4;
    /* Отсечь позиции за пределами кадра (иначе выход за буфер fb[]). */
    if (x >= FB_W || y >= FB_H) return;
    while (*s) {
        const uint8_t *glyph;
        int ch;
        unsigned char c0 = (unsigned char)*s;

        if (c0 >= 0x80) {
            /* UTF-8 двухбайтовый символ. Проверяем наличие второго байта. */
            unsigned int code;
            if ((c0 == 0xD0 || c0 == 0xD1) && s[1] != '\0') {
                unsigned char c1 = (unsigned char)s[1];
                code = (unsigned int)(((c0 & 0x1F) << 6) | (c1 & 0x3F));
                if (code >= 0x410 && code <= 0x44F) {
                    ch = code - 0x410;
                    glyph = font8x8_cyr[ch];
                    s += 2;
                } else {
                    ch = '?';
                    glyph = font8x8_basic[ch - 0x20];
                    s++;
                }
            } else {
                ch = '?';
                glyph = font8x8_basic[ch - 0x20];
                s++;
            }
        } else {
            ch = c0;
            if (ch < 0x20 || ch > 0x7E) ch = '?';
            glyph = font8x8_basic[ch - 0x20];
            s++;
        }
        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                uint32_t px = (uint32_t)(x + gx);
                uint32_t off = (uint32_t)(y + gy) * pitch_w + px;
                fb[off] = bg;
                if (row & (1u << gx))   /* глиф зеркально (панель отражает X) */
                    fb[off] = color;
            }
        }
        x += 8;
    }
}