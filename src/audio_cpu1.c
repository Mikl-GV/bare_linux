/* Поток CPU1 (слой звука): принимает PCM-куски по почте и пишет их в
 * кольцо i2s (DMA играет независимо). Архитектура ядер проекта:
 * CPU0 — эмулятор/меню, CPU1 — обработка и вывод звука (занят DMA).
 *
 * Протокол почты (mailbox.h):
 *   MB_MSG_AUDIO_DATA: param = адрес PCM (стерео int16), param2 = число кадров.
 * Буфер PCM должен лежать в uncached (.coherent), чтобы DMA/кэш не спорили —
 * ядро Handy (glue) шлёт свой gAudioBuffer (в .coherent).
 */

#include "mailbox.h"
#include "i2s.h"
#include "uart.h"
#include "sys.h"

void audio_cpu1_main(void)
{
    uart_print("cpu1: audio worker up\r\n");
    for (;;) {
        uint32_t cmd = 0, prm = 0, prm2 = 0;
        if (mb_recv(&cmd, &prm, &prm2) == 1) {
            if (cmd == MB_MSG_AUDIO_DATA && prm && prm2 > 0) {
                i2s_write((const int16_t *)(uintptr_t)prm, prm2);
            }
        }
        /* Между сообщениями — сон до следующего события. */
        __asm volatile("wfi" ::: "memory");
    }
}