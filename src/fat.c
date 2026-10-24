#include <string.h>
#include "fat.h"
#include "h3_sdc0.h"
#include "uart.h"

/* Читатель/писатель FAT32 для SD-карты и USB-флешки (блочный источник
 * переключается fat_set_block_source).
 *   - fat_init() сканирует MBR за типами 0x0B/0x0C (только FAT32),
 *     выбирает раздел, в корне которого есть папка /roms; если такого нет —
 *     первый валидный FAT32-раздел;
 *   - каталоги: 8.3 + LFN (порядок LFN-фрагментов на диске);
 *   - чтение: цепочка кластеров через FAT, PIO-чтение секторов;
 *   - запись: fat_mkdir (посев папок), fat_write_file (запись/перезапись),
 *     fat_delete (удаление). Имена при записи — только 8.3 (LFN не пишем).
 *
 * Проект линкуется без libc: минимальные memcpy/strncpy/strncmp здесь
 * локальные (как в wpa_crypto.c). */

static void fat_memcpy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    while (n--) *d++ = *s++;
}

static void fat_strncpy(char *d, const char *s, uint32_t n)
{
    while (n > 0 && *s) { *d++ = *s++; n--; }
    while (n > 0) { *d++ = 0; n--; }
}

static uint8_t  g_sector[512];          /* буфер данных (не DMA-буфер USB) */
static uint32_t g_sec_per_cluster;
static uint32_t g_reserved;
static uint32_t g_num_fats;
static uint32_t g_fat_size;
static uint32_t g_root_cluster;
static uint32_t g_data_start;
static uint32_t g_total_clusters;
static uint32_t g_part_lba;     /* LBA начала FAT-раздела */

/* Блочный источник: по умолчанию SD (h3_sdc0). Можно переключить на
   USB-накопитель через fat_set_block_source (см. fat.h). */
static fat_blk_read_t  g_blk_rd = 0;
static fat_blk_write_t g_blk_wr = 0;

/* Источник по умолчанию — SD. Кастомный (USB-флешка) ставится
 * fat_set_block_source. Рекурсия здесь недопустима: забытый вызов blk_read
 * самого себя вешал FAT при отсутствии кастомного источника (W.9xx). */
static int blk_read(uint32_t lba, void *buf)
{
    if (g_blk_rd) return g_blk_rd(lba, buf);
    return h3_sdc0_read_sector(lba, buf);
}

static int blk_write(uint32_t lba, const void *buf)
{
    if (g_blk_wr) return g_blk_wr(lba, buf);
    return h3_sdc0_write_sector(lba, buf);
}

void fat_set_block_source(fat_blk_read_t rd, fat_blk_write_t wr)
{
    g_blk_rd = rd;
    g_blk_wr = wr;
}

