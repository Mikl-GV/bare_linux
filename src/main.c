#include "uart.h"
#include "ports.h"
#include "sys.h"
#include "i2s.h"
#include "h3_i2c_bb.h"
#include "pcf8574.h"
#include "display.h"
#include "secure.h"
#include "gic.h"
#include "h3_sdc.h"
#include "wifi_layer.h"
#include "version.h"

/* ================= Состояние ================= */

/* Экраны (состояния машины меню). */
enum {
    SCR_MAIN = 0,   /* главное меню */
    SCR_TONE,       /* подменю «Тест звука» (выбор частоты) */
    SCR_SETTINGS,   /* подменю «Настройки» */
    SCR_TRANSFER,   /* подменю «Transfer»: WiFi-слой (передача ROM по WiFi) */
    SCR_COUNT
};

/* Пункты главного меню. */
enum {
    MITEM_SOUND = 0,
    MITEM_TRANSFER,   /* вызов WiFi-слоя */
    MITEM_SETTINGS,
    MITEM_COUNT
};
static const char *main_items[MITEM_COUNT] = {
    "Sound test",
    "Transfer (WiFi)",
    "Settings"
};

/* Частоты теста в подменю. */
enum {
    TONE_440 = 0,
    TONE_697,
    TONE_1209,
    TONE_COUNT
};
static const int tone_freqs[TONE_COUNT] = { 440, 697, 1209 };
static const char *tone_labels[TONE_COUNT] = {
    "440 Hz",
    "697 Hz",
    "1209 Hz"
};

static int screen = SCR_MAIN;     /* текущий экран */
static int main_sel = MITEM_SOUND; /* выбранный пункт главного меню */
static int tone_sel = TONE_440;    /* выбранная частота в подменю */
static int testing = 0;            /* идёт ли тест звука */
static int transfer_sel = 0;       /* выбранная сеть в подменю Transfer */

static uint32_t prev_buttons = 0;

/* Индикатор «я жив» (heartbeat на LED): переключать каждые ~500 мс. */
static uint32_t led_last = 0;
static int led_state = 0;

/* Глобальный флаг: WiFi-режим активирован (устанавливается при успешном
   sdio_init слоем wifi_layer). Используется для значка на главном экране. */
extern int wifi_active;

static void led_heartbeat(void)
{
    uint32_t now = micros();
    if ((uint32_t)(now - led_last) >= 500000) {
        led_last = now;
        led_state = !led_state;
        set_led(led_state);
    }
}

/* Нарисовать маркер выбора (треугольник-стрелку) у пункта y. */
static void draw_marker(int x, int y)
{
    uint32_t rowpw = fb_pitch / 4;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col <= row; col++) {
            fb[((uint32_t)(y + row) * rowpw) + (uint32_t)(x + col)] = 0x00FFFFFF;
        }
    }
}

/* Вывести строку по центру в прямоугольнике x0..x1, y0..y1. */
static void draw_centered_text(const char *s, int x0, int x1, int y, uint32_t color)
{
    int len = 0;
    for (const char *p = s; *p; p++)
        if (*p >= 0x80) len += 2; else len += 1;
    int tw = len * 8;
    int cx = x0 + (x1 - x0) / 2 - tw / 2;
    fb_text(cx, y, s, color, 0x000D1A33);
}

