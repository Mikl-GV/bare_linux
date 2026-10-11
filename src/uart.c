#include "uart.h"
#include "h3.h"
#include "h3_ccu.h"
#include "ports.h"

/* UART0, 115200 8N1. U-Boot уже задал скорость; здесь лишь включаем
   такт/reset и PA4 = TX (F2). */

#define UART0_USR (*(volatile uint32_t *)(H3_UART0_BASE + 0x7C))
#define UART0_THR (*(volatile uint32_t *)(H3_UART0_BASE + 0x00))

void uart_init(void)
{
    set_pin_mode(PORTA, 4, 2);   /* PA4 = UART0 TX (F2) */

    H3_CCU_GATING3 |= (1u << 16);   /* такт UART0 */
    H3_CCU_RST4    |= (1u << 16);   /* снять reset UART0 */
    volatile uint32_t n = 1000;
    while (n--) {}
}

static int uart_tx_ready(void)
{
    return (UART0_USR & (1u << 1)) ? 1 : 0;
}

void uart_putc(char c)
{
    uint32_t t = 0;
    while (!uart_tx_ready()) {
        if (++t > 100000) return;
    }
    UART0_THR = (uint32_t)(unsigned char)c;
}

void uart_print(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_print_hex(uint32_t n)
{
    const char *hex = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) {
        buf[i] = hex[n & 0xF];
        n >>= 4;
    }
    buf[8] = 0;
    uart_print(buf);
}

void uart_print_int(int n)
{
    char buf[12];
    int i = 0;
    if (n == 0) { uart_print("0"); return; }
    if (n < 0) { uart_putc('-'); n = -n; }
    while (n > 0 && i < 11) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) uart_putc(buf[--i]);
}