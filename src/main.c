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
#include "wifi_net.h"
#include "h3_smp.h"
#include "retro_menu.h"
#include "version.h"
#include "mailbox.h"
#include "usb.h"

/* Индикатор «я жив» (heartbeat на LED): переключать каждые ~500 мс. */
static uint32_t led_last = 0;
static int led_state = 0;

static void led_heartbeat(void)
{
    uint32_t now = micros();
    if ((uint32_t)(now - led_last) >= 500000) {
        led_last = now;
        led_state = !led_state;
        set_led(led_state);
    }
}

void kmain(void)
{
    /* Убрать преграды TrustZone (U-Boot может блокировать DMA/I2S/CCU регистры). */
    secure_unlock();
    /* Таймер ДО SMP: h3_smp_boot/udelay работают через HSTMR (sys_timer_init). */
    sys_timer_init();
    /* Почта CPU0<->CPU1: инициализировать ДО старта CPU1 (область uncached). */
    mb_init();
    /* Поднять вторичные ядра: CPU1 (слой звука через почту), CPU2/CPU3
       «потушены» (не поднимаем) — архитектура ядер проекта. */
    extern void audio_cpu1_main(void);
    h3_smp_boot(audio_cpu1_main);
    /* GIC: инициализировать distributor + CPU interface, включить обе группы
       (U-Boot закрывает GIC; без этого DMA/SPI прерывания не маршрутизируются). */
    gic_init();
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
    uart_print("display ok\r\n");

    /* PA0-3 = JTAG (RMW, не трогать PA4 UART-TX). */
    volatile uint32_t *pio_cfg0 = (volatile uint32_t *)0x01C20800;
    *pio_cfg0 = (uint32_t)((*pio_cfg0 & 0xFFFF0000u) | 0x00003333u);

    /* Кнопки (I2C bitbang PG8=SDA, PG9=SCL) + PCF8574 @0x27 */
    uart_print("i2c init...\r\n");
    i2cbb_init(PORTG, 9, PORTG, 8);
    pcf8574_init();
    /* Sega-джойстик (PCF8574@0x20, та же шина PG8/PG9): драйвер 1 к 1 из
       pico-retro-new-gen, инициализация обязательна (без неё скан не стартует). */
    extern int sega_pad_init(void);
    sega_pad_init();
    uart_print("i2c ok\r\n");

    /* Звук */
    uart_print("i2s init...\r\n");
    if (i2s_init() != 0) {
        uart_print("I2S FAIL\r\n");
    } else {
        uart_print("i2s ok\r\n");
    }

    /* WiFi отдельным слоем (wifi_layer_start) — вызывается из меню (Settings). */

    /* USB host: полный подъём с нуля (FEL+SPL — наследовать нечего).
       Поднимает обе шины, ищет устройства; флешка ожидается на USB0. */
    uart_print("usb init...\r\n");
    int usb_devs = usb_hc_init();
    uart_print("usb ok (devs=");
    uart_print_int(usb_devs);
    uart_print(")\r\n");

    /* Главное ретро-меню: блокирует, внутри крутит wifi_layer_poll() и
       кнопки PCF8574. Это основной экран системы. */
    retro_menu_run();

    /* Сюда не возвращаемся; страховка от падения. */
    for (;;) {
        led_heartbeat();
        wifi_layer_poll();
        __asm volatile("wfi" ::: "memory");
    }
}