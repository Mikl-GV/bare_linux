/* gameboy_host.c — host-слой Game Boy / Game Boy Color (binjgb) для
 * MultiTool, по контракту слоя (emu_io.h).
 *
 * Жизнь эмулятора: input_poll -> emulate кадр -> audio_push -> blit.
 * Ядро НЕ знает про PCF8574/USB/I2S/DMA — всё через emu_io.
 *
 * API binjgb (src/gameboy/emulator.h):
 *   emulator_new(&init)          — rom, audio_frequency
 *   emulator_run_until(e, ...ticks + PPU_FRAME_TICKS) — один кадр
 *   emulator_get_frame_buffer    — RGBA (32-бит) SCREEN_WIDTHxHEIGHT
 *   emulator_get_audio_buffer + audio_buffer_get_frames — u8 стерео (0..255)
 *   emulator_set_joypad_buttons  — кнопки (JoypadButtons)
 */

#include <stdint.h>
#include <string.h>

extern "C" {
#include "emulator.h"
#include "display.h"
#include "emu_io.h"
#include "systems.h"
#include "sys.h"
#include "uart.h"
#include "i2s.h"
}

#include "gameboy/emulator.h"
#include "gameboy/common.h"

static Emulator *g_emu;

/* ---- Кнопки: контракт emu_io (PAD_*) -> JoypadButtons binjgb ---- */
static JoypadButtons gb_buttons_from_platform(void)
{
    struct emu_input in;
    emu_input_poll(&in);
    uint16_t b = in.pads[0];
    JoypadButtons jp;
    memset(&jp, 0, sizeof(jp));
    jp.up    = (b & PAD_UP)    ? TRUE : FALSE;
    jp.down  = (b & PAD_DOWN)  ? TRUE : FALSE;
    jp.left  = (b & PAD_LEFT)  ? TRUE : FALSE;
    jp.right = (b & PAD_RIGHT) ? TRUE : FALSE;
    jp.A     = (b & PAD_A)     ? TRUE : FALSE;
    jp.B     = (b & PAD_B)     ? TRUE : FALSE;
    jp.start = (b & PAD_START) ? TRUE : FALSE;
    jp.select= (b & PAD_SELECT)? TRUE : FALSE;
    return jp;
}

/* ---- Кадр: 160x144 RGBA -> центр экрана 1024x600, масштаб x3 ---- */
static void gb_blit_frame(void)
{
    FrameBuffer *fbuf = emulator_get_frame_buffer(g_emu);
    if (!fbuf) return;
    const uint32_t *src = (const uint32_t *)*fbuf;
    const int SW = SCREEN_WIDTH, SH = SCREEN_HEIGHT;
    const int S = 3;
    const int ox = (FB_W - SW * S) / 2;
    const int oy = (FB_H - SH * S) / 2;

    for (int y = 0; y < FB_H; y++) {
        uint32_t *row = (uint32_t *)fb + (uint32_t)y * (fb_pitch / 4);
        for (int x = 0; x < FB_W; x++) row[x] = 0x00000000u;
    }

    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            uint32_t c = src[(uint32_t)y * SW + (uint32_t)x];
            for (int dy = 0; dy < S; dy++) {
                uint32_t *row = (uint32_t *)fb + (uint32_t)(oy + y * S + dy) * (fb_pitch / 4);
                for (int dx = 0; dx < S; dx++) row[ox + x * S + dx] = c;
            }
        }
    }
    fb_clean();
}

