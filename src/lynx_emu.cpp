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
#include "uart.h"
#include "systems.h"
#include "sys.h"
}
#include "emu_io.h"
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

static uint16_t g_lynx_fb[LYNX_W * LYNX_H];   /* RGB565, как в эталоне */
static CSystem *g_sys = 0;
static int g_lynx_registered = 0;
static volatile int g_lynx_frame_ready = 0;   /* 1 = кадр отрисован (callback) */
static uint32_t g_last_audio_sent = 0;   /* число отправленных аудиокусков (для ack) */

/* Дисплейный callback Mikie: вернуть адрес буфера кадра. Mikie вызывает его
 * в конце кадра — по факту готовности выходим из кадрового цикла (эталон
 * pico-retro lynx_host.cpp: exit по lynx_frame_ready, а не только счётчик). */
static UBYTE *lynx_display_get(ULONG objref)
{
    (void)objref;
    g_lynx_frame_ready = 1;
    return (UBYTE *)g_lynx_fb;
}

/* Кнопки платформы -> биты Handy. Ввод через слой emu_io (ясдро не знает
 * про PCF8574/USB/SEGA): emu_input_poll сводит все источники к PAD_*. */
static uint32_t lynx_buttons_from_platform(void)
{
    struct emu_input in;
    emu_input_poll(&in);
    uint16_t b = in.pads[0];
    uint32_t h = 0;
    if (b & PAD_UP)     h |= LB_UP;
    if (b & PAD_DOWN)   h |= LB_DOWN;
    if (b & PAD_LEFT)   h |= LB_LEFT;
    if (b & PAD_RIGHT)  h |= LB_RIGHT;
    if (b & PAD_A)      h |= LB_A;
    if (b & PAD_B)      h |= LB_B;
    if (b & PAD_START)  h |= LB_OPT1;    /* Option 1 */
    if (b & PAD_SELECT) h |= LB_OPT2;    /* Option 2 */
    return h;
}

/* Вывод кадра: буфер RGB565 160x102 (как в эталоне pico-retro), поворот на
 * 90° и масштаб x3. Пиксель 565 -> XRGB8888 для нашего fb. */
static void lynx_blit_frame(void)
{
    uint32_t *fbp = (uint32_t *)fb;
    const uint16_t *src = g_lynx_fb;
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

    /* Поворот против часовой: (x,y) источника -> (y, W-1-x). RGB565->888. */
    for (int y = 0; y < LYNX_H; y++) {
        for (int x = 0; x < LYNX_W; x++) {
            uint16_t p = src[(uint32_t)y * LYNX_W + (uint32_t)x];
            uint32_t r = (uint32_t)((p >> 11) & 0x1F) << 3;
            uint32_t g = (uint32_t)((p >> 5)  & 0x3F) << 2;
            uint32_t b = (uint32_t)(p & 0x1F) << 3;
            uint32_t c = (r << 16) | (g << 8) | b;
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
                        false,          /* как в эталоне pico-retro: свой ROM-загрузчик */
                        NULL);
    if (!g_sys) {
        uart_print("lynx: new CSystem fail (mem?)\r\n");
        return EMU_ERR_MEM;
    }

    /* Звук: в Handy gAudioEnabled по умолчанию FALSE — без TRUE Mikie::Update
       не обновляет аудио (эталон pico-retro lynx_host.cpp ставит его явно). */
    gAudioEnabled = TRUE;

    /* Дисплей: оригинальный 160x102, RGB565, без поворота (эталон). */
    g_sys->mMikie->DisplaySetAttributes(MIKIE_NO_ROTATE,
                                        MIKIE_PIXEL_FORMAT_16BPP_565,
                                        LYNX_W * 2,
                                        lynx_display_get, 0);

    uart_print("lynx: run\r\n");
    uart_flush();

    /* Запустить кольцо DMA; CPU1 через почту будет писать в него. */
    uart_print("lynx: i2s_start...\r\n");
    uart_flush();
    emu_audio_start();
    uart_print("lynx: loop enter\r\n");
    uart_flush();

    uint32_t dbg_frame = 0;
    for (;;) {
        if (emu_input_exit_hold(EMU_EXIT_HOLD_MS)) break;
        g_sys->SetButtonData(lynx_buttons_from_platform());

        if (dbg_frame < 3) {
            uart_print("lynx: frame ");
            uart_print_int((int)dbg_frame);
            uart_print(" cycle=");
            uart_print_hex(gSystemCycleCount);
            uart_print("\r\n");
            uart_flush();
            dbg_frame++;
        }

        /* Один кадр. Выходим по готовности кадра (callback) ИЛИ по счётчику цикла —
   как в эталоне pico-retro lynx_run_frame. Защита от «замка»: если ни то, ни
   другое не наступило за 300 мс реального времени — прерываем (меню снова
   живое), вместо вечного повисания. */
        g_lynx_frame_ready = 0;
        ULONG start = gSystemCycleCount;
        uint32_t fstart = micros();
        while ((ULONG)(gSystemCycleCount - start) < CYCLES_PER_FRAME) {
            g_sys->Update();
            if (g_lynx_frame_ready) break;
            if ((uint32_t)(micros() - fstart) > 300000u) {
                uart_print("lynx: frame lock, abort\r\n");
                goto out;
            }
        }

        /* Кадр -> экран. */
        lynx_blit_frame();

        /* Звук: стерео int16 в gAudioBuffer; pointer — сэмплы канала
         * (кадры = pointer/2). Куски на CPU1 -> i2s. */
        g_sys->FetchAudioSamples();
        if (gAudioBufferPointer >= 2) {
            if (g_last_audio_sent) {
                /* Рукопожатие: не перезаписывать gAudioBuffer, пока CPU1 ещё
                   читает прошлый кусок (без этого — data race CPU0/CPU1, треск).
                   Ждём, пока audio_done догонит количество отправленных кусков. */
                struct mailbox *mb = mb_get();
                while (mb->audio_done < g_last_audio_sent) {
                    __asm volatile("wfe" ::: "memory");
                    __asm volatile("dmb" ::: "memory");
                }
            }
            mb_send(MB_MSG_AUDIO_DATA,
                    (uint32_t)(uintptr_t)(const void *)gAudioBuffer,
                    gAudioBufferPointer >> 1);
            g_last_audio_sent++;
            gAudioBufferPointer = 0;
        }
    }

out:   /* выход по сторожу кадра или штатный выход (долгое START) */
    uart_print("lynx: exit\r\n");
    emu_audio_stop();
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
    /* Регистрируем ядро по реальному индексу системы "lynx" в systems[].
       Если системы нет в реестре — не регистрируем (захардкоженный индекс
       был бы привязкой к порядку списка и ломал эмулятор). */
    for (int i = 0; i < NUM_SYSTEMS; i++) {
        if (strcmp(systems[i].id, "lynx") == 0) {
            emulator_register_core((uint32_t)i, lynx_core_run);
            g_lynx_registered = 1;
            return;
        }
    }
    g_lynx_registered = 1;   /* системы нет — нечего регистрировать */
}

/* Регистрация ядра вызывается из C (retro_menu.c) — без манглинга. */
extern "C" void lynx_register(void)
{
    lynx_register_impl();
}