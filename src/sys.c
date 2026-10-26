#include "sys.h"
#include "h3.h"
#include "h3_ccu.h"
#include "uart.h"
#include "irq.h"
#include "gic.h"
#include "ports.h"

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

/* Замер частоты ядра по счётчику тактов CPU (PMCCNTR, Cortex-A7): интервал
 * меряем одновременно HSTMR-микросекундами и тактами — результат не зависит
 * от промахов кэша инструкций и от того, что цикл исполняется из DRAM. */
static uint32_t cpu_mhz_measure(void)
{
    uint32_t pmcr, c0, c1, lo, t0, t1;

    __asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    pmcr |= 1u;                              /* E: включить счётчики */
    __asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(pmcr));
    __asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(0x80000000u)); /* C: пуск CCNT */
    __asm volatile("isb");

    __asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c0));
    t0 = micros();
    lo = micros();
    while ((uint32_t)(micros() - lo) < 2000u) { }   /* ~2 мс */
    t1 = micros();
    __asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c1));

    if (t1 == t0) return 0;
    return (c1 - c0) / (t1 - t0);            /* такты / мкс = МГц */
}

/* Тактирование ядра настраивает загрузчик (U-Boot), прошивка его НЕ трогает:
 * только диагностика текущего состояния (PLL_CPUX/CPU_AXI и замер частоты). */
void cpu_clk_init(void)
{
    /* Тактирование ядра настраивает ЗАГРУЗЧИК (U-Boot), а не прошивка:
       здесь только диагностика текущего состояния PLL/делителей. Раньше
       код сам поднимал частоту (PMIC SY8106A + PLL_CPUX), но на Orange Pi
       Lite PMIC-шина TWI на PA11/PA12 не разведена (rc=-1), а вольтаж
       выставлять нечем — безопаснее не трогать такты. */
    uart_print("clk: pllx="); uart_print_hex(H3_CCU_PLL_CPUX);
    uart_print(" axi=");      uart_print_hex(H3_CCU_CPU_AXI);
    uart_print(" cpu=");      uart_print_int((int)cpu_mhz_measure());
    uart_print("MHz (u-boot owns clock)\r\n");
    uart_flush();
}

/* Короткая задержка ДО инициализации таймера (HSTMR ещё не считает).
 * Намеренно NOP-цикл, а не micros()/udelay: на 97 МГц ~4 такта на итерацию
 * (каждая nop + декремент/сравнение), n=10 даёт порядка десятков нс — этого
 * достаточно как пауза после CCU-reset. Не претендует на точные микросекунды. */
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

/* ===== Периодный системный тик (Timer0, 0x01C20C00, IRQ 50) =====
 *
 * HSTMR занят под micros()/udelay() как free-running счётчик, поэтому
 * периодный тик делаем на ОТДЕЛЬНОМ штатном таймере Timer0. Он не имеет
 * собственного clock-gate (работает всегда от APB/OSC24M — в DTS таймера
 * даже нет clocks/resets) и служит как раз для "scheduler interrupt"
 * (даташит 4.6.1). Частота тика SYS_TICK_HZ.
 *
 * Регистры (даташит 4.6.4):
 *   TMR_IRQ_EN_REG  0x00  — bit0 = Timer0 IRQ enable
 *   TMR_IRQ_STA_REG 0x04  — bit0 = pending (write-1-to-clear)
 *   TMR0_CTRL_REG   0x10  — bit0=EN, bit1=RELOAD, bits[3:2]=CLK_SRC(01=OSC24M),
 *                           bits[6:4]=PRES(000=/1), bit7=MODE(0=continuous)
 *   TMR0_INTV_VALUE 0x14  — интервал (24-bit)
 *   TMR0_CUR_VALUE  0x18  — текущее значение (вниз)
 */

#define TIMER0_IRQ_EN_REG   (*(volatile uint32_t *)(0x01C20C00u + 0x00))
#define TIMER0_IRQ_STA_REG  (*(volatile uint32_t *)(0x01C20C00u + 0x04))
#define TIMER0_CTRL_REG     (*(volatile uint32_t *)(0x01C20C00u + 0x10))
#define TIMER0_INTV_REG     (*(volatile uint32_t *)(0x01C20C00u + 0x14))

#define TMR0_IRQ_EN         (1u << 0)
#define TMR0_IRQ_PEND       (1u << 0)
/* CTRL биты (даташит 4.6.4.3). */
#define TMR0_CTRL_EN        (1u << 0)
#define TMR0_CTRL_RELOAD    (1u << 1)
#define TMR0_CTRL_CLK_OSC24M (1u << 2)   /* bits[3:2] = 01 */
#define TMR0_CTRL_PRES_16   (4u << 4)    /* bits[6:4] = 100 -> /16 (1.5 МГц) */
#define TMR0_CTRL_CONT      (0u << 7)    /* continuous */
/* IRQ 50 = Timer0 (даташит 4.12.1). */
#define TIMER0_IRQ_NUM      50u

/* Такт Timer0 после предделителя: 24 МГц / 16 = 1.5 МГц.
   Интервал ОБЯЗАН влезать в 24-битный счётчик (<= 0xFFFFFF = 16 777 215):
   1500000 при 1 Гц и 1500 при 1 кГц — влезает; без предделителя 24 МГц
   при 1 Гц дал бы 24e6 (0x16E3600, 25 бит) и регистр обрезался бы. */
#define TIMER0_TICK_CLK     1500000u   /* 24e6 / 16 */

static volatile uint32_t g_tick_count;

static void timer0_tick_hook(uint32_t irq, void *param)
{
    (void)irq;
    (void)param;
    TIMER0_IRQ_STA_REG = TMR0_IRQ_PEND;   /* W1C — снять pending */
    g_tick_count++;
}

void sys_tick_init(void)
{
    /* Стоп + интервал + разрешение прерывания таймера ДО старта, чтобы
       первое срабатывание не потерялось. */
    TIMER0_CTRL_REG = 0;
    TIMER0_INTV_REG = TIMER0_TICK_CLK / SYS_TICK_HZ;
    TIMER0_IRQ_STA_REG = TMR0_IRQ_PEND;
    TIMER0_IRQ_EN_REG |= TMR0_IRQ_EN;
    __asm volatile("dsb" ::: "memory");

    irq_register(TIMER0_IRQ_NUM, timer0_tick_hook, 0);
    irq_enable(TIMER0_IRQ_NUM);

    g_tick_count = 0;

    /* Старт последним: первый тик — ровно через период (INTV / 1.5 МГц). */
    TIMER0_CTRL_REG = TMR0_CTRL_CLK_OSC24M | TMR0_CTRL_PRES_16 | TMR0_CTRL_CONT
                    | TMR0_CTRL_RELOAD | TMR0_CTRL_EN;
    __asm volatile("dsb" ::: "memory");
}

uint32_t sys_tick_count(void)
{
    return g_tick_count;
}

/* Free-standing memset (may be called by the toolchain). */
void *memset(void *dst, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}