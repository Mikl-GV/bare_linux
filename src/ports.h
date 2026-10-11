#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>

/* PIO (GPIO). Порт занимает 0x24 байта: cfg0..cfg3, data. */

#define PIO_BASE 0x01C20800u

enum {
    PORTA = PIO_BASE + 0 * 0x24,
    PORTC = PIO_BASE + 2 * 0x24,
    PORTD = PIO_BASE + 3 * 0x24,
    PORTE = PIO_BASE + 4 * 0x24,
    PORTF = PIO_BASE + 5 * 0x24,
    PORTG = PIO_BASE + 6 * 0x24,
    PORTH = PIO_BASE + 7 * 0x24,
    PORTL = 0x01F02C00u
};

/* mode: 0=input, 1=output, 2..7=данные функции (Alt) */
void set_pin_mode(uint32_t port, uint32_t pin, uint32_t mode);
void set_pin_data(uint32_t port, uint32_t pin, uint32_t data);
uint32_t get_pin_data(uint32_t port, uint32_t pin);

/* Подтяжка: pull 0=none, 1=pull-up, 2=pull-down (регистр PULL порта). */
void set_pin_pull(uint32_t port, uint32_t pin, uint32_t pull);

/* PL10 - светодиод (always-on домен, порт L). */
void set_led(int on);

/* Инициализация GPIO (включает такт APB0 для порта L). */
void gpio_init(void);

#endif /* PORTS_H */