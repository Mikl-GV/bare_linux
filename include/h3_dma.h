#ifndef H3_DMA_H
#define H3_DMA_H

#include <stdint.h>

/* sunxi-DMA (H3): циклическая передача RAM -> I2S0_TX_FIFO.
 *
 * Кольцо из N LLI-дескрипторов равного размера (0 -> 1 -> ... -> N-1 -> 0).
 * Буфер обязан лежать в uncached-памяти (.dma_buf) и быть доступен DMAC по
 * физическому адресу; LLI лежат в .coherent.
 *
 * Последовательность инициализации (по даташиту/HARDWARE-REF):
 *   1) включить такт DMA (gate) и снять reset, если он удержан;
 *   2) пометить все каналы non-secure (DMA_SECURE_REG) и включить auto-gating;
 *   3) погасить осиротевшие SPL/U-Boot DMA-зомби;
 *   4) выбрать свободный канал и заполнить его LLI-дескрипторы.
 *
 * Прерывания DMA не включаем (IRQ_EN=0), поэтому pending-флаги не накапливаются
 * и канал работает без вмешательства CPU (даташит 4.11.4.1). Позиция чтения
 * берётся из регистров канала (LLI/CUR_SRC), см. h3_dma_audio_pos_bytes().
 */

/* Максимум периодов в кольце (LLI-массив фиксированного размера). */
#define H3_DMA_AUDIO_PERIODS_MAX  8

/* buf_bytes делится на periods ровно; период кратен 4 байтам. */
int  h3_dma_audio_init(uint32_t buf_phys, uint32_t buf_bytes, int periods);
void h3_dma_audio_start(void);
void h3_dma_audio_stop(void);

/* Текущее смещение чтения внутри кольца в байтах (0..buf_bytes-1).
 * Читает CUR_SRC канала (0x10, RO — HARDWARE-REF.md:51); если значение вне
 * диапазона буфера (канал остановлен/не инициализирован) — возвращает
 * последнее достоверное. Позиция обновляется только при работающем канале. */
uint32_t h3_dma_audio_pos_bytes(void);

#endif /* H3_DMA_H */