/* ---- Ядро: блокирующий цикл (как lynx_core_run) ---- */
static int gb_core_run(const struct emu_run *run)
{
    /* binjgb использует gb_heap_reset? Нет: это в host pico-retro (свой
       аллокатор). У нас heap из run. ROM в run->rom_addr/rom_size. */
    EmulatorInit init;
    memset(&init, 0, sizeof(init));
    init.rom.data = (uint8_t *)(uintptr_t)run->rom_addr;
    init.rom.size = run->rom_size;
    init.audio_frequency = 48000;
    init.audio_frames = 1024;
    init.random_seed = 42;
    init.force_dmg = FALSE;
    init.cgb_color_curve = CGB_COLOR_CURVE_NONE;

    g_emu = emulator_new(&init);
    if (!g_emu) return -1;

    uart_print("gb: run\r\n");
    uart_flush();
    uart_print("gb: vol=");
    uart_print_int((int)i2s_get_volume());
    uart_print("\r\n");
    uart_flush();
    emu_audio_start();

    uint32_t dbg = 0;
    for (;;) {
        if (dbg < 20) { uart_print("gb: in\r\n"); uart_flush(); }
        if (emu_input_exit_hold(EMU_EXIT_HOLD_MS)) break;
        if (dbg < 20) { uart_print("gb: jp\r\n"); uart_flush(); }
        JoypadButtons jp = gb_buttons_from_platform();
        emulator_set_joypad_buttons(g_emu, &jp);
        if (dbg < 20) { uart_print("gb: setj\r\n"); uart_flush(); }

        /* Один кадр: до следующего PPU-кадра. */
        Ticks start = emulator_get_ticks(g_emu);
        EmulatorEvent ev = 0;
        uint32_t guard = 0;
        do {
            ev = emulator_run_until(g_emu, start + PPU_FRAME_TICKS);
        } while (!(ev & EMULATOR_EVENT_NEW_FRAME) && ++guard < 4);
        if (dbg < 20) { uart_print("gb: tick done\r\n"); uart_flush(); }

        gb_blit_frame();
        if (dbg < 20) { uart_print("gb: blit done\r\n"); uart_flush(); }

        /* Звук: u8 стерео (0..255) -> s16, отдаём слою звука.
           Буфер СТАТИЧЕСКИЙ: CPU1 читает его асинхронно (i2s_write по почте),
           стековый tmp был бы перезаписан новым кадром до подтверждения. */
        /* Буфер куска по максимуму init.audio_frames (1024) — при 60 Гц
           ядро генерит ~800 стереокадров на кадр, потолок 512 давал недобор
           (звук молчал/голодал DMA). */
        /* Буфер куска по максимуму init.audio_frames (1024) — при 60 Гц
           ядро генерит ~800 стереокадров на кадр, потолок 512 давал недобор.
           Эталон (pico-retro-new-gen gameboy_host.cpp) читает ВСЕ накопленные
           кадры и затем сбрасывает ab->position — повторяем его схему. */
        static int16_t tmp[1024 * 2];
        AudioBuffer *ab = emulator_get_audio_buffer(g_emu);
        u32 nf = audio_buffer_get_frames(ab);
        if (nf > 1024) nf = 1024;
        if (dbg < 20) { uart_print("gb: audio nf="); uart_print_int((int)nf); uart_print("\r\n"); uart_flush(); }
        for (u32 i = 0; i < nf; i++) {
            tmp[2 * i]     = (int16_t)((int32_t)ab->data[2 * i] - 128) << 8;
            tmp[2 * i + 1] = (int16_t)((int32_t)ab->data[2 * i + 1] - 128) << 8;
        }
        if (nf) { if (dbg < 20) { uart_print("gb: push\r\n"); uart_flush(); } emu_audio_push(tmp, nf); }
        if (dbg < 20) { uart_print("gb: push done\r\n"); uart_flush(); }
        /* Эталон сбрасывает позицию ПОСЛЕ потребления — буфер перезапишется. */
        ab->position = ab->data;

        if (dbg < 20) { uart_print("gb: frame #"); uart_print_int((int)dbg); uart_print("\r\n"); uart_flush(); }
        dbg++;

        /* Кадровый ритм ~60 Гц (как emu_throttle в эталоне pico-retro-new-gen).
           Без него binjgb производит ~804 сэмпла/кадр быстрее, чем DMA
           потребляет — звуковое кольцо переполняется, сэмплы теряются
           (шумы/пульсации), ритм разваливается. Ждём остаток до периода. */
        uint32_t prev = micros();
        while ((uint32_t)(micros() - prev) < 16667) { /* сброс */ }
    }

    uart_print("gb: exit\r\n");
    emu_audio_stop();
    emulator_delete(g_emu);
    g_emu = 0;
    return 0;
}

/* Регистрация ядра (вызывается из C: retro_menu.c). */
extern "C" void gameboy_register(void);
static int g_reg = 0;
static void gb_register_impl(void)
{
    if (g_reg) return;
    for (int i = 0; i < NUM_SYSTEMS; i++) {
        if (strcmp(systems[i].id, "gameboy") == 0) {
            emulator_register_core((uint32_t)i, gb_core_run);
            g_reg = 1;
            return;
        }
    }
    g_reg = 1;
}
extern "C" void gameboy_register(void) { gb_register_impl(); }