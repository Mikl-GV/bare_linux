#ifndef FAT_H
#define FAT_H

#include <stdint.h>

/* Минимальный читатель FAT32 для SD-карты (раздел 1, именованный /roms).
 * Поиск раздела: сканируем MBR за типами 0x0B/0x0C/0x06 (FAT32/FAT16),
 * выбираем раздел, в корне которого есть папка /roms (системный раздел,
 * например с boot.scr, пропускается). Поддерживается чтение: корневой
 * каталог, подпапки 1 уровня (системы), 8.3 + LFN, цепочки кластеров.
 *
 * Запись, mkdir, delete НЕ реализованы (в этом заходе только чтение). */

#define FAT_MAX_ENTRIES 128
#define FAT_NAME_LEN    128

typedef struct {
    uint32_t first_cluster;
    uint32_t size;
    char     name[FAT_NAME_LEN];
} fat_entry_t;

/* Инициализация: найти FAT32-раздел с /roms (или первый не-system),
 * разобрать BPB (bytes/sec, sec/clu, reserved, fats, fat_size, root_clu).
 * Возвращает 0 при успехе, -1 при ошибке. */
int fat_init(void);

/* Список записей каталога dir ("/roms" или "/roms/<система>").
 * Возвращает число записей (или -1 при ошибке). */
int fat_list(const char *dir, fat_entry_t *out, int max);

/* Найти файл/папку по имени в каталоге dir. 1 = найдено, 0 = нет. */
int fat_find(const char *dir, const char *name, fat_entry_t *out);

/* Прочитать len байт файла начиная с offset. Возвращает прочитано, -1 при ошибке. */
int fat_read_file(const fat_entry_t *f, uint32_t offset, uint8_t *buf, uint32_t len);

/* Разделяемый буфер каталога (меню/браузер/FAT не конфликтуют). */
fat_entry_t *fat_scratch(void);

#endif /* FAT_H */