int fat_block_source_active(void)
{
    return g_blk_rd != 0;
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void set_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t cluster_to_sector(uint32_t cl)
{
    return g_data_start + (cl - 2) * g_sec_per_cluster;
}

/* Кэш одного сектора FAT: read_chain ходит по одному кластеру = одно чтение
 * сектора FAT. Для файла в N кластеров это вдвое меньше обращений к носителю.
 * Инвалидируется при записи в FAT (fat_set_cluster). */
static uint32_t g_fat_cache_lba = 0xFFFFFFFFu;
static uint8_t  g_fat_cache[512];

static int fat_cache_read(uint32_t sec, uint8_t **out)
{
    if (g_fat_cache_lba != sec) {
        if (blk_read(sec, g_fat_cache) < 0) return -1;
        g_fat_cache_lba = sec;
    }
    *out = g_fat_cache;
    return 0;
}

static void fat_cache_invalidate(uint32_t sec)
{
    if (g_fat_cache_lba == sec) g_fat_cache_lba = 0xFFFFFFFFu;
}

static uint32_t fat_next_cluster(uint32_t cl)
{
    if (cl < 2) return 0x0FFFFFFFu;
    uint32_t fat_off = cl * 4;
    uint32_t sec = g_part_lba + g_reserved + (fat_off / 512);
    uint8_t *f;
    if (fat_cache_read(sec, &f) < 0) return 0x0FFFFFFFu;
    return le32(f + (fat_off % 512)) & 0x0FFFFFFFu;
}

static uint32_t chain_guard_limit(void)
{
    uint32_t l = g_total_clusters;
    return (l < 100000u) ? 100000u : l;
}

static uint32_t read_chain(uint32_t cl, uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (g_sec_per_cluster == 0) return 0;
    uint32_t cl_size = g_sec_per_cluster * 512;
    uint32_t skip = offset;
    uint32_t guard = 0;
    uint32_t clim = chain_guard_limit();
    while (skip >= cl_size) {
        if (guard++ >= clim) return 0;
        cl = fat_next_cluster(cl);
        if (cl >= 0x0FFFFFF8u) return 0;
        skip -= cl_size;
    }
    uint32_t done = 0;
    guard = 0;
    while (done < len && guard++ < clim) {
        if (cl == 0 || cl >= 0x0FFFFFF8u) break;
        uint32_t sec_off = cluster_to_sector(cl) + skip / 512;
        uint32_t in_sec  = skip % 512;
        uint32_t avail   = cl_size - skip;
        uint32_t want    = len - done;
        if (want > avail) want = avail;
        while (want > 0) {
            if (blk_read(sec_off, g_sector) < 0) return done;
            uint32_t n = 512 - in_sec;
            if (n > want) n = want;
            fat_memcpy(buf + done, g_sector + in_sec, n);
            done += n; want -= n;
            in_sec = 0;
            sec_off++;
        }
        cl = fat_next_cluster(cl);
        skip = 0;
    }
    return done;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Обработка одной записи каталога. 1 = файл/папка, 0 = промежуточная/мусор,
 * -1 = конец каталога. LFN-фрагменты копятся в lfn_buf. На диске LFN лежат
 * от старшего номера к младшему перед 8.3-записью; код кладёт фрагмент seq
 * в позицию (seq & 0x0F - 1)*13, т.е. собирает имя в правильном порядке. */
static int parse_dir_entry(const uint8_t *e, fat_entry_t *out, char *lfn, int *lfn_len)
{
    uint8_t attr = e[11];
    if (attr == 0x0F) {
        uint8_t seq = e[0];
        if (seq == 0xE5) { lfn[0] = 0; *lfn_len = 0; return 0; }
        int pos = (seq & 0x0F) - 1;
        if (pos >= 0 && pos < 20) {
            int off = pos * 13;
            if (off + 13 > FAT_NAME_LEN - 1) return 0;
            int idx = 0;
            for (int i = 0; i < 10; i += 2) {
                uint16_t c = (uint16_t)(e[1 + i] | ((uint16_t)e[2 + i] << 8));
                if (c == 0 || c == 0xFFFF) break;
                lfn[off + idx++] = (char)c;
            }
            for (int i = 0; i < 12; i += 2) {
                uint16_t c = (uint16_t)(e[14 + i] | ((uint16_t)e[15 + i] << 8));
                if (c == 0 || c == 0xFFFF) break;
                lfn[off + idx++] = (char)c;
            }
            for (int i = 0; i < 4; i += 2) {
                uint16_t c = (uint16_t)(e[28 + i] | ((uint16_t)e[29 + i] << 8));
                if (c == 0 || c == 0xFFFF) break;
                lfn[off + idx++] = (char)c;
            }
            if (off + idx > *lfn_len) *lfn_len = off + idx;
        }
        return 0;
    }

    uint8_t first = e[0];
    if (first == 0x00) return -1;
    if (first == 0xE5) { lfn[0] = 0; *lfn_len = 0; return 0; }

    uint8_t sa = attr;
    if (sa & (0x08 | 0x20)) {
        if ((sa & 0x08) && !(sa & 0x10)) { lfn[0] = 0; *lfn_len = 0; return 0; }
    }

    if (lfn[0]) {
        lfn[*lfn_len] = 0;
        fat_strncpy(out->name, lfn, FAT_NAME_LEN - 1);
        out->name[FAT_NAME_LEN - 1] = 0;
    } else {
        char n[9], x[4];
        fat_memcpy((uint8_t *)n, e, 8); n[8] = 0;
        fat_memcpy((uint8_t *)x, e + 8, 3); x[3] = 0;
        for (int i = 7; i >= 0 && n[i] == ' '; i--) n[i] = 0;
        for (int i = 2; i >= 0 && x[i] == ' '; i--) x[i] = 0;
        char *d = out->name;
        int pi = 0;
        for (int i = 0; n[i] && pi < FAT_NAME_LEN - 1; i++) d[pi++] = n[i];
        if (x[0]) {
            d[pi++] = '.';
            for (int i = 0; x[i] && pi < FAT_NAME_LEN - 1; i++) d[pi++] = x[i];
        }
        d[pi] = 0;
    }

    out->first_cluster = (uint32_t)le16(e + 26) | ((uint32_t)le16(e + 20) << 16);
    out->size = (sa & 0x10) ? 0 : le32(e + 28);
    lfn[0] = 0;
    *lfn_len = 0;
    return 1;
}

static int read_dir(uint32_t cl, fat_entry_t *out, int max)
{
    /* FAT не инициализирована (нет карты / сбой fat_init) — не делим на 0
       и не читаем неинициализированные g_*: просто пусто. */
    if (g_sec_per_cluster == 0 || g_data_start == 0) return 0;
    if (max <= 0) return 0;
    int count = 0;
    char lfn[FAT_NAME_LEN] = {0};
    int lfn_len = 0;
    uint32_t guard = 0;
    uint32_t clim = chain_guard_limit();

    while (cl && cl < 0x0FFFFFF8u && guard++ < clim) {
        uint32_t sec = cluster_to_sector(cl);
        for (uint32_t s = 0; s < g_sec_per_cluster; s++) {
            if (blk_read(sec + s, g_sector) < 0) return count;
            for (int i = 0; i < 512; i += 32) {
                const uint8_t *e = g_sector + i;
                int r = parse_dir_entry(e, &out[count], lfn, &lfn_len);
                if (r < 0) return count;
                if (r > 0) {
                    if (out[count].name[0] == '.' &&
                        (out[count].name[1] == 0 ||
                         (out[count].name[1] == '.' && out[count].name[2] == 0)))
                        continue;
                    count++;
                    if (count >= max) return count;
                }
            }
        }
        cl = fat_next_cluster(cl);
    }
    return count;
}

static fat_entry_t g_scratch_dir[FAT_MAX_ENTRIES];

fat_entry_t *fat_scratch(void)
{
    return g_scratch_dir;
}

/* Копировать запись каталога в out (без memcpy — проект без libc). */
static void entry_copy(fat_entry_t *out, const fat_entry_t *src)
{
    out->first_cluster = src->first_cluster;
    out->size = src->size;
    fat_strncpy(out->name, src->name, FAT_NAME_LEN - 1);
    out->name[FAT_NAME_LEN - 1] = 0;
}

/* Поиск каталога по пути. ""/"/" = корень раздела; "/roms/x" — папка x ВНУТРИ
   /roms (структура как в корне проекта: /roms/nes, /roms/sms и т.д.). */
static int path_lookup(const char *path, fat_entry_t *out)
{
    if (!path) return -1;
    if (path[0] == '/' ) path++;
    if (!*path) { out->first_cluster = g_root_cluster; out->size = 0; out->name[0] = 0; return 0; }
    /* Первый компонент. */
    char part[FAT_NAME_LEN];
    int ni = 0;
    while (*path && *path != '/' && ni < FAT_NAME_LEN - 1) part[ni++] = *path++;
    part[ni] = 0;
    fat_entry_t *dirs = g_scratch_dir;
    int n = read_dir(g_root_cluster, dirs, FAT_MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        if (dirs[i].size != 0) continue;
        if (!name_eq(dirs[i].name, part)) continue;
        /* Если компонентов больше — второй уровень внутри папки. */
        if (!*path) {   /* это и был каталог (первый уровень) */
            entry_copy(out, &dirs[i]);
            return 0;
        }
        if (*path != '/') return -1;
        path++;
        /* Второй компонент (система внутри /roms). subs алиасит g_scratch_dir:
           это безопасно, т.к. dirs[i] больше не нужен после read_dir ниже. */
        char sub[FAT_NAME_LEN];
        int si = 0;
        while (*path && *path != '/' && si < FAT_NAME_LEN - 1) sub[si++] = *path++;
        sub[si] = 0;
        fat_entry_t *subs = g_scratch_dir;
        int m = read_dir(dirs[i].first_cluster, subs, FAT_MAX_ENTRIES);
        for (int k = 0; k < m; k++) {
            if (subs[k].size == 0 && name_eq(subs[k].name, sub)) {
                entry_copy(out, &subs[k]);
                return 0;
            }
        }
        return -1;
    }
    return -1;
}

/* Разобрать BPB раздела lba и заполнить глобальную геометрию томом.
 * Только FAT32: BYTES_PER_SEC=512, FATSz16==0 (иначе это FAT12/16),
 * ROOT_CLU!=0. Возвращает 0 при успехе. */
static int fat_setup_vol(uint32_t lba)
{
    if (blk_read(lba, g_sector) < 0) return -1;
    if (le16(g_sector + 510) != 0xAA55) return -1;
    if (le16(g_sector + 11) != 512) return -1;
    uint8_t spc = g_sector[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) return -1;   /* степень двойки */
    if (le16(g_sector + 22) != 0) return -1;             /* FAT16-поле != 0 -> не FAT32 */
    uint32_t root_clu = le32(g_sector + 44);
    if (root_clu == 0) return -1;

    uint32_t reserved   = le16(g_sector + 14);
    uint32_t num_fats   = g_sector[16];
    uint32_t fat_size   = le32(g_sector + 36);
    uint32_t total_sectors = le32(g_sector + 32);
    if (total_sectors == 0) total_sectors = le16(g_sector + 19);
    if (num_fats == 0 || fat_size == 0 || fat_size == 0xFFFFFFFFu) return -1;

    uint32_t data_start = lba + reserved + num_fats * fat_size;
    if (data_start >= total_sectors) return -1;

    g_part_lba        = lba;
    g_sec_per_cluster = spc;
    g_reserved        = reserved;
    g_num_fats        = num_fats;
    g_fat_size        = fat_size;
    g_root_cluster    = root_clu;
    g_data_start      = data_start;
    g_total_clusters  = (total_sectors - data_start) / spc;
    return 0;
}

/* Проверка: в корне раздела (текущего контекста) есть папка /roms. */
static int part_has_roms(void)
{
    fat_entry_t *dirs = g_scratch_dir;
    int n = read_dir(g_root_cluster, dirs, FAT_MAX_ENTRIES);
    for (int i = 0; i < n; i++)
        if (dirs[i].size == 0 && name_eq(dirs[i].name, "roms")) return 1;
    return 0;
}

/* ================= Запись (посев папок, файлы, удаление) =================
   Запись — 8.3-имена без LFN (как в mkdir): файл/папка с длинным именем
   получит усечённое 8.3. Все записи в FAT идут через blk_write. */

/* 8.3 имя из строки (верхний регистр, base[<=8].ext[<=3]). */
static void make_short_name(const char *name, uint8_t *out83)
{
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    /* Выделяем расширение по последней точке (не в начале имени). */
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.' && p != name) dot = p;
    uint32_t base_len = dot ? (uint32_t)(dot - name) : 0;
    const char *ext = dot ? dot + 1 : 0;
    if (!dot) { uint32_t t = 0; while (name[t]) t++; base_len = t; }

    int n = 0, total = (int)base_len;
    int keep = (total > 8) ? 6 : total;
    for (int i = 0; name[i] && n < keep; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out83[n++] = (uint8_t)c;
    }
    if (total > 8) { out83[6] = '~'; out83[7] = '1'; }
    /* Расширение: до 3 символов после точки. */
    if (ext) {
        int m = 0;
        while (ext[m]) m++;
        int ke = m > 3 ? 3 : m;
        for (int i = 0; i < ke; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out83[8 + i] = (uint8_t)c;
        }
    }
}

/* Свободный кластер по FAT (с кэшем сектора FAT). */
static uint32_t find_free_cluster(void)
{
    for (uint32_t cl = 2; cl < 2 + g_total_clusters; cl++) {
        uint32_t fat_off = cl * 4;
        uint32_t sec = g_part_lba + g_reserved + (fat_off / 512);
        uint8_t *f;
        if (fat_cache_read(sec, &f) < 0) return 0;
        uint32_t v = le32(f + (fat_off % 512)) & 0x0FFFFFFFu;
        if (v == 0) return cl;
    }
    return 0;
}

/* Записать значение в FAT (обе копии); инвалидирует кэш FAT. */
static int fat_set_cluster(uint32_t cl, uint32_t val)
{
    uint32_t fat_off = cl * 4;
    uint32_t sec = g_part_lba + g_reserved + (fat_off / 512);
    uint32_t off = fat_off % 512;
    for (uint32_t copy = 0; copy < g_num_fats; copy++) {
        uint32_t s = sec + copy * g_fat_size;
        if (blk_read(s, g_sector) < 0) return -1;
        uint32_t v = le32(g_sector + off) & 0xF0000000u;
        v |= (val & 0x0FFFFFFFu);
        set_le32(g_sector + off, v);
        if (blk_write(s, g_sector) < 0) return -1;
        fat_cache_invalidate(s);
    }
    return 0;
}

/* Освободить цепочку кластера cl (все кластеры в 0). */
static void free_chain(uint32_t cl)
{
    uint32_t guard = 0;
    uint32_t clim = chain_guard_limit();
    while (cl >= 2 && cl < 0x0FFFFFF8u && guard++ < clim) {
        uint32_t nxt = fat_next_cluster(cl);
        fat_set_cluster(cl, 0);
        cl = nxt;
    }
}

/* Выделить цепочку под len байт; возвращает первый кластер (0 = ошибка).
 * Цепочка замыкается EOC. */
static uint32_t alloc_chain(uint32_t len)
{
    uint32_t cl_per = g_sec_per_cluster * 512;
    uint32_t need = (len + cl_per - 1) / cl_per;
    if (need == 0) need = 1;
    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < need; i++) {
        uint32_t cl = find_free_cluster();
        if (cl == 0) { free_chain(first); return 0; }
        if (fat_set_cluster(cl, 0x0FFFFFFFu) < 0) { free_chain(first); return 0; }
        if (prev) fat_set_cluster(prev, cl);
        else first = cl;
        prev = cl;
    }
    return first;
}

/* Пустая запись каталога (32 байта) в e. attr: 0x10 = каталог, 0x20 = файл. */
static void make_dir_entry(uint8_t *e, const char *name, uint32_t cluster,
                           uint32_t size, uint32_t attr)
{
    for (int i = 0; i < 32; i++) e[i] = 0;
    make_short_name(name, e);
    e[11] = (uint8_t)attr;
    set_le16(e + 20, (uint16_t)((cluster >> 16) & 0xFFFF));
    set_le16(e + 26, (uint16_t)(cluster & 0xFFFF));
    set_le32(e + 28, size);
}

/* Свободная запись в каталоге cl (пустая или удалённая). Если каталог полон —
 * выделяет новый кластер, дописывает к цепочке и обнуляет его. */
static int find_free_entry(uint32_t cl, uint32_t *sec_out, int *off_out, uint32_t *last_cl)
{
    uint32_t guard = 0;
    uint32_t clim = chain_guard_limit();
    uint32_t tail = 0;
    while (cl && cl < 0x0FFFFFF8u && guard++ < clim) {
        tail = cl;
        uint32_t base = cluster_to_sector(cl);
        for (uint32_t s = 0; s < g_sec_per_cluster; s++) {
            if (blk_read(base + s, g_sector) < 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                uint8_t first = g_sector[i];
                if (first == 0x00 || first == 0xE5) {
                    *sec_out = base + s;
                    *off_out = i;
                    if (last_cl) *last_cl = cl;
                    return 0;
                }
            }
        }
        cl = fat_next_cluster(cl);
    }
    if (!tail) return -1;
    /* Каталог полон: расширяем одним кластером. */
    tail = fat_next_cluster(tail);
    uint32_t ncl = find_free_cluster();
    if (ncl == 0) return -1;
    if (fat_set_cluster(ncl, 0x0FFFFFFFu) < 0) return -1;
    fat_set_cluster(tail, ncl);
    uint32_t ns = cluster_to_sector(ncl);
    uint8_t empty[512];
    for (int i = 0; i < 512; i++) empty[i] = 0;
    for (uint32_t s = 0; s < g_sec_per_cluster; s++)
        if (blk_write(ns + s, empty) < 0) return -1;
    *sec_out = ns;
    *off_out = 0;
    if (last_cl) *last_cl = ncl;
    return 0;
}

static int fat_delete_impl(const char *dir, const char *name);

/* Создать каталог name в родителе parent_path (например "" или "roms").
   Возвращает 0 при успехе, -1 при ошибке. */
int fat_mkdir(const char *parent_path, const char *name)
{
    fat_entry_t parent;
    if (path_lookup(parent_path, &parent) != 0) return -1;

    uint32_t new_cl = find_free_cluster();
    if (new_cl == 0) return -1;

    /* Инициализировать кластер: "." и "..", остальные секторы — нули.
       Для родителя-корня ".." должен указывать кластер 0 (спецификация). */
    uint32_t new_sec = cluster_to_sector(new_cl);
    uint8_t zero[512];
    for (int i = 0; i < 512; i++) zero[i] = 0;
    make_dir_entry(zero, ".", new_cl, 0, 0x10);
    uint32_t parent_cl = (parent.first_cluster == g_root_cluster)
                       ? 0 : parent.first_cluster;
    make_dir_entry(zero + 32, "..", parent_cl, 0, 0x10);
    if (blk_write(new_sec, zero) < 0) return -1;
    for (uint32_t s = 1; s < g_sec_per_cluster; s++) {
        uint8_t empty[512];
        for (int i = 0; i < 512; i++) empty[i] = 0;
        if (blk_write(new_sec + s, empty) < 0) return -1;
    }

    /* В FAT: новый кластер = конец цепочки. */
    if (fat_set_cluster(new_cl, 0x0FFFFFFFu) < 0) return -1;

    /* Запись в родительском каталоге. */
    uint32_t dummy_sec;
    int off;
    if (find_free_entry(parent.first_cluster, &dummy_sec, &off, 0) < 0) return -1;
    if (blk_read(dummy_sec, g_sector) < 0) return -1;
    make_dir_entry(g_sector + off, name, new_cl, 0, 0x10);
    if (blk_write(dummy_sec, g_sector) < 0) return -1;

    uart_print("fat: mkdir ");
    uart_print(parent_path[0] ? parent_path : "/");
    uart_print("/");
    uart_print(name);
    uart_print("\r\n");
    return 0;
}

/* Записать файл name в каталоге dir ("/roms/nes"). Данные — len байт.
 * Если файл существует — перезаписывается (старая цепочка освобождается).
 * Имя пишется как 8.3. Возвращает 0 при успехе. */
int fat_write_file(const char *dir, const char *name,
                   const uint8_t *data, uint32_t len)
{
    if (!dir || !name || !data) return -1;
    fat_entry_t d;
    if (path_lookup(dir, &d) != 0) return -1;
    if (!(d.size == 0 && d.name[0])) return -1;

    /* Существующий файл: удалить (освобождает цепочку и помечает запись). */
    fat_entry_t *dirs = g_scratch_dir;
    int n = read_dir(d.first_cluster, dirs, FAT_MAX_ENTRIES);
    int old_entry = -1;
    for (int i = 0; i < n; i++) {
        if (name_eq(dirs[i].name, name) && dirs[i].size != 0) { old_entry = i; break; }
    }
    if (old_entry >= 0) {
        if (fat_delete_impl(dir, name) != 0) return -1;
    }

    uint32_t new_cl = alloc_chain(len);
    if (new_cl == 0) return -1;

    /* Писать данные: идём по выделенной цепочке. */
    uint32_t cl = new_cl;
    uint32_t left = len;
    uint32_t guard = 0;
    while (cl >= 2 && cl < 0x0FFFFFF8u && guard++ < chain_guard_limit()) {
        uint32_t sec = cluster_to_sector(cl);
        for (uint32_t s = 0; s < g_sec_per_cluster && left > 0; s++) {
            for (int i = 0; i < 512; i++) g_sector[i] = 0;
            uint32_t n = 512;
            if (n > left) n = left;
            fat_memcpy(g_sector, data, n);
            data += n;
            left -= n;
            if (blk_write(sec + s, g_sector) < 0) { free_chain(new_cl); return -1; }
        }
        cl = fat_next_cluster(cl);
        if (left == 0) break;
    }
    if (left > 0) { free_chain(new_cl); return -1; }

    /* Запись каталога. */
    uint32_t dummy_sec;
    int off;
    if (find_free_entry(d.first_cluster, &dummy_sec, &off, 0) < 0) { free_chain(new_cl); return -1; }
    if (blk_read(dummy_sec, g_sector) < 0) { free_chain(new_cl); return -1; }
    make_dir_entry(g_sector + off, name, new_cl, len, 0x20);
    if (blk_write(dummy_sec, g_sector) < 0) { free_chain(new_cl); return -1; }
    return 0;
}

/* Внутренняя реализация удаления (используется fat_write_file для
 * перезаписи). См. fat_delete ниже. */
static int fat_delete_impl(const char *dir, const char *name)
{
    if (!dir || !name) return -1;
    fat_entry_t d;
    if (path_lookup(dir, &d) != 0) return -1;
    if (!(d.size == 0 && d.name[0])) return -1;

    /* Найти запись и её физический адрес (сектор/смещение). */
    uint32_t cl = d.first_cluster;
    uint32_t guard = 0;
    uint32_t clim = chain_guard_limit();
    while (cl && cl < 0x0FFFFFF8u && guard++ < clim) {
        uint32_t base = cluster_to_sector(cl);
        for (uint32_t s = 0; s < g_sec_per_cluster; s++) {
            if (blk_read(base + s, g_sector) < 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                uint8_t first = g_sector[i];
                if (first == 0x00) return -1;
                if (first == 0xE5) continue;
                fat_entry_t tmp;
                char lfn[FAT_NAME_LEN] = {0};
                int ll = 0;
                int r = parse_dir_entry(g_sector + i, &tmp, lfn, &ll);
                if (r != 1) continue;
                if (!name_eq(tmp.name, name)) continue;
                /* Пометить удалёнными 8.3-запись и LFN-фрагменты перед ней.
                   LFN-фрагменты идут на диске перед 8.3 (от N до 1);
                   идём от 8.3 вниз по сектору, пока attr==0x0F. */
                free_chain(tmp.first_cluster);
                int fl = i;
                for (int j = i - 32; j >= 0 && (g_sector[j + 11] == 0x0F); j -= 32)
                    fl = j;
                for (int k = fl; k <= i; k += 32)
                    g_sector[k] = 0xE5;
                if (blk_write(base + s, g_sector) < 0) return -1;
                return 0;
            }
        }
        cl = fat_next_cluster(cl);
    }
    return -1;
}

/* Удалить файл/папку name в каталоге dir. Пометка 0xE5 на 8.3-запись и
 * LFN-фрагменты, цепочка кластеров файла освобождается. Для папки
 * освобождает ТОЛЬКО её собственную цепочку (вложенное содержимое не
 * трогает — вызывающий отвечает за рекурсию). Возвращает 0 при успехе. */
int fat_delete(const char *dir, const char *name)
{
    return fat_delete_impl(dir, name);
}

int fat_init(void)
{
    /* Собрать FAT32-разделы из MBR (типы 0x0B/0x0C; FAT16 не поддерживаем). */
    enum { MAX_PART = 4 };
    uint32_t plt_start[MAX_PART];
    int part_count = 0;
    if (blk_read(0, g_sector) < 0) return -1;
    for (int n = 0; n < 4; n++) {
        int off = 446 + n * 16;
        uint8_t type = g_sector[off + 4];
        if (type == 0x0B || type == 0x0C) {
            uint32_t lba = le32(g_sector + off + 8);
            if (lba != 0 && part_count < MAX_PART)
                plt_start[part_count++] = lba;
        }
    }
    if (part_count == 0) return -1;

    /* Сначала ищем раздел с /roms, иначе первый валидный FAT32. */
    uint32_t part_lba = 0;
    int found_roms = -1, found_valid = -1;
    for (int pi = 0; pi < part_count; pi++) {
        if (fat_setup_vol(plt_start[pi]) != 0) continue;
        if (found_valid < 0) found_valid = pi;
        if (part_has_roms()) { found_roms = pi; break; }
    }
    if (found_roms >= 0) { part_lba = plt_start[found_roms]; }
    else if (found_valid >= 0) { part_lba = plt_start[found_valid]; }
    else return -1;

    /* Финализируем выбранный раздел. */
    if (fat_setup_vol(part_lba) != 0) return -1;

    uart_print("fat: part lba=");
    uart_print_int((int)part_lba);
    uart_print(" sec/clu=");
    uart_print_int((int)g_sec_per_cluster);
    uart_print(" root_clu=");
    uart_print_int((int)g_root_cluster);
    if (found_valid >= 0 && found_roms < 0)
        uart_print(" (no /roms)");
    uart_print("\r\n");
    return 0;
}

int fat_list(const char *dir, fat_entry_t *out, int max)
{
    if (g_sec_per_cluster == 0) return -1;   /* FAT не инициализирована */
    fat_entry_t d;
    if (path_lookup(dir, &d) != 0) return -1;
    return read_dir(d.first_cluster, out, max);
}

int fat_find(const char *dir, const char *name, fat_entry_t *out)
{
    if (g_sec_per_cluster == 0) return 0;   /* FAT не инициализирована */
    fat_entry_t d;
    if (path_lookup(dir, &d) != 0) return 0;
    fat_entry_t *dirs = g_scratch_dir;
    int n = read_dir(d.first_cluster, dirs, FAT_MAX_ENTRIES);
    for (int i = 0; i < n; i++)
        if (name_eq(dirs[i].name, name)) {
            out->first_cluster = dirs[i].first_cluster;
            out->size = dirs[i].size;
            fat_strncpy(out->name, dirs[i].name, FAT_NAME_LEN - 1);
            out->name[FAT_NAME_LEN - 1] = 0;
            return 1;
        }
    return 0;
}

int fat_read_file(const fat_entry_t *f, uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (!f) return -1;
    if (offset >= f->size) return 0;
    if (len > f->size - offset) len = f->size - offset;
    return (int)read_chain(f->first_cluster, offset, buf, len);
}