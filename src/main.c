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
#include "version.h"

/* ================= Состояние ================= */

/* Экраны (состояния машины меню). */
enum {
    SCR_MAIN = 0,   /* главное меню */
    SCR_TONE,       /* подменю «Тест звука» (выбор частоты) */
    SCR_SETTINGS,   /* подменю «Настройки» */
    SCR_COUNT
};

/* Пункты главного меню. */
enum {
    MITEM_SOUND = 0,
    MITEM_SETTINGS,
    MITEM_COUNT
};
static const char *main_items[MITEM_COUNT] = {
    "Sound test",
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

static uint32_t prev_buttons = 0;

/* Индикатор «я жив» (heartbeat на LED): переключать каждые ~500 мс. */
static uint32_t led_last = 0;
static int led_state = 0;

/* Глобальный флаг: WiFi-режим активирован (устанавливается при успешном
   sdio_init). Используется для значка на главном экране. */
int wifi_active = 0;

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

    /* WiFi: включить чип (WIFI_EN=PL7), инициализировать SDC1 и SDIO-карту.
       При успехе выставить wifi_active=1 (для индикатора) и прочитать CCID. */
    uart_print("wifi init...\r\n");
    uart_print("  pl_before=0x"); uart_print_hex(*(volatile uint32_t *)0x01F02C10);
    uart_print("\r\n");
    h3_sdc1_init();
    uart_print("  pl_after=0x"); uart_print_hex(*(volatile uint32_t *)0x01F02C10);
    uart_print("\r\n");
    /* Диагностика после инициализации контроллера. */
    uart_print("  gctl=0x"); uart_print_hex(*(volatile uint32_t *)0x01C10000);
    uart_print(" ckcr=0x");  uart_print_hex(*(volatile uint32_t *)0x01C10004);
    uart_print(" star=0x");  uart_print_hex(*(volatile uint32_t *)0x01C1003C);
    uart_print(" rst0=0x");  uart_print_hex(*(volatile uint32_t *)0x01C202C0);
    uart_print(" pg_cfg0=0x");uart_print_hex(*(volatile uint32_t *)0x01C208D8);
    uart_print(" pl_cfg=0x");uart_print_hex(*(volatile uint32_t *)0x01F02C00);
    uart_print(" pl_dat=0x");uart_print_hex(*(volatile uint32_t *)0x01F02C10);
    uart_print("\r\n");
    /* Ручной probe CMD5 (диагностика контроллера). */
    {
        volatile uint32_t *SDC = (volatile uint32_t *)0x01C10000;
        SDC[0x38/4] = 0xFFFFFFFF;               /* очистить RISR */
        SDC[0x1C/4] = 0x00E00000;               /* ARG: 3.0-3.6V */
        SDC[0x18/4] = (1u<<31) | 5u | (1u<<6);  /* START|CMD5|RESP_RCV */
        for (int w = 0; w < 1000000; w++)
            if (SDC[0x38/4] & (1u<<2)) break;   /* CMD_DONE */
        uart_print("  cmdr=0x"); uart_print_hex(SDC[0x18/4]);
        uart_print(" risr=0x");  uart_print_hex(SDC[0x38/4]);
        uart_print(" resp0=0x"); uart_print_hex(SDC[0x20/4]);
        uart_print("\r\n");
    }
    {
        int rc = h3_sdio_init_chip();
        uart_print("  sdio_init rc=");
        uart_print_int(rc);
        uart_print("\r\n");
        if (rc == 0) {
            wifi_active = 1;
            uint8_t ccid0 = 0, ccid1 = 0;
            h3_sdc_cmd52_read(0, 0x00, &ccid0);   /* CCCR/SDIO revision */
            h3_sdc_cmd52_read(0, 0x01, &ccid1);   /* SDIO function info */
            uart_print("  ccid0=0x");
            uart_print_hex(ccid0);
            uart_print(" ccid1=0x");
            uart_print_hex(ccid1);
            uart_print("\r\n");
        }
    }
    uart_print("wifi ok\r\n");

    /* Дисплей: переиспользуем фреймбуфер U-Boot */
    uart_print("display init...\r\n");
    display_init();
    draw_ui();
    uart_print("display ok\r\n");

    /* Главный цикл */
    for (;;) {
        led_heartbeat();         /* мигать PL10 — индикатор «я жив» */
        uint32_t btns = pcf8574_read();
        if (btns != prev_buttons) {
            handle_buttons(btns);
        }
        udelay(10000);   /* интервал опроса 10 мс */
    }
}