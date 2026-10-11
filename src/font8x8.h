#ifndef FONT8X8_H
#define FONT8X8_H

#include <stdint.h>

/* Полный ASCII-шрифт 8x8 (коды 0x20..0x7E, 95 символов).
   Стандартный Classic VGA font 8x8 (public domain, MIT).
   Каждая строка: бит 7 = ЛЕВЫЙ пиксель, бит 0 = правый. */

extern const uint8_t font8x8_basic[95][8];

/* Кириллический шрифт 8x8, 64 глифа в порядке кодовой точки Unicode.
   Индекс = (кодовая_точка - 0x410): индексы 0..31 = А..Я (U+0410..U+042F,
   заглавные), индексы 32..63 = а..я (U+0430..U+044F, строчные).
   Ё/ё опущены. Формат строки как в font8x8_basic (бит 7 = левый пиксель). */

extern const uint8_t font8x8_cyr[64][8];

#endif /* FONT8X8_H */