#ifndef GIC_H
#define GIC_H

#include <stdint.h>

/* GIC-400 (H3): distributor 0x01C81000, CPU interface 0x01C82000. */

#define H3_GIC_DIST_BASE   0x01C81000u
#define H3_GIC_CPUIF_BASE  0x01C82000u

/* Инициализировать GIC: очистить все enable/pending/active, настроить
   приоритеты/таргеты на CPU0, включить группы 0 и 1 (IRQ). */
void gic_init(void);

/* Разрешить/запретить конкретное SPI-прерывание n (в группе 1) на CPU0. */
void gic_enable_irq(uint32_t n);
void gic_disable_irq(uint32_t n);

/* Прочитать текущий IRQ из CPU-интерфейса (GICC_IAR) и подтвердить
   окончание обработки (GICC_EOIR). Вызываются диспетчером (irq.c). */
uint32_t gic_ack(void);
void     gic_eoi(uint32_t irq);

#endif /* GIC_H */