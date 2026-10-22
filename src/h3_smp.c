#include "h3_smp.h"
#include "h3.h"
#include "uart.h"
#include "sys.h"

/* CPU local- и внешние регистры H3 (см. h3_smp.c в pico-retro, проверено
 * на H3 bare-metal). */

#define SUNXI_SRAM_A1_BASE  0x00000000u   /* трамплин для вторичных ядер */
#define R_CPUCFG_BASE       0x01F01C00u   /* CPU power/startup (R-домен) */
#define CPU_RST_CTRL(cpu)   (R_CPUCFG_BASE + 0x40u + (cpu) * 0x40u)
#define CPU_STAT(cpu)       (R_CPUCFG_BASE + 0x48u + (cpu) * 0x40u)

/* Почта ядра в SRAM A1 (0x20): первая инструкция ядра пишет magic —
 * SRAM не кэшируется, видимость мгновенная, MMU/кэши ядер не важны. */
#define CPU_MAIL_ADDR       0x00000020u
#define CPU_MAGIC           0x13579BDFu

/* PSCI 0.2 SMC32: CPU_ON. */
#define PSCI_CPU_ON         0x84000003u

static void (*volatile g_sec_entry)(void);

static uint32_t h3_read_scr(void)
{
    uint32_t v;
    __asm volatile("mrc p15, 0, %0, c1, c1, 0" : "=r"(v));
    return v;
}

static uint32_t h3_psci_cpu_on(uint32_t cpu, uint32_t entry)
{
    register uint32_t r0 __asm("r0") = PSCI_CPU_ON;
    register uint32_t r1 __asm("r1") = cpu;
    register uint32_t r2 __asm("r2") = entry;
    register uint32_t r3 __asm("r3") = 0;
    __asm volatile(
        "smc #0\n\t"
        : "+r"(r0)
        : "r"(r1), "r"(r2), "r"(r3)
        : "memory");
    return r0;
}

/* Трамплин в SRAM A1: ядро после сброса стартует отсюда.
 *   ldr sp, [pc, #4]   ; SP = стek вторичного ядра
 *   ldr r0, [pc, #4]   ; r0 = entry (g_sec_entry)
 *   bx  r0
 * Стек каждого ядра — своя область (SP задаётся трамплином). */
static int h3_secondary_start(int cpu, void (*entry)(void))
{
    volatile uint32_t *s = (volatile uint32_t *)SUNXI_SRAM_A1_BASE;
    const uint32_t tramp[5] = {
        0xE59FD004u,               /* ldr sp, [pc, #4] */
        0xE59F0004u,               /* ldr r0, [pc, #4] */
        0xE12FFF10u,               /* bx  r0 */
        (0x5FE00000u - (uint32_t)(cpu - 1) * 0x1000u),  /* стек ядра */
        (uint32_t)entry,
    };
    for (int i = 0; i < 5; i++) s[i] = tramp[i];
    /* Clean+invalidate SRAM (могли быть dirty линии у нас). */
    {
        uint32_t a = SUNXI_SRAM_A1_BASE & ~0x1Fu;
        uint32_t e = a + 64;
        for (; a < e; a += 32)
            __asm volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a));
    }
    __asm volatile("dsb" ::: "memory");

    (void)cpu;
    volatile uint32_t *mail = (volatile uint32_t *)CPU_MAIL_ADDR;
    __asm volatile("str %0, [%1]" :: "r"(0u), "r"(mail) : "memory");
    __asm volatile("dsb" ::: "memory");

    /* Release: RST_CTRL 0 (hold) -> 3 (running). */
    *(volatile uint32_t *)CPU_RST_CTRL(cpu) = 0u;
    __asm volatile("dsb" ::: "memory");
    udelay(2000);
    *(volatile uint32_t *)CPU_RST_CTRL(cpu) = 3u;
    __asm volatile("dsb" ::: "memory");

    for (uint32_t t = 0; t < 200; t++) {
        udelay(100);
        uint32_t m;
        __asm volatile("ldr %0, [%1]" : "=r"(m) : "r"(mail) : "memory");
        if (m == CPU_MAGIC) {
            uart_print("smp: CPU");
            uart_print_int(cpu);
            uart_print(" alive\r\n");
            return 1;
        }
    }
    uart_print("smp: CPU");
    uart_print_int(cpu);
    uart_print(" not alive\r\n");
    return 0;
}

/* Общий цикл вторичного ядра. */
static void cpuN_loop(void)
{
    for (;;) {
        __asm volatile("wfi" ::: "memory");
    }
}

int h3_smp_start_secondaries(void (*entry)(void))
{
    int started = 1;   /* ядро-0 (мы) уже работает */
    uint32_t scr = h3_read_scr();
    uart_print("smp: start secondaries (scr=");
    uart_print_hex(scr);
    uart_print(")\r\n");

    for (int cpu = 1; cpu <= 3; cpu++) {
        if (scr & 1u) {
            /* Non-secure: R_CPUCFG закрыт — будим через PSCI. */
            uint32_t r = h3_psci_cpu_on((uint32_t)cpu, (uint32_t)entry);
            uart_print("smp: CPU");
            uart_print_int(cpu);
            uart_print(" PSCI ret=0x");
            uart_print_hex(r);
            uart_print("\r\n");
            if (r == 0) started++;
        } else {
            if (h3_secondary_start(cpu, entry)) started++;
        }
    }
    uart_print("smp: secondaries up: ");
    uart_print_int(started - 1);
    uart_print("/3\r\n");
    return started;
}

/* Публичная обёртка: глобальный entry + запуск вторичных. Вызывается
 * один раз в kmain сразу после старта (до/вместо прочих инициализаций). */
void h3_smp_boot(void (*entry)(void))
{
    g_sec_entry = entry ? entry : cpuN_loop;
    h3_smp_start_secondaries(g_sec_entry);
}