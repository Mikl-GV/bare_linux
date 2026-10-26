#include "h3_mmu.h"

/* mmu_mark_uncached(): пометить диапазон как Normal Non-Cacheable.
 *
 * Работает на существующей таблице (TTBR0), не перестраивая её. Это стабильнее
 * полной перестройки таблицы (проверено: переключение TTBR0 на свою таблицу
 * на лету даёт data abort из-за несоответствия TLB/кэша с кодом).
 *
 * Поддерживает ОБА типа карты памяти ARMv7:
 *   - Section-записи L1 (тип 0b10): правим секцию.
 *   - Двухуровневую L1->L2 (Large page 16KB): спускаемся в L2 и правим страницу.
 */

static inline void dcc_clean_line(uint32_t addr)
{
    __asm volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(addr)); /* DCCMVAC */
}

void mmu_mark_uncached(uint32_t addr)
{
    uint32_t l1_base, ref, idx;

    __asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(l1_base));  /* TTBR0 */
    l1_base &= ~0x3FFFu;

    idx = (addr >> 20) & 0xFFF;
    ref = ((volatile uint32_t *)l1_base)[idx];

    switch (ref & 3u) {
    case 0b10u: {   /* Section (1MB) */
        uint32_t pa = l1_base + idx * 4;
        uint32_t ne = (addr & 0xFFF00000u)
                    | (ref & 0x0008DDF0u)
                    | (1u << 16)          /* S */
                    | (0u << 3) | (0u << 2) /* C=0,B=0 */
                    | 0b10u;
        ((volatile uint32_t *)l1_base)[idx] = ne;
        dcc_clean_line(pa);
        break;
    }
    case 0b11u: {   /* L2 (page-table) */
        uint32_t l2_base = ref & 0xFFFFFC00u;
        /* Защита от мусорных L1-записей: U-Boot на стенде оставляет
           L1[0x400..0x402]=0xFFFFFFFF (тип 3, HARDWARE.md §6). Тогда
           l2_base = 0xFFFFFC00 — разыменование несуществующей памяти
           (data abort на запуске). Если L2 вне RAM — не трогаем: пометку
           сделает вызывающий явным clean/invalidate (control-путь так и
           работает). */
        if (l2_base < 0x40000000u || l2_base >= 0x60000000u) break;
        uint32_t n = (addr >> 14) & 0x3FFu;
        volatile uint32_t *l2 = (volatile uint32_t *)l2_base;
        uint32_t e0 = l2[n * 2 + 0];
        uint32_t e1 = l2[n * 2 + 1];
        if ((e0 & 3u) == 0b01u) {          /* Large page (16KB) */
            e0 &= ~((1u << 3) | (1u << 2));  /* C=0,B=0 */
            l2[n * 2 + 0] = e0;
            l2[n * 2 + 1] = e1;
            dcc_clean_line(l2_base + n * 8);
        }
        break;
    }
    default:
        break;   /* невалидная запись — игнорируем */
    }

    __asm volatile("dsb" ::: "memory");
    __asm volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));  /* TLBIALL */
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

/* ---- L2C-310 (PL310) @ 0x01C43000 ----
 * На H3 под L1 стоит L2-кэш. cache_clean (DCCMVAC) сбрасывает строку лишь
 * в L2, НЕ в DRAM. DMA-мастер (OHCI/EHCI/DMAC) читает DRAM напрямую, поэтому
 * без L2-операции он видит устаревшие данные: EHCI/OHCI «не видят» ED/TD,
 * хотя CPU читает корректные значения. Операции by-PA (CLEAN_INV_PA 0x7B0)
 * точечные и не требуют глобального flush. */
#define L2CC_BASE        0x01C43000u
#define L2CC_CACHE_ID    0x000u
#define L2CC_CACHE_SYNC  0x730u
#define L2CC_CLEAN_INV_PA 0x7B0u

/* Определить наличие PL310 по CACHE_ID (0x410000C8/C9 у PL310 r3).
 * Если контроллера нет или адрес неверен — НЕ трогаем L2: запись by-PA в
 * неизвестный регистр может затереть чужую периферию. */
static int l2_present(void)
{
    uint32_t id = ((volatile uint32_t *)L2CC_BASE)[L2CC_CACHE_ID / 4];
    return ((id & 0xFFFFFF00u) == 0x41000000u) && ((id & 0x0Fu) >= 8u);
}

static inline void l2_sync(void)
{
    if (!l2_present()) return;
    volatile uint32_t *r = (volatile uint32_t *)L2CC_BASE;
    r[L2CC_CACHE_SYNC / 4] = 0;
    for (volatile int i = 0; i < 1000 && (r[L2CC_CACHE_SYNC / 4] & 1u); i++)
        ;
}

/* Сбросить в DRAM и инвалидировать строки диапазона во ВСЕХ уровнях
 * (L1 -> L2 -> DRAM). Для буферов/дескрипторов, которые читает DMA. */
void dma_clean_range(uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~0x1Fu;
    uint32_t end   = (addr + len + 0x1Fu) & ~0x1Fu;
    volatile uint32_t *l2 = (volatile uint32_t *)L2CC_BASE;

    /* L1: clean+invalidate by MVA (DCCIMVAC) — строка уходит в L2. */
    for (uint32_t a = start; a < end; a += 32)
        __asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a));
    __asm volatile("dsb" ::: "memory");

    /* L2: clean+invalidate by PA — строка уходит в DRAM. */
    l2_sync();
    for (uint32_t a = start; a < end; a += 32)
        l2[L2CC_CLEAN_INV_PA / 4] = a;
    l2_sync();
    __asm volatile("dsb" ::: "memory");
}

/* Инвалидировать диапазон после записи DMA: сперва L2 (чтобы не поднялись
 * грязные строки), затем L1. */
void dma_inval_range(uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~0x1Fu;
    uint32_t end   = (addr + len + 0x1Fu) & ~0x1Fu;
    volatile uint32_t *l2 = (volatile uint32_t *)L2CC_BASE;

    l2_sync();
    for (uint32_t a = start; a < end; a += 32)
        l2[L2CC_CLEAN_INV_PA / 4] = a;
    l2_sync();
    __asm volatile("dsb" ::: "memory");
    for (uint32_t a = start; a < end; a += 32)
        __asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a));  /* DCIMVAC */
    __asm volatile("dsb" ::: "memory");
}

/* Clean (write-back) D-cache для диапазона. Идём по 32-байтным кэш-линиям,
   прикрывая границы (округляем start вниз, end вверх к линии 32). */
void dcc_clean_range(uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~0x1Fu;
    uint32_t end = (addr + len + 0x1Fu) & ~0x1Fu;
    for (uint32_t a = start; a < end; a += 32)
        dcc_clean_line(a);
    __asm volatile("dsb" ::: "memory");
}