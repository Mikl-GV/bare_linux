#include "pcf8574.h"
#include "h3_i2c_bb.h"

/* PCF8574: чтение возвращает 8 бит (1 = линия HIGH, 0 = LOW/GND).
   Кнопки на GND -> нажатие = 0. Возвращаем маску нажатых (инверсную). */

void pcf8574_init(void)
{
    /* Все выходами в 1 (чтобы читать входами): запись 0xFF. */
    uint8_t all_hi = 0xFF;
    i2cbb_write(PCF8574_ADDR, &all_hi, 1);
}

uint32_t pcf8574_read(void)
{
    uint8_t v = 0xFF;
    if (i2cbb_read(PCF8574_ADDR, &v, 1) != 0)
        return 0;   /* ошибка чтения - считаем все отпущенными */
    return (uint32_t)(~v) & 0xFFu;
}