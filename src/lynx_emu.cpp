/* Слой Atari Lynx (Handy) — независимый эмулятор для MultiTool.
 *
 * CPU0 (эмулятор): CSystem (ядро Handy) — в этой функции; цикл по
 * CYCLES_PER_FRAME (75 Гц), кадр 160x102 (портрет) рендерится в fb
 * (HDMI 1024x600): поворот на 90° и масштаб, чтобы вертикальная картинка
 * Lynx читалась на горизонтальном мониторе. Звук: FetchAudioSamples()
 * заполняет глобальный gAudioBuffer (стерео 48 кГц, int16), куски
 * уходят на CPU1 через почту MB_MSG_AUDIO_DATA (там i2s_write + DMA).
 * Кнопки: PCF8574 -> BTN_* -> биты Handy (SetButtonData).
 *
 * Независимый слой: завязан только на lynx/*.cpp, emulator.h, display.h,
 * mailbox.h, pcf8574.h, uart.h. На другие слои не влияет. Сборка только
 * чистая (make clean && make -j8 bin).
 */

/* C-заголовки — функции с C-линковкой (проект компилируется как C). */
extern "C" {
#include "emulator.h"
#include "display.h"
#include "mailbox.h"
#include "pcf8574.h"
#include "uart.h"
#include "systems.h"
}
#include <string.h>

/* Ядро Handy (C++). */
#include "system.h"

/* C-функции i2s для звука CPU0->CPU1 (без манглинга). */
extern "C" {
void i2s_start(void);
void i2s_stop(void);
}

/* Оригинальное разрешение Lynx (портрет). */
#define LYNX_W     160
#define LYNX_H     102

/* Тактовая Lynx (65SC02 4 МГц эффективно на 16 МГц таймере) и кадры. */
#define LYNX_CLK   16000000u
#define LYNX_FPS   75u
#define CYCLES_PER_FRAME (LYNX_CLK / LYNX_FPS)

/* Биты кнопок Handy (susie.h). */
#define LB_A      0x0001u
#define LB_B      0x0002u
#define LB_OPT2   0x0004u
#define LB_OPT1   0x0008u
#define LB_LEFT   0x0010u
#define LB_RIGHT  0x0020u
#define LB_UP     0x0040u
#define LB_DOWN   0x0080u
#define LB_PAUSE  0x0100u

static uint8_t g_lynx_fb[LYNX_W * LYNX_H * 4];
static CSystem *g_sys = 0;
static int g_lynx_registered = 0;

/* Дисплейный callback Mikie: вернуть адрес буфера кадра. */
static UBYTE *lynx_display_get(ULONG objref)
{
    (void)objref;
    return (UBYTE *)g_lynx_fb;
}

/* Кнопки платформы -> биты Handy. */
static uint32_t lynx_buttons_from_platform(void)
{
    uint32_t b = pcf8574_read();
    uint32_t h = 0;
    if (b & BTN_UP)     h |= LB_UP;
    if (b & BTN_DOWN)   h |= LB_DOWN;
    if (b & BTN_LEFT)   h |= LB_LEFT;
    if (b & BTN_RIGHT)  h |= LB_RIGHT;
    if (b & BTN_A)      h |= LB_A;
    if (b & BTN_B)      h |= LB_B;
    if (b & BTN_START)  h |= LB_OPT1;    /* Option 1 */
    if (b & BTN_SELECT) h |= LB_OPT2;    /* Option 2 */
    return h;
}

/* Вывод кадра: портрет 160x102 поворачиваем на 90° (куда — лево/право
 * зависит от картриджа; по умолчанию LEFT, как в эталоне для «EXCLUSIVE
 * ROTATE» большинства игр). Масштаб x3 -> 306x480, по центру. Пиксель
 * 32bpp Handy = 0x00RRGGBB = формат fb (XRGB8888). */
