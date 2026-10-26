#ifndef EMU_IO_H
#define EMU_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Слой ввода/звука для эмуляторов CPU0 (H3 multi-tool).
 *
 * ИДЕЯ: «жизнь эмулятора» = кадр из трёх шагов:
 *     input_poll(&st); core_run_frame(); audio_push(buf, n);
 * Ничего другого внутри кадра быть не должно: ни FAT, ни USB-блокировок,
 * ни uart_print, ни udelay. Все источники ввода и PCM-приёмник спрятаны
 * за этими тремя вызовами. Эмулятор включает ТОЛЬКО этот заголовок и
 * никогда не видит pcf8574.h / usb_kbd.h / i2s.h / mailbox.h.
 *
 * Маппинг кнопок — здесь (один раз), а не в каждом ядре. Ядро получает
 * PAD_* биты, понятные для ретро-приставки: A/B/X/Y/START/SELECT/D-Pad.
 * USB-клавиатура, PCF8574-кнопки, SEGA 6-btn (I2C) — всё сводится к этой
 * маске ядро->кнопка. Ниже кнопки названы PAD_* (нейтрально, без «BTN_»). */

/* Состояние ввода на один кадр (4 виртуальных порта максимум). */
#define EMU_MAX_PADS  4

enum {
    PAD_UP      = (1u << 0),
    PAD_DOWN    = (1u << 1),
    PAD_LEFT    = (1u << 2),
    PAD_RIGHT   = (1u << 3),
    PAD_A       = (1u << 4),   /* primary action */
    PAD_B       = (1u << 5),   /* secondary action */
    PAD_START   = (1u << 6),   /* start / pause */
    PAD_SELECT  = (1u << 7),   /* select / options */
    PAD_C       = (1u << 8),   /* SEGA C / third action */
    PAD_X       = (1u << 9),   /* SEGA X / extra */
    PAD_Y       = (1u << 10),  /* SEGA Y / extra */
    PAD_Z       = (1u << 11),  /* SEGA Z / extra */
    PAD_MODE    = (1u << 12),  /* SEGA Mode / turbo etc */
};

struct emu_input {
    uint16_t pads[EMU_MAX_PADS];
    uint8_t  npads;   /* сколько портов реально заполнено */
};

/* Ввод: собрать состояние на текущий кадр. ВСЕГДА мгновенный, не ждёт шин.
 * Источники по приоритету: PCF8574-кнопки -> SEGA 6-btn -> USB-клавиатура.
 * Каждый источник молчит, если его нет/не отвечает. */
void emu_input_poll(struct emu_input *st);

/* Долгое удержание START (для выхода из эмулятора) — из слоя ввода.
 * 1, если START удержан >= hold_ms. Ядро дёргает в конце кадра. */
int emu_input_exit_hold(uint32_t hold_ms);

/* ---- Звук: единственный способ для ядра отдать PCM ---- */

/* Записать n стереокадров (interleaved L,R int16) в звуковой слой.
 * Возвращает число ПРИНЯТЫХ кадров (может быть < n при переполнении
 * кольца — это НЕ ошибка ядра, а нехватка бюджета; ядро просто
 * продолжает и в следующем кадре данные уйдут). 0 — не принято ничего. */
int emu_audio_push(const int16_t *pcm, unsigned frames);

/* Статистика для экрана отладки (underrun/overrun считает слой). */
void emu_audio_stats(uint32_t *underrun, uint32_t *overrun);

/* Включить/выключить звуковой поток (должно звать ядро при старте/выходе). */
void emu_audio_start(void);
void emu_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_IO_H */