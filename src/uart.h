#ifndef UART_H
#define UART_H

#include <stdint.h>

/* UART0 — консоль (PA4=TX, PA5=RX, 115200 8N1). */

void uart_init(void);
void uart_putc(char c);
void uart_putc_buffered(char c);
void uart_print(const char *s);
void uart_print_hex(uint32_t n);
void uart_print_int(int n);
/* Принудительный слив буфера UART в линию (не дожидаясь udelay). */
void uart_flush(void);
/* Полный слив: ждёт, пока кольцевой буфер опустеет (FIFO отдаётся порциями).
   Для диагностики на месте падения — обычный uart_flush отдаёт только то,
   что влезло в FIFO, и хвост строки теряется. */
void uart_flush_all(void);

#endif /* UART_H */