static void lynx_blit_frame(void)
{
    uint32_t *fbp = (uint32_t *)fb;
    const uint32_t *src = (const uint32_t *)g_lynx_fb;
    const int S = 3;
    const int ow = LYNX_H * S;   /* 306 */
    const int oh = LYNX_W * S;   /* 480 */
    const int ox = (FB_W - ow) / 2;
    const int oy = (FB_H - oh) / 2;

    /* Чёрный фон. */
    for (int y = 0; y < FB_H; y++) {
        uint32_t *row = fbp + (uint32_t)y * (FB_STRIDE / 4);
        for (int x = 0; x < FB_W; x++) row[x] = 0x00000000u;
    }

    /* Поворот против часовой: пиксель (x,y) источника -> цель (y, W-1-x).
     * Каждый пиксель размножаем в квадрат SxS. */
    for (int y = 0; y < LYNX_H; y++) {
        for (int x = 0; x < LYNX_W; x++) {
            uint32_t c = src[(uint32_t)y * LYNX_W + (uint32_t)x];
            int tx0 = ox + y * S;
            int ty0 = oy + (LYNX_W - 1 - x) * S;
            for (int dy = 0; dy < S; dy++) {
                uint32_t *row = fbp + (uint32_t)(ty0 + dy) * (FB_STRIDE / 4);
                for (int dx = 0; dx < S; dx++) row[tx0 + dx] = c;
            }
        }
    }
    fb_clean();
}

/* Ядро Lynx: блокирующий цикл эмуляции. */
static int lynx_core_run(const struct emu_run *run)
{
    /* Ром — уже в RAM (ROM-зона, run->rom_addr/rom_size).
     * BIOS: useEmu=1 (HLE, без lynxboot.img); EEPROM: NULL (без файла). */
    g_sys = new CSystem(NULL,
                        (const UBYTE *)run->rom_addr,
                        (ULONG)run->rom_size,
                        NULL,
                        true,
                        NULL);
    if (!g_sys) return EMU_ERR_MEM;

    /* Дисплей: оригинальный 160x102, 32bpp, поворот LEFT (портрет). */
    g_sys->mMikie->DisplaySetAttributes(MIKIE_ROTATE_L,
                                        MIKIE_PIXEL_FORMAT_32BPP,
                                        LYNX_W * 4,
                                        lynx_display_get, 0);

    uart_print("lynx: run\r\n");

    /* Запустить кольцо DMA; CPU1 через почту будет писать в него. */
    i2s_start();

    for (;;) {
        if (emu_check_exit()) break;
        g_sys->SetButtonData(lynx_buttons_from_platform());

        /* Один кадр. */
        ULONG start = gSystemCycleCount;
        while ((ULONG)(gSystemCycleCount - start) < CYCLES_PER_FRAME)
            g_sys->Update();

        /* Кадр -> экран. */
        lynx_blit_frame();

        /* Звук: стерео int16 в gAudioBuffer; pointer — сэмплы канала
         * (кадры = pointer/2). Куски на CPU1 -> i2s. */
        g_sys->FetchAudioSamples();
        if (gAudioBufferPointer >= 2) {
            mb_send(MB_MSG_AUDIO_DATA,
                    (uint32_t)(uintptr_t)(const void *)gAudioBuffer,
                    gAudioBufferPointer >> 1);
            gAudioBufferPointer = 0;
        }
    }

    uart_print("lynx: exit\r\n");
    i2s_stop();
    delete g_sys;
    g_sys = 0;
    return EMU_OK;
}

/* Регистрация ядра (идём по systems[]: Lynx — id "lynx"). */
/* Регистрация ядра вызывается из C (retro_menu.c) — без манглинга. */
extern "C" void lynx_register(void);

static void lynx_register_impl(void)
{
    if (g_lynx_registered) return;
    for (int i = 0; i < NUM_SYSTEMS; i++) {
        if (strcmp(systems[i].id, "lynx") == 0) {
            emulator_register_core((uint32_t)i, lynx_core_run);
            g_lynx_registered = 1;
            return;
        }
    }
    /* На всякий случай — индекс 1 (порядок: gameboy, lynx). */
    emulator_register_core(1u, lynx_core_run);
    g_lynx_registered = 1;
}

/* Регистрация ядра вызывается из C (retro_menu.c) — без манглинга. */
extern "C" void lynx_register(void)
{
    lynx_register_impl();
}