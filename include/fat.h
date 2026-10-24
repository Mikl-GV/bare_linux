#ifndef FAT_H
#define FAT_H

#include <stdint.h>

/* Минимальный читатель/писатель FAT32 для SD-карты и USB-флешки.
 * Поиск раздела: сканируем MBR за типами 0x0B/0x0C (только FAT32),
 * выбираем раздел, в корне которого есть папка /roms; если такого нет —
 * первый валидный FAT32-раздел. Поддерживается: корневой каталог, подпапки
 * (в т.ч. /roms/<система>), 8.3 + LFN (чтение), цепочки кластеров,
 * чтение файлов, создание каталогов, запись файлов (имя 8.3 без LFN),
 * удаление файлов/папок. FAT16/FAT12 не поддерживаются.
 *
 * Каталоги: 8.3 + LFN (порядок LFN-фрагментов на диске); чтение файлов —
 * цепочка кластеров через FAT, PIO-чтение секторов. */

#define FAT_MAX_ENTRIES 128
#define FAT_NAME_LEN    128

typedef struct {
    uint32_t first_cluster;
    uint32_t size;
    char     name[FAT_NAME_LEN];
} fat_entry_t;

/* Инициализация: найти FAT32-раздел с /roms (или первый валидный FAT32),
 * разобрать BPB (bytes/sec, sec/clu, reserved, fats, fat_size, root_clu).
 * Возвращает 0 при успехе, -1 при ошибке. */
int fat_init(void);

/* Список записей каталога dir ("/roms" или "/roms/<система>").
 * Возвращает число записей (или -1 при ошибке). */
int fat_list(const char *dir, fat_entry_t *out, int max);

/* Найти файл/папку по имени в каталоге dir. 1 = найдено, 0 = нет. */
int fat_find(const char *dir, const char *name, fat_entry_t *out);

/* Прочитать len байт файла начиная с offset (не за конец файла).
 * Возвращает прочитано, -1 при ошибке. */
int fat_read_file(const fat_entry_t *f, uint32_t offset, uint8_t *buf, uint32_t len);

/* Разделяемый буфер каталога (меню/браузер/FAT не конфликтуют). */
fat_entry_t *fat_scratch(void);

/* Создать каталог name внутри parent_path ("" = корень, "roms" = /roms).
 * Нужно для посева отсутствующих папок систем на SD.
 * Возвращает 0 при успехе, -1 при ошибке. */
int fat_mkdir(const char *parent_path, const char *name);

/* Записать файл name в каталоге dir ("/roms/nes"). data — len байт.
 * Существующий файл перезаписывается. Имя пишется как 8.3 (LFN не пишем).
 * Возвращает 0 при успехе. */
int fat_write_file(const char *dir, const char *name,
                   const uint8_t *data, uint32_t len);

/* Удалить файл/папку name в каталоге dir ("/roms/nes").
 * Для папки освобождается только её цепочка; вложенное содержимое
 * не трогается. Возвращает 0 при успехе. */
int fat_delete(const char *dir, const char *name);

/* ---- Блочный источник: FAT читает/пишет сектора через функторы.
 * По умолчанию — SD (h3_sdc0_read/write_sector). Меню переключает на
 * USB-накопитель (usb_msd) только при неудаче SD. ---- */
typedef int (*fat_blk_read_t)(uint32_t lba, void *buf);
typedef int (*fat_blk_write_t)(uint32_t lba, const void *buf);

/* Установить источник секторов (NULL = вернуть к SD). */
void fat_set_block_source(fat_blk_read_t rd, fat_blk_write_t wr);

/* 1, если источник секторов переопределён (не SD, напр. USB-флешка). */
int fat_block_source_active(void);

#endif /* FAT_H */