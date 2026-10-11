#ifndef UART_H
#define UART_H

#include <stdint.h>

/* UART0 — консоль (PA4=TX, PA5=RX, 115200 8N1). */

void uart_init(void);
void uart_putc(char c);
void uart_print(const char *s);
void uart_print_hex(uint32_t n);
void uart_print_int(int n);

#endif /* UART_H */