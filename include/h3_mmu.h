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

#endif /* H3_MMU_H */