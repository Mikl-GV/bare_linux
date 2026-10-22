#ifndef H3_SMP_H
#define H3_SMP_H

#include <stdint.h>

/* Запуск вторичных ядер Cortex-A7 (CPU1..CPU3) Allwinner H3.
 *
 * Способ (проверен на H3 bare-metal, см. pico-retro h3_smp.c):
 *   - трамплин пишется в SRAM A1 (0x00000000) — ядро после сброса стартует
 *     оттуда; содержательно: SP + адрес entry функции ядра;
 *   - release через R_CPUCFG (0x01F01C00): RST_CTRL(cpu) 0 (hold) -> 3 (release);
 *   - ядро ставит magic в SRAM-почту (0x20) первой инструкцией, core0
 *     опрашивает почту (SRAM вне кэшей — видимость мгновенная);
 *   - в non-secure мире (PSCI от U-Boot / SCR bit0=1) регистры R_CPUCFG
 *     закрыты — тогда CPU_ON через SMC (fid=0x84000003).
 *
 * Каждое ядро выполняет cpuN_loop() — IDLE-цикл, который сейчас просто
 * крутится (заглушка для будущих задач: эмуляторы, потоки). */

/* Поднять ядра CPU1..CPU3. entry — функция-цикл для каждого ядра.
 * Возвращает число успешно поднятых ядер (1 = только ядро-0, если пуск
 * вторичных не удался). */
int h3_smp_start_secondaries(void (*entry)(void));

/* Вызов из kmain: поднять вторичные ядра с entry (или со встроенным
 * idle-циклом, если entry == NULL). */
void h3_smp_boot(void (*entry)(void));

#endif /* H3_SMP_H */