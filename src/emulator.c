#include "emulator.h"
#include "mem.h"
#include "fat.h"
#include "h3_sdc0.h"
#include "pcf8574.h"
#include "uart.h"
#include "sys.h"

/* Таблица ядер: id системы -> фнция запуска. Слои регистрируют свои ядра
 * через emulator_register_core (обычно в .init-секции или перед запуском). */
#define EMU_MAX_CORES  32
static emu_core_fn g_cores[EMU_MAX_CORES];
static uint32_t    g_core_ids[EMU_MAX_CORES];
static int         g_core_count = 0;

void emulator_register_core(uint32_t sys_id, emu_core_fn fn)
{
    if (!fn || g_core_count >= EMU_MAX_CORES) return;
    g_core_ids[g_core_count]   = sys_id;
    g_cores[g_core_count]      = fn;
    g_core_count++;
}

/* Выход по долгому нажатию START: ядро зовёт в своём цикле. */
static uint32_t g_exit_hold_start = 0;

int emu_check_exit(void)
{
    uint32_t btns = pcf8574_read();
    uint32_t now = micros() / 1000;
    if (btns & BTN_START) {
        if (!g_exit_hold_start) g_exit_hold_start = now;
        if ((uint32_t)(now - g_exit_hold_start) >= EMU_EXIT_HOLD_MS)
            return 1;
    } else {
        g_exit_hold_start = 0;
    }
    return 0;
}

static emu_core_fn find_core(uint32_t sys_id)
{
    for (int i = 0; i < g_core_count; i++)
        if (g_core_ids[i] == sys_id) return g_cores[i];
    return 0;
}

/* Загрузить ром с SD в зону ROM целиком (fat_read_file по кластерам).
 * Здесь используем fat_find + fat_read_file в один проход. */
int emu_load_rom(const char *path, uint32_t *out_addr, uint32_t *out_size)
{
    if (!path || !out_addr || !out_size) return EMU_ERR_ROM;

    /* Найти файл в /roms/<система>/... — путь вида "roms/<dir>/name" или
       "/roms/<dir>/name": убираем ведущий слеш и ищем через fat_find. */
    const char *p = path;
    if (*p == '/') p++;
    fat_entry_t fe;
    /* Пройдём по компонентам пути (одна подпапка + файл). */
    const char *slash = 0;
    for (const char *q = p; *q; q++) if (*q == '/') slash = q;
    if (!slash) return EMU_ERR_ROM;
    char dir[FAT_NAME_LEN], name[FAT_NAME_LEN];
    uint32_t dl = (uint32_t)(slash - p);
    if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
    for (uint32_t i = 0; i < dl; i++) dir[i] = p[i];
    dir[dl] = 0;
    uint32_t nl = 0;
    for (const char *q = slash + 1; *q && nl < sizeof(name) - 1; q++) name[nl++] = *q;
    name[nl] = 0;

    char lookup[FAT_NAME_LEN + 8];
    lookup[0] = '/';
    uint32_t li = 1;
    for (uint32_t i = 0; dir[i] && li < sizeof(lookup) - 1; i++)
        lookup[li++] = (dir[i] == '/') ? '/' : dir[i];   /* уже с '/' */
    lookup[li] = 0;
    /* Ищем файл с полным путём "dir/name" (dir = "roms/<система>"). */
    if (fat_find(dir, name, &fe) == 0) return EMU_ERR_ROM;
    if (fe.size == 0) return EMU_ERR_ROM;

    /* Зона ROM. */
    uint8_t *dst = (uint8_t *)mem_rom_reserve();
    if (!dst) return EMU_ERR_MEM;
    uint32_t cap = mem_rom_capacity();
    if (fe.size > cap) { mem_rom_release(); return EMU_ERR_MEM; }
    uint32_t got = (uint32_t)fat_read_file(&fe, 0, dst, fe.size);
    if (got != fe.size) { mem_rom_release(); return EMU_ERR_ROM; }

    *out_addr = (uint32_t)(uintptr_t)dst;
    *out_size = fe.size;
    return EMU_OK;
}

/* Запустить эмулятор: резерв ROM-зоны, ром -> куча, вызов ядра, освобождение. */
int emulator_run(uint32_t sys_id, const char *rom_path, uint32_t rom_size)
{
    (void)rom_size;
    emu_core_fn core = find_core(sys_id);
    if (!core) return EMU_ERR_NO_CORE;

    /* Ром -> зона ROM. */
    uint32_t ro_addr = 0, ro_size = 0;
    int rc = emu_load_rom(rom_path, &ro_addr, &ro_size);
    if (rc != EMU_OK) return rc;

    /* Куча для слоя: берём из свободного (mem_alloc большой кусок). */
    uint32_t heap_size = mem_free_bytes() / 2;   /* половина свободного — запас */
    if (heap_size > 0x1000000u) heap_size = 0x1000000u;   /* не более 16 МБ кучи слоя */
    if (heap_size < 0x10000u) heap_size = 0x10000u;
    void *heap = mem_alloc(heap_size);
    if (!heap) { mem_rom_release(); return EMU_ERR_MEM; }

    struct emu_run run;
    run.rom_addr  = ro_addr;
    run.rom_size  = ro_size;
    run.sys_id    = sys_id;
    run.heap      = heap;
    run.heap_size = heap_size;

    uart_print("emu: start sys=");
    uart_print_int((int)sys_id);
    uart_print(" rom=0x");
    uart_print_hex(ro_addr);
    uart_print(" sz=");
    uart_print_int((int)ro_size);
    uart_print("\r\n");

    int result = core(&run);

    mem_free(heap);
    mem_rom_release();
    uart_print("emu: exit result=");
    uart_print_int(result);
    uart_print("\r\n");
    return (result == 0) ? EMU_OK : result;
}