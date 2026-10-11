#ifndef SYS_H
#define SYS_H

#include <stdint.h>

/* Задержки и общие утилиты (на High-Speed Timer HSTMR ~97 МГц). */

void sys_timer_init(void);   /* включить такт HSTMR и запустить счётчик */
void udelay(uint32_t us);
uint32_t micros(void);       /* мкс с момента init (переполнение ~44 c) */
void udelay_short(uint32_t n); /* короткая задержка (до инициализации таймера) */

#endif /* SYS_H */