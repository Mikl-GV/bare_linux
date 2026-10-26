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
#define GICC_IAR       (*(volatile uint32_t *)(H3_GIC_CPUIF_BASE + 0x00C))
#define GICC_EOIR      (*(volatile uint32_t *)(H3_GIC_CPUIF_BASE + 0x010))
/* Алиасные регистры для Group1 при включённой защите (GICD_CTLR.DS=0):
   secure-мир подтверждает Group1 только через AIAR/AEOIR. Чтение обычного
   GICC_IAR в этом режиме даёт 1022 («pending, но не Group0»). */
#define GICC_AIAR      (*(volatile uint32_t *)(H3_GIC_CPUIF_BASE + 0x020))
#define GICC_AEOIR     (*(volatile uint32_t *)(H3_GIC_CPUIF_BASE + 0x024))

#define CTL_GRP0       (1u << 0)
#define CTL_GRP1       (1u << 1)
#define CTL_DS         (1u << 6)   /* 1 = единое состояние безопасности */

/* DS=1 — защита отключена, работают обычные IAR/EOIR.
   DS=0 — все наши линии в Group1, подтверждаем через AIAR/AEOIR. */
static int gic_security_off(void)
{
    return (GICD_CTLR & CTL_DS) != 0;
}

void gic_init(void)
{
    uint32_t i;
    uint32_t ints   = ((GICD_TYPER & 0x1F) + 1) * 32;   /* число линий (H3: 160) */
    uint32_t regs32 = ints / 32;   /* enable/pending/active/group: 1 бит на линию */
    uint32_t regs8  = ints / 4;    /* priority/target: 1 байт на линию */

    /* Глобальный сброс distributor (GICD_CTLR = 0) НЕ делаем: GIC мог быть
       поднят загрузчиком или уже включён. Вместо сброса точечно отключаем ВСЕ
       источники (ICENABLE), снимаем pending/active, а в конце ДО-включаем
       группы через RMW — не трогая уже включённые биты. (Новый U-Boot GIC
       не инициализирует — конфигурации GICV2 в нём нет, — но RMW безопасен
       и в этом случае: регистры в reset-дефолте, биты групп просто станут 1.) */
    for (i = 0; i < regs32; i++) {
        GICD_ICENABLE[i] = 0xFFFFFFFFu;   /* все прерывания отключить */
        GICD_ICPEND[i]   = 0xFFFFFFFFu;   /* снять pending */
        GICD_ICACTIVE[i] = 0xFFFFFFFFu;   /* снять active */
    }
    for (i = 0; i < regs8; i++) {
        GICD_IPRIOR[i]   = 0;             /* приоритеты 0 */
        GICD_ITARGET[i]  = 0x01010101u;   /* на CPU0 (по 4 линии в слове) */
    }
    for (i = 0; i < regs32; i++)
        GICD_IGROUP[i]   = 0xFFFFFFFFu;   /* все в Group1 (IRQ, не FIQ) */

    /* До-включить группы (не сбрасывая distributor). */
    GICD_CTLR |= CTL_GRP0 | CTL_GRP1;

    /* CPU interface. */
    GICC_PMR  = 0xFF;                     /* приоритетная маска — все */
    GICC_CTLR |= CTL_GRP0 | CTL_GRP1;

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

void gic_disable_irq(uint32_t n)
{
    uint32_t idx = n >> 5;
    uint32_t bit = 1u << (n & 0x1F);
    GICD_ICENABLE[idx] = bit;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

/* Прочитать номер активного IRQ. 1023 = спуриозный (не обрабатываем).
   При DS=0 читаем алиас Group1 (AIAR), иначе обычный IAR. */
uint32_t gic_ack(void)
{
    __asm volatile("dsb" ::: "memory");
    uint32_t iar = gic_security_off() ? GICC_IAR : GICC_AIAR;
    return iar & 0x3FFu;
}

void gic_eoi(uint32_t irq)
{
    if (gic_security_off()) GICC_EOIR  = irq;
    else                    GICC_AEOIR = irq;
    __asm volatile("dsb" ::: "memory");
}