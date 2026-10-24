#include "ports.h"
#include "h3_ccu.h"

/* Регистры порта: cfg0(0x00) cfg1(0x04) cfg2(0x08) cfg3(0x0C) data(0x10) */

static inline volatile uint32_t *cfg_reg(uint32_t port, uint32_t pin)
{
    return (volatile uint32_t *)(port + ((pin / 8) * 4));
}

void set_pin_mode(uint32_t port, uint32_t pin, uint32_t mode)
{
    volatile uint32_t *cfg = cfg_reg(port, pin);
    uint32_t shift = (pin % 8) * 4;
    uint32_t v = *cfg;
    v &= ~(0xFUL << shift);
    v |= (mode & 0xF) << shift;
    *cfg = v;
}

void set_pin_data(uint32_t port, uint32_t pin, uint32_t data)
{
    volatile uint32_t *ddr = (volatile uint32_t *)(port + 0x10);
    if (data)
        *ddr |= (1u << pin);
    else
        *ddr &= ~(1u << pin);
}

uint32_t get_pin_data(uint32_t port, uint32_t pin)
{
    volatile uint32_t *ddr = (volatile uint32_t *)(port + 0x10);
    return (*ddr >> pin) & 1u;
}

void set_pin_pull(uint32_t port, uint32_t pin, uint32_t pull)
{
    /* Всев run-time: PULL0 (0x1C) на пины 0..15, PULL1 (0x20) на 16..31. */
    volatile uint32_t *pull_reg = (volatile uint32_t *)(port + ((pin < 16) ? 0x1C : 0x20));
    uint32_t shift = (pin % 16) * 2;
    uint32_t v = *pull_reg;
    v &= ~(3u << shift);
    v |= (pull & 3u) << shift;
    *pull_reg = v;
}

/* Инициализация GPIO: включить такт always-on домена (APB0) для порта L
   и обычного PIO. Без этого PL10-светодиод не работает. */
void gpio_init(void)
{
    H3_APB0_CLK_GATING |= (1u << 0);   /* APB0 gating (R_PRCM) */
    __asm volatile("dsb" ::: "memory");
}

/* PL10 - пользовательский светодиод (always-on домен, порт L).
   Включить как выход и установить. */
void set_led(int on)
{
    set_pin_mode(PORTL, 10, 1);   /* output */
    set_pin_data(PORTL, 10, on ? 1 : 0);
}