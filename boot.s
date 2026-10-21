/* boot.s — точка входа Allwinner H3 (Orange Pi Lite).
 *
 * Загрузка:
 *   - U-Boot: `go 0x40000000` (кэши/MMU унаследованы от U-Boot);
 *   - FEL: SPL инициализирует DRAM, затем `write` + `exe 0x40000000`.
 *
 * Важно:
 *   - PA0..PA3 сразу в Function 3 (JTAG), чтобы подключить OpenOCD
 *     с самого старта (H3 не включает JTAG сам).
 *   - MMU не трогаем: если U-Boot включил кэши — работаем с ними
 *     (DMA-буферы помечаем uncached из C-кода).
 */

.syntax unified
.arch armv7-a
.arm

/* ===== Векторы исключений (VBAR = 0x40000000) ===== */
.section .vectors, "ax"
.global _vectors
_vectors:
    ldr pc, =_start        /* 0x00 Reset */
    ldr pc, =_hang         /* 0x04 Undefined */
    ldr pc, =_hang         /* 0x08 SWI */
    ldr pc, =_hang         /* 0x0C Prefetch Abort */
    ldr pc, =_hang         /* 0x10 Data Abort */
    nop                    /* 0x14 reserved */
    ldr pc, =_hang         /* 0x18 IRQ */
    ldr pc, =_hang         /* 0x1C FIQ */

/* ===== Точка входа ===== */
.section .text
.global _start
_start:
    /* SVC mode, IRQ/FIQ off */
    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0xD3
    msr cpsr, r0

    /* Включить JTAG: PA0-3 = Function 3 (0x3333). PA4-7 не задеваются:
       маска 0x0000FFFF покрывает только поля PA0..PA3 (биты 0..15),
       PA4 (UART-TX) занимает биты 16..19 и остаётся как был. */
    ldr r0, =0x01C20800
    ldr r1, [r0]
    movw r2, #0x3333
    movw r3, #0xFFFF
    bic r1, r1, r3
    orr r1, r1, r2
    str r1, [r0]
    dsb

    /* VBAR = наши векторы */
    ldr r0, =_vectors
    mcr p15, 0, r0, c12, c0, 0
    dsb
    isb

    /* VFP/NEON включить */
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(0xF << 20)
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #0x40000000
    vmsr fpexc, r0
    isb

    /* Стеки исключений */
    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0xD1       /* FIQ */
    msr cpsr, r0
    ldr sp, =0x5FF02000

    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0xD2       /* IRQ */
    msr cpsr, r0
    ldr sp, =0x5FF03000

    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0xD7       /* ABT */
    msr cpsr, r0
    ldr sp, =0x5FF01000

    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0xDB       /* UND */
    msr cpsr, r0
    ldr sp, =0x5FF00000

    /* SVC, стек */
    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0xD3
    msr cpsr, r0
    ldr sp, =0x60000000

    /* Обнулить BSS и .coherent (NOLOAD) */
    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
1:  cmp r0, r1
    bhs 2f
    str r2, [r0], #4
    b 1b
2:
ldr r0, =_coherent_start
    ldr r1, =_coherent_end
3:  cmp r0, r1
    bhs 4f
    str r2, [r0], #4
    b 3b
4:
    bl kmain

/* ===== Заглушка ===== */
.section .text
.global _hang
_hang:
    b _hang
.end
