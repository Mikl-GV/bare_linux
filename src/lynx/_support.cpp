/* Поддержка C++ для bare-metal слоя Lynx (Handy).
 * Проект собран без libstdc++ (-fno-exceptions -fno-rtti -nostdlib),
 * поэтому глобальные new/delete реализуем поверх mem_alloc/mem_free,
 * а недостающие символы рантайма (pure virtual, guard) — заглушками. */

#include <stddef.h>
extern "C" {
#include "mem.h"
}

void *operator new(size_t size)
{
    void *p = mem_alloc((uint32_t)size);
    return p ? p : (void *)0;
}

void *operator new[](size_t size)
{
    void *p = mem_alloc((uint32_t)size);
    return p ? p : (void *)0;
}

void operator delete(void *p) noexcept
{
    if (p) mem_free(p);
}

void operator delete[](void *p) noexcept
{
    if (p) mem_free(p);
}

void operator delete(void *p, size_t) noexcept
{
    if (p) mem_free(p);
}

void operator delete[](void *p, size_t) noexcept
{
    if (p) mem_free(p);
}

/* Вызов виртуального чисто-виртуального метода: в эмуляторе не встречается,
 * но линкеру нужен символ. */
extern "C" void __cxa_pure_virtual(void)
{
    for (;;) ;
}

/* Ядро Handy и blip (lynxdec, Blip_Buffer) используют C-аллокаторы
 * (calloc/malloc/realloc/free). Для bare-metal — поверх mem_alloc/mem_free. */
extern "C" void *malloc(size_t n)    { return mem_alloc((uint32_t)n); }
extern "C" void *calloc(size_t n, size_t sz)
{
    uint32_t total = (uint32_t)(n * sz);
    uint8_t *p = (uint8_t *)mem_alloc(total);
    if (p) for (uint32_t i = 0; i < total; i++) p[i] = 0;
    return p;
}
extern "C" void *realloc(void *p, size_t n)
{
    /* Простого realloc нет: выделяем новый, копируем старый (размер неизвестен —
     * копируем n, рискованно, но blip использует realloc для увеличения буфера
     * и сразу читает новые данные; перезаписываем стартовые байты из старого). */
    if (!p) return malloc(n);
    void *np = mem_alloc((uint32_t)n);
    if (np) {
        /* старое содержимое переносим побайтно; размер старого блока возьмём
           из первого слова заголовка mem (см. mem.c: size в байтах данных). */
        uint32_t oldsz = *(uint32_t *)((uint8_t *)p - 8u) & ~1u;
        uint32_t cp = (oldsz < (uint32_t)n) ? oldsz : (uint32_t)n;
        for (uint32_t i = 0; i < cp; i++) ((uint8_t *)np)[i] = ((uint8_t *)p)[i];
        mem_free(p);
    }
    return np;
}
extern "C" void free(void *p) { if (p) mem_free(p); }

/* ---- Недостающие libc-символы (проект с -nostdlib; memcpy/memset уже в
 * sys.c, strlen/strcmp/... частично используются) ---- */

extern "C" void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

extern "C" size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t i, sl;
    for (i = 0; i + 1 < size && dst[i]; i++) ;
    for (sl = 0; src[sl]; sl++) ;
    if (i < size) {
        size_t k = 0;
        while (k < size - i - 1 && src[k]) { dst[i + k] = src[k]; k++; }
        dst[i + k] = 0;
    }
    return i + sl;
}

extern "C" size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t sl = 0;
    while (src[sl]) sl++;
    if (size) {
        size_t k = 0;
        while (k < size - 1 && src[k]) { dst[k] = src[k]; k++; }
        dst[k] = 0;
    }
    return sl;
}

extern "C" void __assert_func(const char *file, int line,
                              const char *func, const char *expr)
{
    (void)file; (void)line; (void)func; (void)expr;
    for (;;) ;
}

extern "C" double floor(double x)
{
    /* округление вниз для x >= 0 (blip использует только с положительными) */
    if (x < 0) return (double)(long long)x - 1;   /* не используется в ядре */
    return (double)(long long)x;
}

/* ---- Строковые функции (в проекте есть только memset в sys.c) ---- */

extern "C" void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

extern "C" int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

extern "C" size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

extern "C" char *strcpy(char *dst, const char *src)
{
    size_t i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

extern "C" char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = 0;
    return dst;
}

/* Минимальный sprintf для ядра: поддерживает %x, %d, %s и ширину
 * (например %04x, %03d). Других спецификаторов в ядре не встречается. */
static void fmt_uint_hex(char *out, uint32_t v, int width)
{
    char tmp[16];
    int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while (v && n < 15);
    while (n < width) tmp[n++] = '0';
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static void fmt_uint_dec(char *out, uint32_t v, int width)
{
    char tmp[16];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 15);
    while (n < width) tmp[n++] = '0';
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

extern "C" int sprintf(char *buf, const char *fmt, ...)
{
    uint32_t *ap = (uint32_t *)(&fmt + 1);
    char *out = buf;
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { *out++ = *p++; continue; }
        p++;
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        char c = *p++;
        switch (c) {
        case 'x': case 'X': {
            char tmp[16];
            uint32_t v = *ap++;
            fmt_uint_hex(tmp, v, width);
            for (const char *q = tmp; *q; q++) *out++ = *q;
            break;
        }
        case 'd': case 'u': {
            char tmp[16];
            uint32_t v = *ap++;
            fmt_uint_dec(tmp, v, width);
            for (const char *q = tmp; *q; q++) *out++ = *q;
            break;
        }
        case 's': {
            const char *s = (const char *)*ap++;
            if (!s) s = "(null)";
            while (*s) *out++ = *s++;
            break;
        }
        case '%': *out++ = '%'; break;
        default: *out++ = '%'; if (c) *out++ = c; break;
        }
    }
    *out = 0;
    return (int)(out - buf);
}