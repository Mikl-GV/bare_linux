#ifndef H3_MMU_H
#define H3_MMU_H

#include <stdint.h>

/* Пометить диапазон как Normal Non-Cacheable (для DMA-когерентности).
 * Работает на существующей таблице U-Boot: для section-записей L1 правит
 * B/C биты, для двухуровневой таблицы (L1->L2) правит Large-page в L2.
 * НЕ перестраиваем таблицу целиком и НЕ отключаем MMU на лету — это
 * приводит к сбоям (проверено на стенде). */

void mmu_mark_uncached(uint32_t addr);

/* Clean (write-back) D-cache для диапазона [addr, addr+len) в физическую память.
   Необходимо перед тем, как DMAC будет читать данные, которые CPU записал из
   кэша (LLI-дескрипторы, DMA-буфер). Без этого DMAC прочитает старые/мусорные
   данные из DRAM (платформа H3 не кэш-когерентна для DMA). */
void dcc_clean_range(uint32_t addr, uint32_t len);

/* Полная синхронизация для DMA: L1 (clean+invalidate by MVA) и L2C-310
 * (clean+invalidate by PA). На H3 cache_clean сбрасывает строку только в L2,
 * а DMA-мастер читает DRAM напрямую — без L2-операции он видит устаревшие
 * ED/TD/буферы. Использовать вместо dcc_clean_range перед передачей DMA. */
void dma_clean_range(uint32_t addr, uint32_t len);

/* Инвалидация после записи DMA (L2, затем L1), чтобы CPU не читал
 * устаревшие строки вместо свежих данных из DRAM. */
void dma_inval_range(uint32_t addr, uint32_t len);

#endif /* H3_MMU_H */