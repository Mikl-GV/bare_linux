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
#include "h3_smp.h"

void audio_cpu1_main(void)
{
    /* Первой инструкцией — «живой» magic в SRAM-почту (требование
       h3_secondary_start): иначе core0 ждёт его 20 с и печатает «not alive»,
       хотя CPU1 уже работает. Через asm, чтобы GCC не ругался -Warray-bounds
       на доступ к 0x20. */
    __asm volatile("str %0, [%1]" :: "r"(H3_CPU_MAGIC), "r"(H3_CPU_MAIL_ADDR) : "memory");
    __asm volatile("dsb" ::: "memory");

    uart_print("cpu1: audio worker up\r\n");
    for (;;) {
        uint32_t cmd = 0, prm = 0, prm2 = 0;
        if (mb_recv(&cmd, &prm, &prm2) == 1) {
            if (cmd == MB_MSG_AUDIO_DATA && prm && prm2 > 0) {
                /* i2s_write копирует данные из буфера prm (указывает на
                   gAudioBuffer CPU0, uncached .coherent) СИНХРОННО. После
                   возврата буфер больше не нужен CPU1 — подтверждаем. */
                i2s_write((const int16_t *)(uintptr_t)prm, prm2);
                struct mailbox *mb = mb_get();
                mb->audio_done++;
                __asm volatile("dsb" ::: "memory");
            }
        }
        /* Между сообщениями НЕ спим (ни wfi, ни wfe): CPU0 ждёт audio_done в
         * emu_audio_push busy-poll'ом, а без IPI/SEV от mb_send событие для
         * wfe не приходит — CPU1 навсегда уснул бы и не инкрементил
         * audio_done (дедлок: зависание после первого кадра). CPU1 выделен
         * только под звук — чистый busy-loop опроса почты. */
        __asm volatile("dmb" ::: "memory");
    }
}