/* ================= Рендер ================= */
static void draw_ui(void)
{
    fb_fill(0x000D1A33);

    /* Заголовок (всегда) */
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    fb_text(8, 24, "MultiTool H3  --  MENU", 0x00FFFFFF, 0x00336699);

    if (screen == SCR_MAIN) {
        /* --- Главное меню --- */
        const int x0 = 60, xw = FB_W - 120;
        const int y0 = 140;
        for (int i = 0; i < MITEM_COUNT; i++) {
            int y = y0 + i * 90;
            int is_sel = (i == main_sel);
            uint32_t box = is_sel ? 0x0044AA44 : 0x00102040;
            uint32_t fg  = is_sel ? 0x00FFFFFF : 0x0088CCBB;
            fb_rect(x0, y, x0 + xw, y + 70, box);
            draw_centered_text(main_items[i], x0, x0 + xw, y + 30, fg);
            if (is_sel) draw_marker(x0 + 10, y + 26);
        }
        /* Статусная строка */
        fb_text(60, y0 + MITEM_COUNT * 90 + 20,
                "A=select  B=back", 0x00A0C0D0, 0x000D1A33);
        /* Индикатор WiFi (справа вверху под шапкой) */
        if (wifi_active)
            fb_text(FB_W - 8 * 9 - 12, 12, "WIFI ON", 0x0000FF00, 0x00336699);
        else
            fb_text(FB_W - 8 * 10 - 12, 12, "WIFI OFF", 0x00888888, 0x00336699);
    }
    else if (screen == SCR_TONE) {
        /* --- Подменю «Тест звука» — выбор частоты --- */
        const int x0 = 60, xw = FB_W - 120;
        const int y0 = 140;
        for (int i = 0; i < TONE_COUNT; i++) {
            int y = y0 + i * 110;
            int is_sel = (i == tone_sel);
            uint32_t box = is_sel ? 0x0044AA44 : 0x00102040;
            uint32_t fg  = is_sel ? 0x00FFFFFF : 0x0088CCBB;
            fb_rect(x0, y, x0 + xw, y + 80, box);
            draw_centered_text(tone_labels[i], x0, x0 + xw, y + 36, fg);
            if (is_sel) draw_marker(x0 + 10, y + 32);
        }
        /* Статус */
        if (testing)
            fb_text(60, y0 + TONE_COUNT * 110 + 20,
                    "TESTING...  (B=stop)", 0x00FFD700, 0x000D1A33);
        else
            fb_text(60, y0 + TONE_COUNT * 110 + 20,
                    "A=play  B=back", 0x00A0C0D0, 0x000D1A33);
    }
    else if (screen == SCR_SETTINGS) {
        /* --- Подменю «Настройки» (заглушка) --- */
        fb_text(60, 200, "Settings (coming soon)", 0x00FFFFFF, 0x000D1A33);
        fb_text(60, 240, "B=back", 0x00A0C0D0, 0x000D1A33);
    }
    else if (screen == SCR_TRANSFER) {
        /* --- Подменю «Transfer»: отдельный WiFi-слой --- */
        fb_text(60, 120, "Transfer over WiFi", 0x00FFFFFF, 0x000D1A33);
        if (wifi_layer_started())
            fb_text(60, 148, "WiFi layer: running", 0x0000FF00, 0x000D1A33);
        else
            fb_text(60, 148, "WiFi layer: idle", 0x00888888, 0x000D1A33);
        /* Список найденных сетей (из скана). */
        fb_text(60, 180, "Networks:", 0x00A0C0D0, 0x000D1A33);
        int n = h3_wifi_scan_ap_count();
        if (n == 0) {
            fb_text(60, 204, "(scan not run yet — press A)", 0x00888888, 0x000D1A33);
        } else {
            if (n > 8) n = 8;   /* помещается на экран */
            if (transfer_sel >= n) transfer_sel = n - 1;
            for (int i = 0; i < n; i++) {
                struct h3_wifi_ap ap;
                if (h3_wifi_scan_ap_get(i, &ap) != 0) break;
                uint32_t col = (i == transfer_sel) ? 0x0000FF00 : 0x00FFFFFF;
                /* Маркер выбора. */
                fb_text(32, 204 + i * 22, (i == transfer_sel) ? ">" : " ", col, 0x000D1A33);
                fb_text(48, 204 + i * 22, ap.ssid, col, 0x000D1A33);
            }
        }
        if (wifi_layer_started() && n > 0)
            fb_text(60, 420, "A=connect  START=rescan  B=back", 0x00A0C0D0, 0x000D1A33);
        else
            fb_text(60, 420, "A=start WiFi  B=back", 0x00A0C0D0, 0x000D1A33);
    }

    /* Версия прошивки — нижний правый угол. */
    fb_text(FB_W - 8 * 11 - 8, FB_H - 8 - 4, "FW v" FW_VERSION_STR,
            0x00505070, 0x000D1A33);

    fb_clean();   /* вымыть D-cache, чтобы DE видел свежие линии */
}

