#include "h3_display.h"
#include "display_timing.h"
#include "uart.h"

/* Полный подъём дисплея H3 для 1024x600 XRGB8888.
   Последовательность (источник ref/pico-retro-new-gen h3_de2.c h3_hdmi.c). */

int h3_display_init(const struct display_timing *timing, uint32_t fbbase)
{
    if (h3_hdmi_probe() != 0) {
        uart_print("h3 display: hdmi probe fail\n");
        return -1;
    }

    de2_composer_init();
    de2_mode_set(timing, 32, fbbase);

    h3_hdmi_enable(timing, 32);

    uart_print("h3 display ok\n");
    return 0;
}