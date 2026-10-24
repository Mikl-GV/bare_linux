#include "mem.h"
#include "sys.h"

/* Простой first-fit аллокатор для кучи эмулятора (без libc).
 * Блок: [8-байт заголовок: size | used(бит0)] [данные...] — выровнен 8.
 * Свободный список: односвязный через поле next в данных блока.
 * Дефраг по освобождению: merge с соседним свободным.
 *
 * ROM-зона: отдельный участок (жёстко зарезервирован), выдаётся одной
 * областью под игру и освобождается целиком по выходу из эмулятора. */

#define MEM_ALIGN  8u
#define MEM_HDR    (uint32_t)MEM_ALIGN

struct mem_blk {
    uint32_t size;      /* размер ДАННЫХ (без заголовка), бит0 = used */
    struct mem_blk *next;
};

static struct mem_blk *g_free = 0;
static uint32_t g_heap_total = 0;

uint32_t mem_heap_start = 0;
uint32_t mem_heap_end   = 0;
uint32_t mem_rom_start  = 0;
uint32_t mem_rom_end    = 0;

static uint32_t align_up(uint32_t v) { return (v + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1); }

void mem_init(uint32_t heap_size)
{
    uint32_t base = ((uint32_t)_end + 0xFFFFFu) & ~0xFFFFFu;   /* 1МБ-граница после образа */
    uint32_t stack_top = 0x60000000u;                          /* стек растёт вниз от верха */
    uint32_t rom_need = MEM_ROM_MAX_BYTES;                     /* 128 МБ */
    uint32_t avail = stack_top - base;

    if (heap_size == 0) {
        /* авто: куча 256 МБ, ROM-зона 128 МБ, запас между ними — остальное */
        heap_size = 256u * 1024u * 1024u;
    }
    if (rom_need > avail / 2) rom_need = avail / 2;   /* ROM не более половины */

    /* Резерв 2 МБ у верха DRAM: стеки исключений (0x5FF00000..0x5FF03000,
       boot.s) и стек CPU1 (0x5FE00000, h3_smp.c) не должны пересекаться с
       ROM-зоной. Раньше резерв был 64 КБ — ROM ~128 МБ затирал стеки. */
    mem_rom_end   = 0x5FE00000u;
    mem_rom_start = mem_rom_end - rom_need;

    mem_heap_start = base;
    mem_heap_end   = mem_rom_start;                   /* куча до ROM-зоны (с запасом) */
    if (heap_size > mem_heap_end - mem_heap_start)
        heap_size = mem_heap_end - mem_heap_start;

    /* один большой свободный блок */
    g_free = (struct mem_blk *)mem_heap_start;
    g_free->size = (mem_heap_start + heap_size) - (uint32_t)g_free - MEM_HDR;
    g_free->next = 0;
    g_heap_total = g_free->size;
}

static struct mem_blk *blk_from_data(void *p)
{
    return (struct mem_blk *)((uint8_t *)p - MEM_HDR);
}

static void *data_from_blk(struct mem_blk *b)
{
    return (void *)((uint8_t *)b + MEM_HDR);
}

void *mem_alloc(uint32_t size)
{
    size = align_up(size);
    struct mem_blk **pp = &g_free;
    while (*pp) {
        struct mem_blk *b = *pp;
        if (b->size >= size) {
            if (b->size >= size + MEM_HDR + MEM_ALIGN) {
                /* расколоть: остаток — новый свободный блок */
                struct mem_blk *rest = (struct mem_blk *)((uint8_t *)b + MEM_HDR + size);
                rest->size = b->size - size - MEM_HDR;
                rest->next = b->next;
                b->size = size;
                *pp = rest;
            } else {
                *pp = b->next;   /* целиком */
            }
            b->size |= 1u;     /* used */
            return data_from_blk(b);
        }
        pp = &b->next;
    }
    return 0;   /* нет свободного блока */
}

void mem_free(void *p)
{
    if (!p) return;
    struct mem_blk *b = blk_from_data(p);
    if (!(b->size & 1u)) return;       /* уже свободен — защита */
    b->size &= ~1u;
    /* вставить в голову списка (порядок не важен) */
    b->next = g_free;
    g_free = b;
    /* merge с соседом справа (по адресу) */
    struct mem_blk *n = (struct mem_blk *)((uint8_t *)b + MEM_HDR + b->size);
    for (struct mem_blk *s = g_free; s; s = s->next) {
        if (s == n && !(n->size & 1u)) {
            b->size += MEM_HDR + n->size;
            /* убрать n из списка (b уже в голове) */
            struct mem_blk **pp = &g_free;
            while (*pp && *pp != n) pp = &(*pp)->next;
            if (*pp) *pp = n->next;
            break;
        }
    }
    /* merge с соседом слева: ищем блок, чей конец совпадает с началом b */
    for (struct mem_blk **pp = &g_free; *pp; pp = &(*pp)->next) {
        struct mem_blk *s = *pp;
        if (!(s->size & 1u) &&
            (uint8_t *)s + MEM_HDR + s->size == (uint8_t *)b) {
            s->size += MEM_HDR + b->size;
            *pp = b->next;   /* выкинуть b из списка */
            break;
        }
    }
}

uint32_t mem_free_bytes(void)
{
    uint32_t f = 0;
    for (struct mem_blk *b = g_free; b; b = b->next)
        if (!(b->size & 1u)) f += b->size;
    return f;
}

void *mem_rom_reserve(void)
{
    if (!mem_rom_start || !mem_rom_end) return 0;
    /* один пользователь: стартовый адрес фиксированный */
    return (mem_rom_start && mem_rom_start < mem_rom_end) ? (void *)mem_rom_start : 0;
}

void mem_rom_release(void)
{
    /* никого: зона просто свободна снова (заполнять при следующем reserve) */
}

uint32_t mem_rom_capacity(void)
{
    return mem_rom_end - mem_rom_start;
}