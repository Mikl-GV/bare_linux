#ifndef SYS_H
#define SYS_H

#include <stdint.h>

/* Задержки и общие утилиты (на High-Speed Timer HSTMR ~97 МГц). */

void sys_timer_init(void);   /* включить такт HSTMR и запустить счётчик */
void cpu_clk_init(void);     /* диагностика PLL_CPUX/CPU_AXI (частоту ставит U-Boot) */
void udelay(uint32_t us);
uint32_t micros(void);       /* мкс с момента init (переполнение ~44 c) */
void udelay_short(uint32_t n); /* короткая задержка (до инициализации таймера) */

/* Периодный системный тик от отдельного Timer0 (0x01C20C00, IRQ 50).
 * Не конфликтует с HSTMR (micros/udelay продолжают работать как прежде).
 * Частота: системный тик в Гц (по умолчанию SYS_TICK_HZ). Регистрирует
 * обработчик в irq.c и разрешает IRQ 50 в GIC. */
#define SYS_TICK_HZ   1000u

void sys_tick_init(void);          /* настроить Timer0 + зарегистрировать IRQ 50 */
uint32_t sys_tick_count(void);     /* число тиков с момента init */

#endif /* SYS_H */