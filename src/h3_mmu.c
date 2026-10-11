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