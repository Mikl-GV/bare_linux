#ifndef GIC_H
#define GIC_H

#include <stdint.h>

/* GIC-400 (H3): distributor 0x01C81000, CPU interface 0x01C82000. */

#define H3_GIC_DIST_BASE   0x01C81000u
#define H3_GIC_CPUIF_BASE  0x01C82000u

/* Инициализировать GIC: очистить все enable/pending/active, настроить
   приоритеты/таргеты на CPU0, включить группы 0 и 1 (IRQ). */
void gic_init(void);

/* Разрешить конкретное SPI-прерывание n (в группе 1) на CPU0. */
void gic_enable_irq(uint32_t n);

#endif /* GIC_H */