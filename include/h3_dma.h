#ifndef H3_DMA_H
#define H3_DMA_H

#include <stdint.h>

/* sunxi-DMA (H3): циклическая передача RAM -> I2S0_TX_FIFO.
 *
 * Два LLI-дескриптора, каждый на половину буфера (0 -> 1 -> 0). Буфер обязан
 * лежать в uncached-памяти и быть заполнен до h3_dma_audio_init().
 *
 * Последовательность инициализации (по даташиту/HARDWARE-REF):
 *   1) включить такт DMA (gate) и снять reset, если он удержан;
 *   2) пометить все каналы non-secure (DMA_SECURE_REG) и включить auto-gating;
 *   3) погасить осиротевшие SPL/U-Boot DMA-зомби;
 *   4) выбрать свободный канал и заполнить его LLI-дескриптор.
 *
 * Прерывания DMA не включаем (IRQ_EN=0), поэтому pending-флаги не накапливаются
 * и канал работает без вмешательства CPU (даташит 4.11.4.1).
 */

int  h3_dma_audio_init(uint32_t buf_phys, uint32_t buf_bytes);
void h3_dma_audio_start(void);
void h3_dma_audio_stop(void);

/* Номер фактически используемого канала после init, или -1. */
int  h3_dma_audio_chan(void);

#endif /* H3_DMA_H */