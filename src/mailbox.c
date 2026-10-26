#include "mailbox.h"
#include <stddef.h>

/* Реализация почты CPU0<->CPU1. Область — статическая, в секции .mailbox
 * (linker.ld размещает её в uncached-память; см. mmu_mark_uncached в h3_mmu.c). */

static struct mailbox g_mb __attribute__((section(".mailbox"), aligned(64)));

struct mailbox *mb_get(void)
{
    return (struct mailbox *)&g_mb;
}

void mb_init(void)
{
    struct mailbox *m = &g_mb;
    for (int i = 0; i < MB_SLOTS; i++) {
        m->slot[i].cmd = MB_MSG_NONE;
        m->slot[i].param = 0;
        m->slot[i].param2 = 0;
        m->slot[i].reserved = 0;
    }
    m->flag_cpu0 = 0;
    m->flag_cpu1 = 0;
    m->wridx = 0;
    m->rdidx = 0;
    m->audio_done = 0;
    __asm volatile("dsb" ::: "memory");
}

int mb_send(uint32_t cmd, uint32_t param, uint32_t param2)
{
    struct mailbox *m = &g_mb;
    /* ждём свободный слот: rd/wr индексы не должны совпадать (кольцо полно) */
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        uint32_t wr = m->wridx;
        uint32_t rd = m->rdidx;
        uint32_t next = (wr + 1) % MB_SLOTS;
        if (next != rd) {
            __asm volatile("dmb" ::: "memory");
            m->slot[wr].cmd    = cmd;
            m->slot[wr].param  = param;
            m->slot[wr].param2 = param2;
            m->slot[wr].reserved = 0;
            __asm volatile("dsb" ::: "memory");
            m->wridx = next;
            __asm volatile("dsb" ::: "memory");
            m->flag_cpu0 = 1;   /* CPU1: есть сообщение */
            __asm volatile("dsb" ::: "memory");
            return 0;
        }
        __asm volatile("dmb" ::: "memory");
    }
    return -1;   /* кольцо переполнено (CPU1 не разбирает) */
}

int mb_recv(uint32_t *cmd, uint32_t *param, uint32_t *param2)
{
    struct mailbox *m = &g_mb;
    uint32_t rd = m->rdidx;
    uint32_t wr = m->wridx;
    if (rd == wr) return 0;   /* пусто */
    __asm volatile("dmb" ::: "memory");
    uint32_t c   = m->slot[rd].cmd;
    uint32_t p   = m->slot[rd].param;
    uint32_t p2  = m->slot[rd].param2;
    if (cmd)   *cmd   = c;
    if (param) *param = p;
    if (param2)*param2 = p2;
    __asm volatile("dsb" ::: "memory");
    m->rdidx = (rd + 1) % MB_SLOTS;   /* освободили слот */
    __asm volatile("dsb" ::: "memory");
    m->slot[rd].cmd = MB_MSG_NONE;
    return 1;
}