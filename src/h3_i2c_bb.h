#ifndef H3_I2C_BB_H
#define H3_I2C_BB_H

#include <stdint.h>

/* Битовый (bit-bang) I2C-мастер на GPIO. Не завязываемся на железный TWI -
   U-Boot мог его перенастроить. Пины выбираются при инициализации.

   Замечание: для кнопок PCF8574 используем PG8 (SDA) и PG9 (SCL),
   функции GPIO (0/1 выходы). */

/* Инициализация: ставит SCL/SDA как открытый коллектор (выход), линия в 1. */
void i2cbb_init(uint32_t scl_port, uint32_t scl_pin,
                uint32_t sda_port, uint32_t sda_pin);

/* Стандартные транзакции:
   - i2cbb_write(addr7, buf, len): пишет buf в slave.
   - i2cbb_read (addr7, buf, len): читает len байт от slave.
   Возвращают 0 при успехе, !=0 при ошибке (NACK/timeout). */
int i2cbb_write(uint8_t addr7, const uint8_t *buf, int len);
int i2cbb_read(uint8_t addr7, uint8_t *buf, int len);

#endif /* H3_I2C_BB_H */