/* ================= Кнопки ================= */
static void handle_buttons(uint32_t btns)
{
    uint32_t pressed = btns & ~prev_buttons;
    prev_buttons = btns;

    /* В главном меню */
    if (screen == SCR_MAIN) {
        if (pressed & BTN_UP) {
            main_sel = (main_sel + MITEM_COUNT - 1) % MITEM_COUNT;
            draw_ui();
        } else if (pressed & BTN_DOWN) {
            main_sel = (main_sel + 1) % MITEM_COUNT;
            draw_ui();
        } else if (pressed & BTN_A) {
            if (main_sel == MITEM_SOUND) screen = SCR_TONE;
            else if (main_sel == MITEM_TRANSFER) screen = SCR_TRANSFER;
            else if (main_sel == MITEM_SETTINGS) screen = SCR_SETTINGS;
            draw_ui();
        }
    }
    /* В подменю Тест звука */
    else if (screen == SCR_TONE) {
        if (pressed & BTN_UP) {
            tone_sel = (tone_sel + TONE_COUNT - 1) % TONE_COUNT;
            draw_ui();
        } else if (pressed & BTN_DOWN) {
            tone_sel = (tone_sel + 1) % TONE_COUNT;
            draw_ui();
        } else if (pressed & BTN_A) {
            /* Запустить тест выбранной частоты */
            uart_print("test freq=");
            uart_print_int(tone_freqs[tone_sel]);
            uart_print("\r\n");
            i2s_play_freq(tone_freqs[tone_sel]);
            testing = 1;
            draw_ui();
        } else if (pressed & BTN_B) {
            /* Остановить тест и вернуться в главное меню */
            i2s_stop();
            testing = 0;
            screen = SCR_MAIN;
            draw_ui();
        }
    }
    /* В подменю Настройки */
    else if (screen == SCR_SETTINGS) {
        if (pressed & BTN_B) {
            screen = SCR_MAIN;
            draw_ui();
        }
    }
    /* В подменю Transfer: WiFi-слой */
    else if (screen == SCR_TRANSFER) {
        int n = h3_wifi_scan_ap_count();
        if (pressed & BTN_UP) {
            if (wifi_layer_started() && n > 0) {
                if (transfer_sel <= 0) transfer_sel = n > 8 ? 7 : n - 1;
                else transfer_sel--;
            }
            draw_ui();
        } else if (pressed & BTN_DOWN) {
            if (wifi_layer_started() && n > 0) {
                int max = n > 8 ? 8 : n;
                transfer_sel = (transfer_sel + 1) % max;
            }
            draw_ui();
        } else if (pressed & BTN_A) {
            if (!wifi_layer_started()) {
                uart_print("transfer: wifi layer start...\r\n");
                wifi_layer_start();
            } else if (n > 0) {
                /* Подключиться к выбранной сети. */
                uart_print("transfer: connect sel=");
                uart_print_int(transfer_sel);
                uart_print("\r\n");
                wifi_layer_connect(transfer_sel);
            }
            draw_ui();
        } else if (pressed & BTN_START) {
            /* Rescan эфира: обновить список сетей без перепрошивки. */
            if (!wifi_layer_started()) {
                uart_print("transfer: wifi layer start...\r\n");
                wifi_layer_start();
            } else {
                uart_print("transfer: rescan...\r\n");
                wifi_layer_rescan();
            }
            draw_ui();
        } else if (pressed & BTN_B) {
            screen = SCR_MAIN;
            draw_ui();
        }
    }
}

void kmain(void)
{
    /* Убрать преграды TrustZone (U-Boot может блокировать DMA/I2S/CCU регистры). */
    secure_unlock();
    /* GIC: инициализировать distributor + CPU interface, включить обе группы
       (U-Boot закрывает GIC; без этого DMA/SPI прерывания не маршрутизируются). */
    gic_init();
    sys_timer_init();
    gpio_init();   /* включить APB0 (порт L) — нужно для LED на PL10 */
    uart_init();
    uart_print("\r\n=== MultiTool H3 bare-metal ===\r\n");
    uart_print("FW v" FW_VERSION_STR "\r\n");

    /* LED на PL10 — индикатор «я жив». */
    set_led(1);

    /* Дисплей поднимаем первым (до I2C/звука/WiFi — это занимает время и
       сдвигает вывод; экран должен гореть с самого старта). */
    uart_print("display init...\r\n");
    display_init();
    draw_ui();
    uart_print("display ok\r\n");

    /* PA0-3 = JTAG (RMW, не трогать PA4 UART-TX). */
    volatile uint32_t *pio_cfg0 = (volatile uint32_t *)0x01C20800;
    *pio_cfg0 = (uint32_t)((*pio_cfg0 & 0xFFFF0000u) | 0x00003333u);

    /* Кнопки (I2C bitbang PG8=SDA, PG9=SCL) + PCF8574 @0x27 */
    uart_print("i2c init...\r\n");
    i2cbb_init(PORTG, 9, PORTG, 8);
    pcf8574_init();
    uart_print("i2c ok\r\n");

    /* Звук */
    uart_print("i2s init...\r\n");
    if (i2s_init() != 0) {
        uart_print("I2S FAIL\r\n");
    } else {
        uart_print("i2s ok\r\n");
    }

    /* WiFi больше не инициализируется при старте — это отдельный слой,
       вызываемый из пункта меню "Transfer" (wifi_layer_start). */

    /* Главный цикл */
    draw_ui();
    for (;;) {
        led_heartbeat();         /* мигать PL10 — индикатор «я жив» */
        uint32_t btns = pcf8574_read();
        if (btns != prev_buttons) {
            handle_buttons(btns);
        }
        udelay(10000);   /* интервал опроса 10 мс */
    }
}