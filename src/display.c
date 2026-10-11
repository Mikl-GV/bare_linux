#include "display.h"
#include "h3.h"
#include "sys.h"
#include "h3_mmu.h"
#include "font8x8.h"

/* U-Boot уже настроил HDMI/DE2 (показывает свой лог). Берём его готовый
   фреймбуфер: читаем адрес и pitch у DE2 (UI-канал), пишем прямо туда.
   НЕ переинициализируем DE/HDMI/TCON — это ломало картинку. */

/* Адреса DE2 MUX0 / UI-канал (H3 datasheet / reference h3_de2.h). */
#define DE_BASE H3_DE_BASE              /* 0x01000000 */
#define MUX0     (DE_BASE + 0x100000)   /* 0x01100000 */
#define UI_BASE  (MUX0 + 0x3000)        /* 0x01103000: chan1 (UI) */
#define UI_PITCH     (*(volatile uint32_t *)(UI_BASE + 0x0C))
#define UI_TOP_LADDR (*(volatile uint32_t *)(UI_BASE + 0x10))

volatile uint32_t *fb;
uint32_t fb_pitch = FB_STRIDE;

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
    uint32_t laddr = UI_TOP_LADDR;
    uint32_t pitch = UI_PITCH;

    /* Принимаем любой DRAM-адрес (U-Boot кладёт консольный FB в 0x5E/0x5F). */
    if (laddr >= 0x40000000u && laddr < 0x60000000u) {
        fb = (volatile uint32_t *)laddr;
        if (pitch >= FB_STRIDE && pitch <= 8192)
            fb_pitch = pitch;
        else
            fb_pitch = FB_STRIDE;
        /* DE читает физическую память: пометить буфер uncached, иначе D-cache
           держит свежезаписанные строки и DE видит старое. */
        mmu_mark_uncached(laddr);
        fb_clean();
    } else {
        extern unsigned char _fb_hdmi_start[];
        fb = (volatile uint32_t *)(uintptr_t)_fb_hdmi_start;
        fb_pitch = FB_STRIDE;
    }
    return 0;
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