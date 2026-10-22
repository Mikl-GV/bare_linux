#include <string.h>
#include "fat.h"
#include "h3_sdc0.h"
#include "uart.h"

/* Читатель FAT32 (только чтение) для SD-карты.
 * Построен по образцу fat.c pico-retro (проверен на том же железе):
 *   - fat_init() сканирует MBR за типами 0x0B/0x0C/0x06, ищет раздел,
 *     в корне которого есть папка /roms (системный раздел пропускается);
 *   - каталоги: 8.3 + LFN (порядок LFN-фрагментов на диске);
 *   - чтение: цепочка кластеров через FAT, PIO-чтение секторов.
 * Запись не реализована (в этом заходе только чтение).
 *
 * Проект линкуется без libc: минимальные memcpy/strncpy/strncmp здесь
 * локальные (как в wpa_crypto.c). */

static void fat_memcpy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    while (n--) *d++ = *s++;
}

static int fat_strncmp(const char *a, const char *b, uint32_t n)
{
    while (n > 0 && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void fat_strncpy(char *d, const char *s, uint32_t n)
{
    while (n > 0 && *s) { *d++ = *s++; n--; }
    while (n > 0) { *d++ = 0; n--; }
}

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t fat_size16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
} fat_bpb_t;

static uint8_t  g_sector[512];
static uint32_t g_sec_per_cluster;
static uint32_t g_reserved;
static uint32_t g_num_fats;
static uint32_t g_fat_size;
static uint32_t g_root_cluster;
static uint32_t g_data_start;
static uint32_t g_total_clusters;
static uint32_t g_part_lba;     /* LBA начала FAT-раздела */

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t cluster_to_sector(uint32_t cl)
{
    return g_data_start + (cl - 2) * g_sec_per_cluster;
}

static uint32_t fat_next_cluster(uint32_t cl)
{
    if (cl < 2) return 0x0FFFFFFFu;
    uint32_t fat_off = cl * 4;
    uint32_t sec = g_part_lba + g_reserved + (fat_off / 512);
    if (h3_sdc0_read_sector(sec, g_sector) < 0) return 0x0FFFFFFFu;
    return le32(g_sector + (fat_off % 512)) & 0x0FFFFFFFu;
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
            if (h3_sdc0_read_sector(sec_off, g_sector) < 0) return done;
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
 * -1 = конец каталога. LFN-фрагменты копятся в lfn_buf (порядок на диске:
 * последний фрагмент идёт первым, затем в обратном порядке, затем 8.3). */
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
    int count = 0;
    char lfn[FAT_NAME_LEN] = {0};
    int lfn_len = 0;
    uint32_t guard = 0;
    uint32_t clim = chain_guard_limit();

    while (cl && cl < 0x0FFFFFF8u && guard++ < clim) {
        uint32_t sec = cluster_to_sector(cl);
        for (uint32_t s = 0; s < g_sec_per_cluster; s++) {
            if (h3_sdc0_read_sector(sec + s, g_sector) < 0) return count;
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

/* Поиск каталога по пути вида "/roms" или "/roms/<система>". */
static int path_lookup(const char *path, fat_entry_t *out)
{
    if (path[0] != '/') return -1;
    const char *p = path + 1;
    if (!*p) { out->first_cluster = g_root_cluster; out->size = 0; return 0; }
    if (fat_strncmp(p, "roms", 4) == 0) p += 4;
    if (*p == '/') p++;
    if (!*p) { out->first_cluster = g_root_cluster; out->size = 0; return 0; }
    /* одна подпапка (система) */
    char name[FAT_NAME_LEN];
    int ni = 0;
    while (*p && *p != '/' && ni < FAT_NAME_LEN - 1) name[ni++] = *p++;
    name[ni] = 0;
    fat_entry_t *dirs = g_scratch_dir;
    int n = read_dir(g_root_cluster, dirs, FAT_MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        if (dirs[i].size == 0 && name_eq(dirs[i].name, name)) {
            out->first_cluster = dirs[i].first_cluster;
            out->size = dirs[i].size;
            fat_strncpy(out->name, dirs[i].name, FAT_NAME_LEN - 1);
            out->name[FAT_NAME_LEN - 1] = 0;
            return 0;
        }
    }
    return -1;
}

/* Проверка: в корне раздела есть папка /roms. */
static int part_has_roms(uint32_t lba)
{
    if (h3_sdc0_read_sector(lba, g_sector) < 0) return 0;
    if (le16(g_sector + 510) != 0xAA55) return 0;
    if (le16(g_sector + 11) != 512) return 0;
    uint32_t rc = le32(g_sector + 44);
    uint32_t spc = g_sector[13];
    if (spc == 0) return 0;

    fat_entry_t *dirs = g_scratch_dir;
    int n = read_dir(rc, dirs, FAT_MAX_ENTRIES);
    for (int i = 0; i < n; i++)
        if (dirs[i].size == 0 && name_eq(dirs[i].name, "roms")) return 1;
    return 0;
}

int fat_init(void)
{
    if (h3_sdc0_read_sector(0, g_sector) < 0) return -1;

    /* Собрать FAT32/FAT16-разделы из MBR. */
    enum { MAX_PART = 4 };
    uint32_t plt_start[MAX_PART];
    int part_count = 0;
    for (int n = 0; n < 4; n++) {
        int off = 446 + n * 16;
        uint8_t type = g_sector[off + 4];
        if (type == 0x0B || type == 0x0C || type == 0x06) {
            uint32_t lba = le32(g_sector + off + 8);
            if (lba != 0 && part_count < MAX_PART)
                plt_start[part_count++] = lba;
        }
    }
    if (part_count == 0) return -1;

    /* Сначала ищем раздел с /roms (системный — первый по счёту — пропускаем,
       пока есть другие). */
    uint32_t part_lba = 0;
    int part_idx = -1;
    for (int pi = 1; pi < part_count; pi++) {
        uint32_t lba = plt_start[pi];
        if (h3_sdc0_read_sector(lba, g_sector) < 0) continue;
        if (le16(g_sector + 510) != 0xAA55) continue;
        if (le16(g_sector + 11) != 512) continue;
        if (g_sector[13] == 0) continue;
        if (part_has_roms(lba)) { part_lba = lba; part_idx = pi; break; }
    }
    if (part_idx < 0 && part_count > 1) { part_lba = plt_start[1]; part_idx = 1; }
    if (part_idx < 0) { part_lba = plt_start[0]; part_idx = 0; }

    /* Инициализация выбранного раздела. */
    if (h3_sdc0_read_sector(part_lba, g_sector) < 0) return -1;
    if (le16(g_sector + 510) != 0xAA55) return -1;
    if (le16(g_sector + 11) != 512) return -1;

    g_part_lba       = part_lba;
    g_sec_per_cluster = g_sector[13];
    g_reserved        = le16(g_sector + 14);
    g_num_fats        = g_sector[16];
    g_fat_size        = le32(g_sector + 36);
    g_root_cluster    = le32(g_sector + 44);

    if (!g_fat_size || g_fat_size == 0xFFFFFFFFu) return -1;
    if (g_sec_per_cluster == 0) return -1;
    if (g_num_fats == 0) return -1;

    g_data_start = part_lba + g_reserved + g_num_fats * g_fat_size;
    uint32_t total_sectors = le32(g_sector + 32);
    if (g_data_start >= total_sectors) return -1;
    g_total_clusters = (total_sectors - g_data_start) / g_sec_per_cluster;

    uart_print("fat: part lba=");
    uart_print_int((int)part_lba);
    uart_print(" sec/clu=");
    uart_print_int((int)g_sec_per_cluster);
    uart_print(" root_clu=");
    uart_print_int((int)g_root_cluster);
    uart_print("\r\n");
    return 0;
}

int fat_list(const char *dir, fat_entry_t *out, int max)
{
    fat_entry_t d;
    if (path_lookup(dir, &d) != 0) return -1;
    return read_dir(d.first_cluster, out, max);
}

int fat_find(const char *dir, const char *name, fat_entry_t *out)
{
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
    return (int)read_chain(f->first_cluster, offset, buf, len);
}