#include "uart.h"
#include "h3.h"
#include "h3_ccu.h"
#include "ports.h"

/* UART0, 115200 8N1. U-Boot уже задал скорость; здесь лишь включаем
   такт/reset и PA4 = TX (F2). */

#define UART0_USR (*(volatile uint32_t *)(H3_UART0_BASE + 0x7C))
#define UART0_THR (*(volatile uint32_t *)(H3_UART0_BASE + 0x00))

/* W.278: кольцевой буфер вывода. uart_putc пишет в память (машино-быстро,
   не влияет на фазы/тайминги SDIO/RF), а реальная передача в UART идёт
   порциями при вызовах uart_flush() из udelay()/пауз — так лог больше не
   тормозит протокол. */
#define UART_BUF_SIZE 4096
static volatile uint32_t uart_wr = 0;   /* индекс записи */
static volatile uint32_t uart_rd = 0;   /* индекс чтения */
static volatile char uart_buf[UART_BUF_SIZE];

void uart_init(void)
{
    set_pin_mode(PORTA, 4, 2);   /* PA4 = UART0 TX (F2) */

    H3_CCU_GATING3 |= (1u << 16);   /* такт UART0 */
    H3_CCU_RST4    |= (1u << 16);   /* снять reset UART0 */
    volatile uint32_t n = 1000;
    while (n--) {}
    uart_wr = 0;
    uart_rd = 0;
}

static int uart_tx_ready(void)
{
    return (UART0_USR & (1u << 1)) ? 1 : 0;
}

/* Вытолкнуть разом всё, что есть в буфере (до 256 байт за вызов), чтобы
   не держать CPU на busy-wait дольше микросекунд. */
void uart_flush(void)
{
    uint32_t wr = uart_wr;
    uint32_t rd = uart_rd;
    uint32_t avail = (uint32_t)(wr - rd);   /* unsigned wrap-safe */
    if (avail > 256) avail = 256;
    uint32_t sent = 0;
    while (sent < avail && uart_tx_ready()) {
        UART0_THR = (uint32_t)(unsigned char)uart_buf[rd & (UART_BUF_SIZE - 1)];
        rd++;
        sent++;
    }
    uart_rd = rd;
}

void uart_putc_buffered(char c)
{
    uint32_t wr = uart_wr;
    uint32_t rd = uart_rd;
    /* Если буфер полностью заполнен — принудительно слить всё. */
    if ((uint32_t)(wr - rd) >= (UART_BUF_SIZE - 256)) {
        uart_flush();
        rd = uart_rd;
    }
    uart_buf[wr & (UART_BUF_SIZE - 1)] = c;
    wr++;
    uart_wr = wr;
}

void uart_putc(char c)
{
    uart_putc_buffered(c);
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