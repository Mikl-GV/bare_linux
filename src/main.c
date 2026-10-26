#include "uart.h"
#include "ports.h"
#include "sys.h"
#include "i2s.h"
#include "h3_i2c_bb.h"
#include "pcf8574.h"
#include "display.h"
#include "secure.h"
#include "gic.h"
#include "irq.h"
#include "h3_sdc.h"
#include "wifi_layer.h"
#include "wifi_net.h"
#include "h3_smp.h"
#include "retro_menu.h"
#include "version.h"
#include "mailbox.h"
#include "mem.h"
#include "usb.h"

/* Единственный мигающий LED-индикатор — led_heartbeat() в retro_menu.c
 * (вызывается из udelay() в sys.c, чтобы работать во всех слоях). Дубль
 * тут убран: два независимых источника в разных единицах трансляции давали
 * рассинхронизированное состояние PL10. */

void kmain(void)
{
    /* UART поднимаем САМЫМ первым (до любой инициализации периферии), чтобы
       диагностические маркеры не терялись: uart_init обнуляет буфер вывода,
       поэтому вызовы uart_print ДО него молча стирались. UART0 уже настроен
       U-Boot'ом (115200 8N1) — здесь лишь включаем такт/reset и буфер. */
    uart_init();

    /* Убрать преграды TrustZone (U-Boot может блокировать DMA/I2S/CCU регистры). */
    secure_unlock();
    /* Таймер ДО SMP: h3_smp_boot/udelay работают через HSTMR (sys_timer_init). */
    sys_timer_init();
/* Такт ядра настраивает U-Boot (поднимает PLL_CPUX/напряжение сам);
       здесь — только диагностика текущего состояния тактов. */
    cpu_clk_init();
    /* Почта CPU0<->CPU1: инициализировать ДО старта CPU1 (область uncached). */
    mb_init();
    /* Куча + ROM-зона для эмуляторов: обязателен ДО первого запуска слоя
       (иначе mem_rom_capacity()==0 и emu_load_rom даёт EMU_ERR_MEM). */
    mem_init(0);
    /* Поднять вторичные ядра: CPU1 (слой звука через почту), CPU2/CPU3
       «потушены» (не поднимаем) — архитектура ядер проекта. */
    extern void audio_cpu1_main(void);
    h3_smp_boot(audio_cpu1_main);
    /* GIC: инициализировать distributor + CPU interface (без глобального
       сброса distributor — GIC мог быть поднят загрузчиком; новый U-Boot
       GIC не трогает, RMW безопасен). Глобально IRQ НЕ включаем до тех
       пор, пока не будет готов периодный источник: включение I-бита с
       неготовыми обработчиками валит CPU0. */
    gic_init();
    uart_print("IRQ: gic_init done\r\n");  uart_flush();
    /* Периодный тик Timer0: обработчик + линия 50 в GIC. */
    sys_tick_init();
    /* Глобально снимаем I-бит только теперь: источник и обработчик готовы. */
    irq_global_enable();
    gpio_init();   /* включить APB0 (порт L) — нужно для LED на PL10 */
    /* Проверка живого тика: за 20 мс должно набежать ~20 тиков (1 кГц). */
    udelay(20000);
    uart_print("IRQ: ticks=");
    uart_print_int((int)sys_tick_count());
    uart_print("\r\n");  uart_flush_all();
    uart_print("\r\n=== MultiTool H3 bare-metal ===\r\n");  uart_flush();
    uart_print("FW v" FW_VERSION_STR "\r\n");

    /* LED на PL10 — индикатор «я жив». */
    set_led(1);

    /* Дисплей поднимаем первым (до I2C/звука/WiFi — это занимает время и
       сдвигает вывод; экран должен гореть с самого старта). */
    uart_print("display init...\r\n");
    display_init();
    uart_print("display ok\r\n");

    /* Кнопки (I2C bitbang PG8=SDA, PG9=SCL) + PCF8574 @0x27 */
    uart_print("i2c init...\r\n");
    i2cbb_init(PORTG, 9, PORTG, 8);
    pcf8574_init();
    /* Диагностика кнопок: отвечает ли PCF8574@0x27 и что на линиях шины.
       pg — PG_DAT целиком (биты 8/9 = SDA/SCL: в idle должны быть 1). */
    {
        uint8_t raw = 0;
        int rc = pcf8574_probe(&raw);
        uart_print("btn: rc="); uart_print_int(rc);
        uart_print(" raw=");    uart_print_hex(raw);
        uart_print(" pg=");     uart_print_hex(*(volatile uint32_t *)0x01C208E8u);
        uart_print("\r\n");
    }
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

    /* USB host: полный подъём с нуля (такты/PHY/порты — свои, независимо
       от U-Boot); находит и поднимает флешку/клавиатуру. */
    uart_print("usb init...\r\n");
    int usb_devs = usb_hc_init();
    uart_print("usb ok (devs=");
    uart_print_int(usb_devs);
    uart_print(")\r\n");

    /* Главное ретро-меню: блокирует, внутри крутит wifi_layer_poll() и
       кнопки PCF8574. Это основной экран системы.
       Выход из меню (return по START) НЕ должен вешать систему в wfi —
       иначе экран застывает и ввод пропадает. Перезапускаем меню. */
    for (;;) {
        retro_menu_run();
    }
}