#ifndef RETRO_MENU_H
#define RETRO_MENU_H

/* Главное меню ретро-консоли (HDMI).
 * Слои: список систем (systems.h) → браузер ромов системы из /roms/<система>
 * на SD → заглушка запуска эмулятора. Плюс служебные пункты Settings/Help/About.
 *
 * Работает на штатном framebuffer (fb_text/fb_rect из display.h) и кнопках
 * PCF8574 (BTN_UP/DOWN/A/B/START из pcf8574.h). */

/* Показать главное меню. Возвращает по выходу (ESC/назад). */
void retro_menu_run(void);

#endif /* RETRO_MENU_H */