#include "irq.h"
#include "gic.h"

/* Диспетчер GIC-прерываний для CPU0.
 *
 * Таблица обработчиков — статическая, по номерам линий GIC. H3: GIC-400,
 * GICD_TYPER.ITLinesNumber=4 -> (4+1)*32 = 160 линий (0..159).
 *
 * Незарегистрированный слот: обработчик не вызывается, но EOI делается
 * всегда — иначе GIC не снимет активное состояние и линия замолчит.
 *
 * Вызывается из boot.s в IRQ-режиме (переключения в SVC нет); IRQ на время
 * обработки запрещены аппаратно при входе в исключение.
 */

#define IRQ_MAX_LINES  160u

typedef struct {
    irq_handler_t h;
    void *param;
} irq_slot_t;

static irq_slot_t g_irq_tab[IRQ_MAX_LINES];

/* Зарегистрировать обработчик. Возвращает 0 при успехе, -1 вне диапазона. */
int irq_register(uint32_t irq, irq_handler_t h, void *param)
{
    if (irq >= IRQ_MAX_LINES) return -1;
    uint32_t save = irq_global_disable();
    g_irq_tab[irq].h = h;
    g_irq_tab[irq].param = param;
    irq_global_restore(save);
    return 0;
}

void irq_enable(uint32_t irq)  { gic_enable_irq(irq); }
void irq_disable(uint32_t irq) { gic_disable_irq(irq); }

/* Глобальные маски: CPSR.I. */
uint32_t irq_global_disable(void)
{
    uint32_t old;
    __asm volatile("mrs %0, cpsr" : "=r"(old));
    __asm volatile("cpsid i" ::: "memory");
    return (old >> 7) & 1u;            /* I-бит: 1 = были запрещены */
}

void irq_global_restore(uint32_t was)
{
    if (!was) __asm volatile("cpsie i" ::: "memory");
}

void irq_global_enable(void)
{
    __asm volatile("dsb" ::: "memory");
    __asm volatile("cpsie i" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

/* Диспетчер. Вызывается из boot.s в IRQ-режиме при IRQ. */
void irq_c_dispatch(void)
{
    uint32_t irq = gic_ack();           /* GICC_AIAR при DS=0 (Group1) */

    /* 1020..1023 — спуриозные/служебные значения; для них EOI не нужен. */
    if (irq < IRQ_MAX_LINES) {
        irq_slot_t *s = &g_irq_tab[irq];
        if (s->h) s->h(irq, s->param);
        gic_eoi(irq);                   /* подтвердить обработку в GIC */
    }
}
