#ifndef PCF8574_H
#define PCF8574_H

#include <stdint.h>

/* Кнопки через PCF8574 (I2C-расширитель, адрес 0x27).
   Раскладка (кнопка замыкает ногу на GND, в PCF8574 это логический 0):
     P0 = Вверх      P4 = A
     P1 = Влево      P5 = B
     P2 = Вправо     P6 = Start
     P3 = Вниз       P7 = Select
   Возвращается битовая маска: 1 = кнопка НАЖАТА (инверсия чтения). */

#define PCF8574_ADDR 0x27u

#define BTN_UP     (1u << 0)
#define BTN_LEFT   (1u << 1)
#define BTN_RIGHT  (1u << 2)
#define BTN_DOWN   (1u << 3)
#define BTN_A      (1u << 4)
#define BTN_B      (1u << 5)
#define BTN_START  (1u << 6)
#define BTN_SELECT (1u << 7)

/* Инициализация: сконфигурировать все линии как входы (записать 0xFF). */
void pcf8574_init(void);

/* Прочитать состояния: возвращает маску нажатых кнопок. */
uint32_t pcf8574_read(void);

#endif /* PCF8574_H */