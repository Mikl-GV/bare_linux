#include "gic.h"
#include "h3.h"

/* GIC-400 v2. Структура регистров — по референсу rpidmx512 gic.c
 * (обобщённый ARM GIC-400, как H3). По даташиту H3 GIC расположен на
 * H3_GIC_BASE=0x01C80000: distributor +0x1000 (0x01C81000),
 * CPU interface +0x2000 (0x01C82000). */

#define GICD_CTLR      (*(volatile uint32_t *)(H3_GIC_DIST_BASE + 0x000))
#define GICD_TYPER     (*(volatile uint32_t *)(H3_GIC_DIST_BASE + 0x004))
#define GICD_IGROUP    ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x080))
#define GICD_ISENABLE  ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x100))
#define GICD_ICENABLE  ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x180))
#define GICD_ICPEND    ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x280))
#define GICD_ICACTIVE  ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x380))
#define GICD_IPRIOR    ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x400))
#define GICD_ITARGET   ((volatile uint32_t *)(H3_GIC_DIST_BASE + 0x800))

#define GICC_CTLR      (*(volatile uint32_t *)(H3_GIC_CPUIF_BASE + 0x000))
#define GICC_PMR       (*(volatile uint32_t *)(H3_GIC_CPUIF_BASE + 0x004))

#define CTL_GRP0       (1u << 0)
#define CTL_GRP1       (1u << 1)

void gic_init(void)
{
    uint32_t i;
    uint32_t ints = ((GICD_TYPER & 0x1F) + 1) * 32;   /* это число IRQ */
    uint32_t regs = ints / 32;

    GICD_CTLR = 0;                 /* выключить distributor на время настройки */

    for (i = 0; i < regs; i++) {
        GICD_ICENABLE[i] = 0xFFFFFFFFu;   /* все прерывания отключить */
        GICD_ICPEND[i]   = 0xFFFFFFFFu;   /* снять pending */
        GICD_ICACTIVE[i] = 0xFFFFFFFFu;   /* снять active */
    }
    for (i = 0; i < regs; i++) {
        GICD_IPRIOR[i]   = 0;             /* приоритеты 0 */
        GICD_ITARGET[i]  = 0x01010101u;   /* на CPU0 */
    }
    for (i = 0; i < regs; i++)
        GICD_IGROUP[i]   = 0xFFFFFFFFu;   /* все в Group1 (IRQ, не FIQ) */

    GICD_CTLR = CTL_GRP0 | CTL_GRP1;      /* включить глобально обе группы */

    /* CPU interface. */
    GICC_PMR  = 0xFF;                     /* приоритетная маска — все */
    GICC_CTLR = CTL_GRP0 | CTL_GRP1;

    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

void gic_enable_irq(uint32_t n)
{
    uint32_t idx = n >> 5;
    uint32_t bit = 1u << (n & 0x1F);
    GICD_ISENABLE[idx] = bit;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}