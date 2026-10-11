/* jtagonly - minimalnaya proshivka TOLKO dlya vklyucheniya JTAG-pinov.
 *
 * Naznachenie: posle polnogo U-Boot (zagruzka s SD) piny PA0-3 ne v JTAG-
 * rezhime, iz-za chego OpenOCD ne mozhet vzyat CPU pod kontrol
 * (JTAG-DP STICKY ERROR, debug regions unpowered). Eta proshivka:
 *   - povtorno pereklyuchaet PA0-3 v Function 3 (JTAG TMS/TCK/TDO/TDI),
 *   - NE trogaet ni odin registr periferii (CCU/I2S/DMA/GIC/SMTA),
 *   - zatem zatsiklivaetsya, davaya OpenOCD ostanovit yadro i snyat kartu
 *     registrov v ChISTOM sostoyanii polnogo U-Boot.
 *
 * Edinstvennoe, chto menyaetsya - PIO_CFG0 (GPIO-multipleksor porta A).
 * Gruzitsya kak h3_bare.bin na 0x40000000 (U-Boot go / FEL+SPL).
 */

#include <stdint.h>

#define PIO_CFG0 (*(volatile uint32_t *)0x01C20800)

static void nop_delay(unsigned long n)
{
    volatile unsigned long i;
    for (i = 0; i < n; i++)
        __asm volatile("nop" ::: "memory");
}

void kmain(void)
{
    /* PA0..PA3 = Function 3 (JTAG). RMW: sokhranit PA4-7 (UART-TX). */
    PIO_CFG0 = (uint32_t)((PIO_CFG0 & 0xFFFF0000u) | 0x00003333u);
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    /* Dat vremya OpenOCD podklyuchitsya i ostanovit nas. */
    for (;;) {
        nop_delay(1000000);
        nop_delay(1000000);
    }
}