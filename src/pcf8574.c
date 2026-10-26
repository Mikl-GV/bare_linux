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
    return pcf8574_read_ex(0);
}

uint32_t pcf8574_read_ex(uint8_t *raw)
{
    uint8_t v = 0xFF;
    if (i2cbb_read(PCF8574_ADDR, &v, 1) != 0) {
        if (raw) *raw = 0xFF;          /* ошибка чтения — как «всё отпущено» */
        return 0;
    }
    if (raw) *raw = v;
    return (uint32_t)(~v) & 0xFFu;
}

int pcf8574_probe(uint8_t *raw)
{
    uint8_t v = 0xFF;
    int rc = i2cbb_read(PCF8574_ADDR, &v, 1);
    if (raw) *raw = v;
    return rc;      /* 0 = чип ответил ACK */
}