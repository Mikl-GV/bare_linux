#include "sys.h"
#include "h3.h"
#include "h3_ccu.h"
#include "uart.h"

/* Мигание LED (индикатор жизни процессора) — вызывается из udelay, чтобы
   работать во ВСЕХ слоях (меню, WiFi join, звук, SD), а не только в меню.
   Реализация в retro_menu.c/ports.c; здесь extern без включения ports.h. */
extern void led_heartbeat(void);

/* High-Speed Timer (HSTMR) на 0x01C60000, 64-bit счётчик вниз.
   Реально работает на ~96.8 МГц (берём 97). Читаем ТОЛЬКО CURNT_LO - по
   результатам стенда CURNT_HI на этой плате нестабилен. */

#define HSTMR_IRQ_EN   (*(volatile uint32_t *)(H3_HSTMR_BASE + 0x00))
#define HSTMR_CTRL     (*(volatile uint32_t *)(H3_HSTMR_BASE + 0x10))
#define HSTMR_INTV_LO  (*(volatile uint32_t *)(H3_HSTMR_BASE + 0x14))
#define HSTMR_INTV_HI  (*(volatile uint32_t *)(H3_HSTMR_BASE + 0x18))
#define HSTMR_CURNT_LO (*(volatile uint32_t *)(H3_HSTMR_BASE + 0x1C))

#define HSTMR_MHZ 97u

void sys_timer_init(void)
{
    /* Полный сброс HSTMR: RST0 bit19 в 0, затем в 1 (как HARDWARE-REF). */
    H3_CCU_RST0    &= ~(1u << 19);
    __asm volatile("dsb" ::: "memory");
    udelay_short(10);
    H3_CCU_RST0    |= (1u << 19);
    H3_CCU_GATING0 |= (1u << 19);   /* HSTMR gate */
    __asm volatile("dsb" ::: "memory");

    HSTMR_IRQ_EN    = 0;
    HSTMR_CTRL      = 0;
    HSTMR_INTV_LO   = 0xFFFFFFFFu;
    HSTMR_INTV_HI   = 0x00FFFFFFu;
    HSTMR_CTRL     |= (1u << 4);    /* выбор такта */
    HSTMR_CTRL     |= (1u << 1);    /* RELOAD */
    HSTMR_CTRL     |= (1u << 0);    /* START */
}

/* Короткая задержка до инициализации таймера (не использует micros). */
void udelay_short(uint32_t n)
{
    volatile uint32_t i;
    for (i = 0; i < n; i++) { __asm volatile("nop"); }
}

uint32_t micros(void)
{
    return (~HSTMR_CURNT_LO) / HSTMR_MHZ;
}

void udelay(uint32_t us)
{
    /* Работаем напрямую со счётчиком вниз, без преобразования в мкс,
       чтобы избежать зависания при 32-bit переполнении (~44 с). */
    uint32_t cur = HSTMR_CURNT_LO;          /* счётчик вниз */
    uint32_t ticks = us * HSTMR_MHZ;
    /* W.278: пока ждём — фоново сливаем UART-буфер (машино-быстрая запись
       уже не тормозит протокол; реальная передача идёт в паузах). */
    uint32_t next_flush = HSTMR_CURNT_LO;
    /* 50 мкс в тиках HSTMR. Раньше было HSTMR_MHZ/20 = 4 тика (~41 нс)
       из-за целочисленного деления — uart_flush() вызывался на каждой
       итерации цикла задержки. */
    uint32_t flush_gap = HSTMR_MHZ * 50u / 1000000u;
    uint32_t next_led  = HSTMR_CURNT_LO;
    uint32_t led_gap   = HSTMR_MHZ * 250000u / 1000000u;   /* ~250 мс */
    while ((uint32_t)(cur - HSTMR_CURNT_LO) < ticks) {
        if ((uint32_t)(next_flush - HSTMR_CURNT_LO) >= flush_gap) {
            next_flush = HSTMR_CURNT_LO;
            uart_flush();
        }
        if ((uint32_t)(next_led - HSTMR_CURNT_LO) >= led_gap) {
            next_led = HSTMR_CURNT_LO;
            led_heartbeat();   /* мигание PL10 в любом слое */
        }
    }
    uart_flush();
}

/* Free-standing memset (may be called by the toolchain). */
void *memset(void *dst, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}