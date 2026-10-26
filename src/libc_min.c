/* libc_min.c — минимальная libc-подложка для сторонних ядер эмуляторов
 * (binjgb и др.), которые без -nostdlib компилятора обходятся только
 * подложкой. Предоставляет: printf/fprintf(stderr), malloc/calloc/free,
 * memcpy (memset есть в sys.c), exit.
 *
 * printf — проверенная реализация из pico-retro-new-gen (h3_bare/src/printf.c):
 * формат %d/%u/%x/%X/%s/%c/%p, ширина '%5d', паддинг '%05d', l-модификатор.
 * Повторный экспорт printf конфликтует с sega_pad.c (extern printf) — НЕ
 * добавлять ещё один символ printf, здесь только определение (если линкер
 * возьмёт его).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "uart.h"
#include "mem.h"

/* Forward-объявления: realloc использует free/memcpy до их определений,
 * а компилятор в -std=gnu99 ругается на неявные. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *memcpy(void *dst, const void *src, size_t n);
void abort(void);
void exit(int code);

/* ---- printf: pico-retro форматтер (достаточно для всех ядер) ---- */

static void pputc(char c)
{
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
}

static void print_uint(unsigned long v, int base, int upper, int width, char pad)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    do { buf[i++] = digits[v % base]; v /= base; } while (v && i < 32);
    if (width > 31) width = 31;
    while (i < width) buf[i++] = pad;
    while (i) pputc(buf[--i]);
}

static void print_int(long v, int width, char pad)
{
    char buf[32];
    int i = 0;
    unsigned long u = (unsigned long)v;
    int neg = (v < 0);
    if (neg) u = 0ul - u;
    do { buf[i++] = (char)('0' + (u % 10)); u /= 10; } while (u && i < 32);
    if (width > 31) width = 31;
    int need = width - i - (neg ? 1 : 0);
    if (neg && pad == '0') pputc('-');
    while (need-- > 0) pputc(pad);
    if (neg && pad != '0') pputc('-');
    while (i) pputc(buf[--i]);
}

static int do_printf(const char *fmt, va_list ap)
{
    /* %zu — size_t (binjgb): обрабатываем как %lu на 32-битной платформе. */
    for (; *fmt; fmt++) {
        if (*fmt != '%') { pputc(*fmt); continue; }
        fmt++;
        int width = 0;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }   /* %zu */
        if (*fmt == 't') { lng = 1; fmt++; }   /* %tu */
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; { const char *p = s; while (*p++) len++; }
            if (width > 31) width = 31;
            for (int sp = width - len; sp > 0; sp--) pputc(' ');
            while (*s) pputc(*s++);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (width > 31) width = 31;
            for (int sp = width - 1; sp > 0; sp--) pputc(' ');
            pputc(c);
            break;
        }
        case 'd': case 'i':
            print_int(lng ? va_arg(ap, long) : (long)va_arg(ap, int), width, pad);
            break;
        case 'u': {
            unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned);
            print_uint(v, 10, 0, width, pad);
            break;
        }
        case 'x': case 'X': {
            unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned);
            print_uint(v, 16, *fmt == 'X', width, pad);
            break;
        }
        case 'p': {
            const void *v = va_arg(ap, const void *);
            pputc('0'); pputc('x'); print_uint((unsigned long)v, 16, 0, 0, ' ');
            break;
        }
        case '%': pputc('%'); break;
        default: pputc('%'); if (*fmt) pputc(*fmt); break;
        }
    }
    return 0;
}

/* printf/vprintf НЕ определяем: они уже есть в lynx/_support.cpp
   (printf -> sprintf -> uart_print). Здесь только fprintf для binjgb. */

/* fprintf(stderr, ...) — binjgb использует PRINT_ERROR и %s:%d: в CHECK_MSG. */
int fprintf(void *stream, const char *fmt, ...)
{
    (void)stream;   /* всегда stderr */
    va_list ap;
    va_start(ap, fmt);
    int r = do_printf(fmt, ap);
    va_end(ap);
    return r;
}

/* --- binjgb: stderr/fprintf идёт через _impure_ptr (newlib).
     В bare-metal ставим NULL — наш fprintf не использует reent.
     malloc/calloc/free/realloc/memcpy/printf — в lynx/_support.cpp. */
void *_impure_ptr;

void abort(void)   /* assert() провалился — не возвращаемся */
{
    for (;;) __asm volatile("wfi" ::: "memory");
}

void exit(int code)
{
    (void)code;
    abort();
}

/* --- binjgb: файловый слой (save/state) — в нашей среде файловая система
     SD/USB-FAT, файл-хэлперы не используются (rom грузится в память).
     Даём пустые стабы, чтобы линкер закрыл символы. */
typedef struct { void *data; size_t size; } FileData;

int file_data_resize(FileData *file_data, size_t new_size)
{
    (void)file_data; (void)new_size;
    abort();
    return -1;
}

void file_data_delete(FileData *file_data)
{
    (void)file_data;
}

FileData *file_read(const char *path)
{
    (void)path;
    return 0;
}

int file_write(const char *path, const FileData *file_data)
{
    (void)path; (void)file_data;
    return -1;
}