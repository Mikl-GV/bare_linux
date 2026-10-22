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
    /* Поднять все 4 ядра Cortex-A7 сразу при старте: вторичные (CPU1..CPU3)
       уходят в idle-цикл (заглушка под будущие задачи). */
    h3_smp_boot(0);
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

    /* WiFi отдельным слоем (wifi_layer_start) — вызывается из меню (Settings). */

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