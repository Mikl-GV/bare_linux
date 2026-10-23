#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

/* Почта CPU0 (эмулятор/меню) <-> CPU1 (звук).
 *
 * Область живёт в .coherent (uncached, нормальная non-cacheable) — см.
 * linker.ld: между CPU0 и CPU1 нет разделяемого кэша, поэтому все флаги и
 * кольцо сообщений должны лежать в некэшируемой памяти (или чиститься
 * вручную). Размещена как отдельная секция .mailbox.
 *
 * Протокол — простой:
 *   - CPU0 кладёт сообщение в кольцо и ставит флаг MB_FLAG_CPU0_PENDING;
 *   - CPU1 читает сообщение, обрабатывает, снимает флаг и (если нужно)
 *     отвечает собственным сообщением + MB_FLAG_CPU1_PENDING;
 *   - поля читаются/пишутся со __dmb()/__dsb() — согласованность на
 *     многоядерном ARM без разделяемого кэша.
 *
 * Одно сообщение — фиксированный размер (байты + параметр 32-бит).
 * Кольцо одного направления.
 */

#define MB_SLOTS       8
#define MB_MSG_BYTES   16
#define MB_MSG_WORDS   (MB_MSG_BYTES / 4)

enum {
    MB_MSG_NONE = 0,
    MB_MSG_AUDIO_PLAY,     /* запустить тон/звук: param = частота Гц */
    MB_MSG_AUDIO_STOP,     /* остановить звук */
    MB_MSG_AUDIO_DATA,     /* передать PCM: param = адрес, param2 = кадров */
    MB_MSG_EMU_LOAD,       /* CPU0 -> CPU1: эмулятор загружен (подготовка) */
    MB_MSG_EMU_EXIT,       /* CPU0 -> CPU1: эмулятор завершён (звук стоп) */
};

/* Разделяемая структура почты (в uncached .mailbox). */
struct mailbox {
    volatile uint32_t flag_cpu0;   /* 0/1: CPU0 поставил сообщение */
    volatile uint32_t flag_cpu1;   /* 0/1: CPU1 ответил/ждёт */
    volatile uint32_t wridx;       /* писать сюда (CPU0) */
    volatile uint32_t rdidx;       /* читать отсюда (CPU1) */
    struct {
        volatile uint32_t cmd;
        volatile uint32_t param;
        volatile uint32_t param2;
        volatile uint32_t reserved;
    } slot[MB_SLOTS];
};

/* Доступ к общей области почты (инициализируется mb_init()). */
struct mailbox *mb_get(void);

/* Инициализация (с нуля, вызывается на ядре 0 ДО запуска CPU1). */
void mb_init(void);

/* CPU0: отправить сообщение. Блокирующая (ждёт свободный слот), т.к. слоты
 * освобождает CPU1. Возвращает 0 при успехе. */
int  mb_send(uint32_t cmd, uint32_t param, uint32_t param2);

/* CPU1: принять сообщение. Неблокирующая; 0 если пусто. */
int  mb_recv(uint32_t *cmd, uint32_t *param, uint32_t *param2);

#endif /* MAILBOX_H */