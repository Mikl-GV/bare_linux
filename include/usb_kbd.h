#ifndef USB_KBD_H
#define USB_KBD_H

#include <stdint.h>

/* USB HID-клавиатура (Full/Low-Speed, OHCI — любой из двух портов) для меню.
 * Паттерн эталона pico-retro usb_kbd.c/usb_ohci.c (interrupt-IN,
 * boot-протокол, HID-отчёт 8 байт).
 *
 * Поток:
 *   usb_kbd_init_on() — разбор config-дескриптора (enum делает usb_hc.c) +
 *                       SET_PROTOCOL(boot) + старт interrupt-IN;
 *   usb_kbd_poll()    — вызвать в меню: вернуть BTN_*-маску нажатий, 0 если
 *                       нажатий нет.
 * Маппинг: стрелки -> BTN_UP/DOWN/LEFT/RIGHT, Enter/Space -> BTN_A,
 * Esc -> BTN_B, Tab -> BTN_START, Backspace -> BTN_SELECT.
 */

/* Разобрать конфигурацию уже пройденного enum устройства (адрес 1) и, если
 * это boot-клавиатура, включить interrupt-IN. base — OHCI-порт, на котором
 * найдено устройство; cfg/cfg_len — полный config-дескриптор из enum.
 * Возвращает 0, если клавиатура готова, <0 — если HID-клавиатуры нет. */
int usb_kbd_init_on(uint32_t base, const uint8_t *cfg, uint32_t cfg_len);

/* Опрос: вернуть BTN_*-маску (одно нажатие/повтор) с клавиатуры. */
uint32_t usb_kbd_poll(void);

/* 1, если клавиатура активна (enum прошёл). */
int usb_kbd_active(void);

#endif /* USB_KBD_H */