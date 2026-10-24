#include "retro_menu.h"
#include "display.h"
#include "pcf8574.h"
#include "systems.h"
#include "fat.h"
#include "h3_sdc0.h"
#include "wifi_layer.h"
#include "wifi_net.h"
#include "h3_sdc.h"
#include "i2s.h"
#include "uart.h"
#include "sys.h"
#include "ports.h"
#include "version.h"
#include "emulator.h"
#include "usb_kbd.h"
#include "usb_msd.h"

/* Флаг «WiFi-слой активен» (пишет wifi_layer) — значок в шапке меню. */
extern int wifi_active;

/* Главное ретро-меню (HDMI). Работает на единственном framebuffer
 * (fb_text/fb_rect, 8x8 шрифт) и кнопках PCF8574.
 *
 * Экраны-состояния:
 *   SYSTEMS        — список систем по группам + Settings/Help/About;
 *   BROWSER        — ромы системы из /roms/<dir> на SD;
 *   BUILTIN_DLG    — «Встроенный ром / Загрузить с SD» для builtin-систем;
 *   PLACEHOLDER    — заглушка запуска эмулятора (ром с SD/встроенный);
 *   SETTINGS/HELP  — служебные тексты (подробно — задача 10).
 *
 * В каждой итерации цикла вызывается wifi_layer_poll() — сетевой слой
 * (DHCP/ARP/ping) продолжает жить, пока крутится меню. */

#define ROW_H      22
#define SYSTEMS_TOP 74
#define SYSTEMS_BOT (FB_H - 90)
#define MAX_ROWS   (NUM_SYSTEMS + GROUP_COUNT + 8)

enum { RU_SYSTEMS, RU_BROWSER, RU_BUILTIN, RU_PLACEHOLDER,
       RU_SETTINGS, RU_HELP, RU_ABOUT, RU_TRANSFER, RU_SOUND };

enum { RT_GROUP = 0, RT_SYS, RT_SETT, RT_HELP, RT_ABOUT };

struct row { int type; int idx; };       /* idx: система или группа */

static struct row g_rows[MAX_ROWS];
static int g_row_count = 0;
static int g_sel = 0;                    /* выбранная строка SYSTEMS */
static int g_scroll = 0;

/* Нет libc в проекте — локальные str-помощники (как в fat.c). */
static int fat_name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void form_cp(char *d, const char *s)
{
    int i = 0;
    while (s[i] && i < FAT_NAME_LEN - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int g_screen = RU_SYSTEMS;
static int g_sys = 0;                    /* выбранная система для BROWSER */
static int g_browser_sel = 0;
static int g_browser_n = 0;
static int g_placeholder_sel = 0;        /* 0=встроенный, 1=с SD */
static int g_transfer_sel = 0;           /* выбранная сеть в Transfer */
static int g_transfer_top = 0;           /* скролл списка сетей */
static int g_settings_sel = 0;           /* 0=Transfer, 1=Sound test, 2=Storage test */
#define STORE_TEST_NOT_RUN (-100)
static int g_store_test_rc = STORE_TEST_NOT_RUN;  /* результат Storage test */
static int g_sound_on = 0;
static int g_sound_freq = 440;
static int g_sd_ok = 0;              /* 0=SD не инициализирована, 1=ok, <0=ошибка */
/* Наличие папки системы на SD (индекс системного idx). Заполняется в
   sd_init_menu после посева. Нужно для цвета/доступности пункта. */
static uint8_t g_sys_has_dir[NUM_SYSTEMS];
/* Эмулятор системы реализован (0 = ещё нет). Пока все 0 — по мере
   написания ядер поднимаем флаг; пункт белеет только при папке+эмуляторе. */
static uint8_t g_sys_emu[NUM_SYSTEMS];
static uint32_t g_prev = 0;

static int sys_max_rows(void)
{
    int h = SYSTEMS_BOT - SYSTEMS_TOP;
    return h / ROW_H;
}

static void build_rows(void)
{
    g_row_count = 0;
    int cur_group = -1;
    for (int i = 0; i < NUM_SYSTEMS; i++) {
        if (systems[i].group != cur_group) {
            cur_group = systems[i].group;
            g_rows[g_row_count].type = RT_GROUP;
            g_rows[g_row_count].idx = cur_group;
            g_row_count++;
        }
        g_rows[g_row_count].type = RT_SYS;
        g_rows[g_row_count].idx = i;
        g_row_count++;
    }
    /* служебные */
    g_rows[g_row_count].type = RT_SETT; g_rows[g_row_count].idx = 0; g_row_count++;
    g_rows[g_row_count].type = RT_HELP;  g_rows[g_row_count].idx = 0; g_row_count++;
    g_rows[g_row_count].type = RT_ABOUT; g_rows[g_row_count].idx = 0; g_row_count++;
}

static void text_clipped(int x, int y, const char *s, uint32_t color, uint32_t bg)
{
    char buf[64];
    int n = 0;
    while (s[n] && n < 62) n++;
    for (int i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = 0;
    fb_text(x, y, buf, color, bg);
}

static void fill_row(int y, uint32_t color)
{
    fb_rect(50, y - 2, FB_W - 50, y + ROW_H, color);
}

static void render_systems(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    fb_text(8, 24, "MultiTool Retro  --  MENU", 0x00FFFFFF, 0x00336699);
    if (wifi_active)
        fb_text(FB_W - 8 * 9 - 12, 12, "WIFI ON", 0x0000FF00, 0x00336699);
    else
        fb_text(FB_W - 8 * 10 - 12, 12, "WIFI OFF", 0x00888888, 0x00336699);
    /* Статус SD-карты: справа под WIFI. */
    if (g_sd_ok == 1)
        fb_text(FB_W - 8 * 7 - 12, 30, "SD OK", 0x0000FF00, 0x00336699);
    else if (g_sd_ok < 0)
        fb_text(FB_W - 8 * 9 - 12, 30, "SD FAIL", 0x00FF4444, 0x00336699);
    else
        fb_text(FB_W - 8 * 10 - 12, 30, "SD ...", 0x00888888, 0x00336699);

    int maxp = sys_max_rows();
    if (g_sel >= g_row_count) g_sel = g_row_count - 1;
    if (g_sel < 0) g_sel = 0;
    /* Двусторонний скролл: курсор всегда в видимой области. */
    if (g_scroll > g_sel) g_scroll = g_sel;
    if (g_sel >= g_scroll + maxp) g_scroll = g_sel - maxp + 1;
    if (g_scroll < 0) g_scroll = 0;

    int y = SYSTEMS_TOP;
    for (int r = g_scroll; r < g_row_count && r < g_scroll + maxp; r++) {
        int sel = (r == g_sel);
        if (g_rows[r].type == RT_GROUP) {
            text_clipped(60, y, (g_rows[r].idx == GROUP_CONSOLE) ? "Consoles" :
                         (g_rows[r].idx == GROUP_ARCADE) ? "Arcade" :
                         (g_rows[r].idx == GROUP_COMPUTER) ? "Computers" :
                         (g_rows[r].idx == GROUP_LAPTOP) ? "Laptops / PDAs" : "Portable",
                         0x0044CCFF, 0x000D1A33);
            y += ROW_H;
        } else if (g_rows[r].type == RT_SYS) {
            int i = g_rows[r].idx;
            /* Белый только если папка системы есть на SD И эмулятор реализован.
               Иначе — серый (пункт не готов к запуску). */
            int playable = g_sys_has_dir[i] && g_sys_emu[i];
            uint32_t clr = playable
                         ? (sel ? 0x00FFFF00 : 0x00FFFFFF)
                         : (sel ? 0x00888800 : 0x00666666);
            if (sel) fill_row(y, 0x00202020);
            char buf[64];
            int n = 0;
            const char *nm = systems[i].name;
            while (nm[n] && n < 48) n++;
            for (int k = 0; k < n; k++) buf[k] = nm[k];
            int m = n;
            if (systems[i].builtin) { buf[m++] = ' '; buf[m++] = '['; buf[m++] = 'B'; buf[m++] = ']'; }
            /* [OK] — папка системы есть на SD (как в pico-retro). */
            if (g_sys_has_dir[i]) {
                buf[m++] = ' '; buf[m++] = '['; buf[m++] = 'O'; buf[m++] = 'K'; buf[m++] = ']';
            }
            buf[m] = 0;
            fb_text(70, y, buf, clr, sel ? 0x00202020 : 0x000D1A33);
            y += ROW_H;
        } else {
            const char *label = g_rows[r].type == RT_SETT ? "== Settings ==" :
                                g_rows[r].type == RT_HELP  ? "== Help ==" :
                                                             "== About ==";
            uint32_t clr = sel ? 0x00FFFF00 : 0x00AAAAAA;
            if (sel) fill_row(y, 0x00181818);
            fb_text(70, y, label, clr, sel ? 0x00181818 : 0x000D1A33);
            y += ROW_H;
        }
    }
    /* Описание выбранной системы — строка ПОД подсказкой (не налазит). */
    {
        int si = -1;
        if (g_sel >= 0 && g_sel < g_row_count && g_rows[g_sel].type == RT_SYS)
            si = g_rows[g_sel].idx;
        if (si >= 0 && systems[si].desc)
            text_clipped(60, SYSTEMS_BOT + 34, systems[si].desc, 0x0088CCBB, 0x000D1A33);
        else
            fb_text(60, SYSTEMS_BOT + 34, " ", 0x0088CCBB, 0x000D1A33);
    }
    fb_text(60, SYSTEMS_BOT + 8, "A=select  B=back  START=exit", 0x00A0C0D0, 0x000D1A33);
    fb_text(FB_W - 8 * 11 - 8, FB_H - 8 - 4, "FW v" FW_VERSION_STR, 0x00505070, 0x000D1A33);
    fb_clean();
}

static void load_browser(void)
{
    g_browser_n = 0;
    g_browser_sel = 0;
    /* Без карты не читаем FAT: каждый h3_sdc0_read_sector без карты упирается
       в таймаут SD-контроллера (~1,7 с), fat_list делает их десятки — вход
       в меню ромов «зависал». Сразу возвращаем пустой список — рендер
       покажет «No SD card!». */
    if (g_sd_ok != 1) return;
    const char *dir = system_rom_dir(g_sys);
    char path[FAT_NAME_LEN + 16];
    int pl = 0;
    const char *pfx = "/roms/";
    while (*pfx && pl < (int)sizeof(path) - 1) path[pl++] = *pfx++;
    for (const char *s = dir; *s && pl < (int)sizeof(path) - 1; s++) path[pl++] = *s;
    path[pl] = 0;

    fat_entry_t *list = fat_scratch();
    int n = fat_list(path, list, FAT_MAX_ENTRIES);
    if (n < 0) n = 0;
    /* сортировка по имени */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (fat_name_cmp(list[i].name, list[j].name) > 0) {
                fat_entry_t t;
                t.first_cluster = list[i].first_cluster;
                t.size = list[i].size;
                form_cp(t.name, list[i].name);
                list[i].first_cluster = list[j].first_cluster;
                list[i].size = list[j].size;
                form_cp(list[i].name, list[j].name);
                list[j].first_cluster = t.first_cluster;
                list[j].size = t.size;
                form_cp(list[j].name, t.name);
            }
    g_browser_n = n;
    g_browser_sel = 0;
}

static void render_browser(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    text_clipped(8, 24, systems[g_sys].name, 0x00FFFFFF, 0x00336699);

    fat_entry_t *list = fat_scratch();
    int n = g_browser_n;
    int maxp = (FB_H - 140) / ROW_H;
    if (g_browser_sel >= n) g_browser_sel = n - 1;
    if (g_browser_sel < 0) g_browser_sel = 0;

    if (n <= 0) {
        /* Без карты — явная подсказка; иначе папка пуста. */
        if (g_sd_ok != 1) {
            fb_text(60, 140, "No SD card!", 0x00FF4444, 0x000D1A33);
            fb_text(60, 168, "Insert SD card to load ROMs (B=back)", 0x00FFFFFF, 0x000D1A33);
        } else {
            fb_text(60, 140, "Folder is empty", 0x00FFAA00, 0x000D1A33);
            fb_text(60, 168, "Put ROM files here (B=back):", 0x00FFFFFF, 0x000D1A33);
        }
        char path[FAT_NAME_LEN + 16];
        int pl = 0;
        const char *pfx = "";
        while (*pfx && pl < (int)sizeof(path) - 1) path[pl++] = *pfx++;
        for (const char *s = system_rom_dir(g_sys); *s && pl < (int)sizeof(path) - 1; s++) path[pl++] = *s;
        path[pl] = 0;
        text_clipped(60, 196, path, 0x00666666, 0x000D1A33);
    } else {
        int scroll = 0;
        /* Двусторонний скролл: курсор всегда в видимой области. */
        if (g_browser_sel >= maxp) scroll = g_browser_sel - maxp + 1;
        if (g_browser_sel < scroll) scroll = g_browser_sel;
        if (scroll < 0) scroll = 0;
        int y = 90;
        for (int i = scroll; i < n && i < scroll + maxp; i++) {
            int sel = (i == g_browser_sel);
            uint32_t clr = sel ? 0x00FFFF00 : 0x00FFFFFF;
            if (sel) fill_row(y, 0x00181818);
            char buf[64];
            int k = 0;
            while (list[i].name[k] && k < 56) k++;
            for (int q = 0; q < k; q++) buf[q] = list[i].name[q];
            buf[k] = 0;
            fb_text(70, y, buf, clr, sel ? 0x00181818 : 0x000D1A33);
            y += ROW_H;
        }
    }
    fb_text(60, FB_H - 30, "A=load  B=back", 0x00A0C0D0, 0x000D1A33);
    fb_clean();
}

static void render_builtin(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    text_clipped(8, 24, systems[g_sys].name, 0x00FFFFFF, 0x00336699);
    int y = 150;
    for (int i = 0; i < 2; i++) {
        int sel = (i == g_placeholder_sel);
        uint32_t clr = sel ? 0x00FFFF00 : 0x00FFFFFF;
        if (sel) fill_row(y, 0x00181818);
        fb_text(70, y, i == 0 ? "Built-in ROM" : "Load ROM from SD",
                clr, sel ? 0x00181818 : 0x000D1A33);
        y += ROW_H;
    }
    fb_text(60, 260, "A=select  B=back", 0x00A0C0D0, 0x000D1A33);
    fb_clean();
}

static void render_placeholder(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    text_clipped(8, 24, systems[g_sys].name, 0x00FFFFFF, 0x00336699);
    fb_text(60, 150, "Emulator not implemented yet", 0x00FFAA00, 0x000D1A33);
    fb_text(60, 190, "(stub, B=back)", 0x00FFFFFF, 0x000D1A33);
    fb_clean();
}

static void render_settings_help(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    fb_text(8, 24, g_screen == RU_SETTINGS ? "Settings" : g_screen == RU_HELP ? "Help" : "About",
            0x00FFFFFF, 0x00336699);
    if (g_screen == RU_SETTINGS) {
        static const char *items[3] = {
            "Transfer (WiFi to SD)",
            "Sound test",
            "Storage test (write/delete)"
        };
        int y = 140;
        for (int i = 0; i < 3; i++) {
            int sel = (i == g_settings_sel);
            uint32_t clr = sel ? 0x00FFFF00 : 0x00FFFFFF;
            if (sel) fill_row(y, 0x00181818);
            fb_text(70, y, items[i], clr, sel ? 0x00181818 : 0x000D1A33);
            y += ROW_H;
        }
        if (g_store_test_rc != STORE_TEST_NOT_RUN) {
            if (g_store_test_rc == 0) {
                fb_text(70, y + 8, "Storage: OK (write/read/delete)", 0x0000FF00, 0x000D1A33);
            } else {
                fb_text(70, y + 8, "Storage: FAIL (see UART log)", 0x00FF4040, 0x000D1A33);
            }
        }
        fb_text(60, 250, "A=open/run  B=back", 0x00A0C0D0, 0x000D1A33);
    } else if (g_screen == RU_HELP) {
        fb_text(60, 140, "SD structure (static, no card read):", 0x00FFFFFF, 0x000D1A33);
        fb_text(60, 170, "/  (roms partition, SD)", 0x00A0C0D0, 0x000D1A33);
        fb_text(80, 190, "ROMS/", 0x00FFFFFF, 0x000D1A33);
        fb_text(100, 210, "sms/  snes/  pce/  ngp/  zxspectrum/", 0x00FFFFFF, 0x000D1A33);
        fb_text(100, 230, "gameboy/  gba/  gamegear/  lynx/  wonderswan/", 0x00FFFFFF, 0x000D1A33);
        fb_text(100, 250, "a2600/  a5200/  a7800/  nes/  megadrive/", 0x00FFFFFF, 0x000D1A33);
        fb_text(100, 270, "segasys/  toaplan/  cps1/  cps2/  cave/", 0x00FFFFFF, 0x000D1A33);
        fb_text(100, 290, "apple2/  msx/  cpc/  amiga/  atarist/  pcxt/", 0x00FFFFFF, 0x000D1A33);
        fb_text(80, 330, "System partition (0): boot.scr, not touched.", 0x00666666, 0x000D1A33);
        fb_text(80, 360, "Put ROM files into roms/<system>/. No card read here.", 0x00A0C0D0, 0x000D1A33);
    } else {
        fb_text(60, 140, "MultiTool H3 bare-metal", 0x00FFFFFF, 0x000D1A33);
        fb_text(60, 170, "FW " FW_VERSION_STR, 0x00FFFFFF, 0x000D1A33);
        fb_text(60, 200, "Orange Pi Lite (Allwinner H3)", 0x00FFFFFF, 0x000D1A33);
    }
    fb_text(60, FB_H - 30, "B=back", 0x00A0C0D0, 0x000D1A33);
    fb_clean();
}

/* Экран «Sound test»: выбор частоты (440/697/1209) + play/stop. */
static void render_sound(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    fb_text(8, 24, "Sound test", 0x00FFFFFF, 0x00336699);
    static const int freqs[3] = { 440, 697, 1209 };
    int y = 140;
    for (int i = 0; i < 3; i++) {
        int sel = (freqs[i] == g_sound_freq);
        uint32_t clr = sel ? 0x00FFFF00 : 0x00FFFFFF;
        if (sel) fill_row(y, 0x00181818);
        char buf[24];
        int k = 0;
        int v = freqs[i];
        if (v == 0) buf[k++] = '0';
        else { char t[8]; int c = 0; while (v > 0 && c < 6) { t[c++] = (char)('0' + v % 10); v /= 10; } while (c > 0) buf[k++] = t[--c]; }
        buf[k++] = ' '; buf[k++] = 'H'; buf[k++] = 'z'; buf[k] = 0;
        fb_text(70, y, buf, clr, sel ? 0x00181818 : 0x000D1A33);
        y += ROW_H;
    }
    fb_text(60, 230, g_sound_on ? "PLAYING...  (A=stop)" : "A=play  B=back",
            g_sound_on ? 0x00FFD700 : 0x00A0C0D0, 0x000D1A33);
    fb_clean();
}

/* Экран «Transfer»: WiFi-слой + список сетей (перенос из старого main.c).
   Доступ к SD по сети — задача следующего этапа; тут подключение к AP. */
static void render_transfer(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    fb_text(8, 24, "Transfer (WiFi)", 0x00FFFFFF, 0x00336699);
    if (wifi_layer_started())
        fb_text(60, 90, "WiFi layer: running", 0x0000FF00, 0x000D1A33);
    else
        fb_text(60, 90, "WiFi layer: idle", 0x00888888, 0x000D1A33);

    fb_text(60, 120, "Networks:", 0x00A0C0D0, 0x000D1A33);
    int n = h3_wifi_scan_ap_count();
    if (n <= 0) {
        fb_text(60, 146, "(scan not run yet - press A)", 0x00888888, 0x000D1A33);
    } else {
        const int VIS = 10;
        if (g_transfer_sel < 0) g_transfer_sel = 0;
        if (g_transfer_sel >= n) g_transfer_sel = n - 1;
        if (g_transfer_sel < g_transfer_top) g_transfer_top = g_transfer_sel;
        if (g_transfer_sel >= g_transfer_top + VIS) g_transfer_top = g_transfer_sel - VIS + 1;
        if (g_transfer_top < 0) g_transfer_top = 0;
        int max_top = (n > VIS) ? (n - VIS) : 0;
        if (g_transfer_top > max_top) g_transfer_top = max_top;
        for (int i = 0; i < VIS && (g_transfer_top + i) < n; i++) {
            int idx = g_transfer_top + i;
            struct h3_wifi_ap ap;
            if (h3_wifi_scan_ap_get(idx, &ap) != 0) break;
            uint32_t col = (idx == g_transfer_sel) ? 0x0000FF00 : 0x00FFFFFF;
            fb_text(32, 146 + i * 22, (idx == g_transfer_sel) ? ">" : " ", col, 0x000D1A33);
            fb_text(48, 146 + i * 22, ap.ssid, col, 0x000D1A33);
        }
        if (g_transfer_top > 0) fb_text(FB_W - 40, 146, "^", 0x00FFD700, 0x000D1A33);
        if ((g_transfer_top + VIS) < n) fb_text(FB_W - 40, 146 + (VIS - 1) * 22, "v", 0x00FFD700, 0x000D1A33);
    }

    /* IP-статус (OFFER/ACK). */
    {
        uint8_t ip[4], mk[4], gw[4];
        int have_ack = 0;
        if (net_dhcp_get_ip(ip, mk, gw, &have_ack)) {
            char lab[24];
            int j = 0;
            lab[j++] = 'I'; lab[j++] = 'P'; lab[j++] = ':'; lab[j++] = ' ';
            for (int s = 0; s < 4; s++) {
                char num[8];
                int k = 0;
                if (ip[s] == 0) num[k++] = '0';
                else {
                    int v = ip[s], t[4], c = 0;
                    while (v > 0 && c < 3) { t[c++] = v % 10; v /= 10; }
                    while (c > 0) num[k++] = (char)('0' + t[--c]);
                }
                num[k] = 0;
                for (int q = 0; num[q]; q++) lab[j++] = num[q];
                if (s < 3) lab[j++] = '.';
            }
            lab[j] = 0;
            fb_text(60, 390, lab, 0x0000FF00, 0x000D1A33);
            fb_text(60, 410, have_ack ? "DHCP ACK - online" : "DHCP OFFER - awaiting ACK",
                    0x00A0C0D0, 0x000D1A33);
        }
    }
    if (wifi_layer_started() && n > 0)
        fb_text(60, 440, "A=connect  START=rescan  B=back", 0x00A0C0D0, 0x000D1A33);
    else
        fb_text(60, 440, "A=start WiFi  B=back", 0x00A0C0D0, 0x000D1A33);
    fb_clean();
}

/* Автоповтор кнопок: первое нажатие возвращается сразу; при удержании
   дольше 1 с — повтор каждые 0,3 с (для навигации без частых нажатий). */
#define AR_HOLD_MS   1000u
#define AR_REPEAT_MS 300u
static uint32_t g_hold_start_ms = 0;    /* когда зажали (0 = не удерживаем) */
static uint32_t g_repeat_at_ms  = 0;    /* когда следующий повтор */

static uint32_t wait_btn(void)
{
    uint32_t btns = pcf8574_read();
    uint32_t pressed = btns & ~g_prev;
    g_prev = btns;
    uint32_t now = micros() / 1000;

    /* USB-клавиатура: нажатие клавиши = как зажатие физической кнопки
       (автоповтор внутри usb_kbd_poll). */
    uint32_t kbd = usb_kbd_poll();

    if (pressed) {
        /* зажали новую кнопку — обычное событие + начали отсчёт удержания */
        g_hold_start_ms = now;
        g_repeat_at_ms  = now + AR_HOLD_MS;
        return pressed;
    }
    if (kbd) {
        /* клавиатура: событие сразу; свой автоповтор, не смешиваем
           с удержанием физических кнопок */
        g_hold_start_ms = 0;
        g_repeat_at_ms  = 0;
        return kbd;
    }
    if (btns == 0) {
        g_hold_start_ms = 0;   /* отпустили всё */
        g_repeat_at_ms  = 0;
        return 0;
    }
    /* удерживаем одну из кнопок: после 1 с повтор каждые 0,3 с */
    if (g_hold_start_ms && (uint32_t)(now - g_hold_start_ms) >= AR_HOLD_MS
        && (uint32_t)(now - g_repeat_at_ms) >= AR_REPEAT_MS) {
        g_repeat_at_ms = now;
        return btns;   /* повтор той же кнопки */
    }
    return 0;
}

/* Индикатор жизни процессора: мигание PL10 каждые ~500 мс.
   Вызывается и из udelay (sys.c) — поэтому не static. */
static uint32_t g_led_last = 0;
static int g_led_state = 1;
void led_heartbeat(void)
{
    uint32_t now = micros();
    if ((uint32_t)(now - g_led_last) >= 500000) {
        g_led_last = now;
        g_led_state = !g_led_state;
        set_led(g_led_state);
    }
}

/* Включить слот SD: PA15 = EN_SD/LED_EN_SD, АКТИВНЫЙ УРОВЕНЬ 1.
   LED светится при логической 1 (используется карта); гаснет при 0. */
/* Инициализация SD при входе в меню: SDC0 + FAT32.
   PA15 (LED_EN_SD) здесь НЕ включается постоянно — он горит только на время
   операций с картой (токены SD0_LED_ON/OFF в h3_sdc0.c). */
static void sd_init_menu(void)
{
    struct sdc0_info info;
    uart_print("sd0: init...\r\n");
    if (h3_sdc0_init(&info) != 0) {
        uart_print("sd0: FAIL\r\n");
        /* SD нет — пробуем USB-флешку (если накопитель активен после
           usb_hc_init): «нет SD → читаем /roms с флешки». Источник
           ставим ЗДЕСЬ, а не в usb_hc_init: иначе флешка перекрывала бы
           SD при её наличии. */
        if (usb_msd_active()) {
            fat_set_block_source(usb_msd_block_read, usb_msd_block_write);
            uart_print("fat: init (USB source)...\r\n");
            if (fat_init() == 0) {
                g_sd_ok = 1;   /* носитель есть (флешка) */
                goto roms_probe;
            }
            uart_print("fat: FAIL (usb)\r\n");
        }
        g_sd_ok = -1;
        return;
    }
    uart_print("fat: init...\r\n");
    if (fat_init() != 0) {
        uart_print("fat: FAIL\r\n");
        g_sd_ok = -2;   /* карта есть, но разметка/ФС не поднялась */
        return;
    }
    g_sd_ok = 1;

roms_probe:
    /* Только ОТМЕЧАЕМ наличие папок систем (без создания!):
       g_sys_has_dir нужен для [OK]/цвета. Папки систем лежат в roms/<система>
       (структура как в корне проекта) — ищем ВНУТРИ /roms, не в корне. */
    {
        fat_entry_t probe;
        for (int i = 0; i < NUM_SYSTEMS; i++) {
            const char *d = system_rom_dir(i);
            g_sys_has_dir[i] = 0;
            if (!d || !*d) continue;
            if (fat_find("roms", d, &probe)) g_sys_has_dir[i] = 1;
        }
    }
    uart_print("sd: ready\r\n");
}

/* IP-статус для перерисовки Transfer: 0 = адреса нет, иначе
   bit28=ACK, биты 0..23 = ip[0..3]. Сравниваем каждую итерацию — при смене
   (IP пришёл/сменился по DHCP) ставим dirty БЕЗ нажатия кнопки. */
static uint32_t transfer_ip_state(void)
{
    uint8_t ip[4], mk[4], gw[4];
    int ack = 0;
    uint32_t st = 0;
    if (net_dhcp_get_ip(ip, mk, gw, &ack))
        st = ((uint32_t)(ack ? 1 : 0) << 28)
           | ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16)
           | ((uint32_t)ip[2] << 8)  | (uint32_t)ip[3];
    return st;
}

void retro_menu_run(void)
{
    build_rows();
    g_sel = 0;
    /* Старт на ПЕРВОМ активном пункте (первая строка — заголовок группы). */
    while (g_sel < g_row_count && g_rows[g_sel].type == RT_GROUP) g_sel++;
    if (g_sel >= g_row_count) g_sel = 0;
    g_scroll = 0;
    g_screen = RU_SYSTEMS;
    g_placeholder_sel = 0;
    g_prev = 0;
    g_sd_ok = 0;
    uint32_t g_transfer_ip_st = 0;   /* последний считанный IP-статус Transfer */
    int dirty = 1;   /* рисовать только при изменении — иначе мерцание */
    sd_init_menu();  /* SDC0 + FAT32 перед браузером ромов; PA15 загорается
                        только на время опроса карты (токены в h3_sdc0.c) */
    set_led(1);

    for (;;) {
        /* IP мог прийти асинхронно (DHCP в wifi_layer_poll) — перерисуем
           экран Transfer без нажатия кнопки. */
        if (g_screen == RU_TRANSFER) {
            uint32_t st = transfer_ip_state();
            if (st != g_transfer_ip_st) { g_transfer_ip_st = st; dirty = 1; }
        }
        if (dirty) {
            if (g_screen == RU_SYSTEMS) render_systems();
            else if (g_screen == RU_BROWSER) render_browser();
            else if (g_screen == RU_BUILTIN) render_builtin();
            else if (g_screen == RU_PLACEHOLDER) render_placeholder();
            else if (g_screen == RU_TRANSFER) render_transfer();
            else if (g_screen == RU_SOUND) render_sound();
            else render_settings_help();
            dirty = 0;
        }

        led_heartbeat();   /* мигание PL10 — индикатор жизни процессора */
        uint32_t p = wait_btn();
        if (p == 0) {
            wifi_layer_poll();
            udelay(10000);
            continue;
        }
        if (g_screen == RU_SYSTEMS) {
            if (p & BTN_UP) {
                int next = g_sel;
                do { next = (next + g_row_count - 1) % g_row_count; } while (g_rows[next].type == RT_GROUP);
                g_sel = next;
                dirty = 1;
            } else if (p & BTN_DOWN) {
                int next = g_sel;
                do { next = (next + 1) % g_row_count; } while (g_rows[next].type == RT_GROUP);
                g_sel = next;
                dirty = 1;
            } else if (p & BTN_A) {
                int r = g_sel;
                if (g_rows[r].type == RT_SYS) {
                    g_sys = g_rows[r].idx;
                    if (systems[g_sys].builtin) {
                        g_placeholder_sel = 0;
                        g_screen = RU_BUILTIN;
                    } else {
                        load_browser();
                        g_screen = RU_BROWSER;
                    }
                } else if (g_rows[r].type == RT_SETT) {
                    g_screen = RU_SETTINGS;
                } else if (g_rows[r].type == RT_HELP) {
                    g_screen = RU_HELP;
                } else if (g_rows[r].type == RT_ABOUT) {
                    g_screen = RU_ABOUT;
                }
                dirty = 1;
            } else if (p & BTN_START) {
                return;                 /* выход из меню */
            }
        } else if (g_screen == RU_BUILTIN) {
            if (p & BTN_UP)   { g_placeholder_sel = 0; dirty = 1; }
            else if (p & BTN_DOWN) { g_placeholder_sel = 1; dirty = 1; }
            else if (p & BTN_A) {
                if (g_placeholder_sel == 1) {
                    load_browser();
                    g_screen = RU_BROWSER;
                } else {
                    g_screen = RU_PLACEHOLDER;
                }
                dirty = 1;
            } else if (p & BTN_B) {
                g_screen = RU_SYSTEMS;
                dirty = 1;
            }
        } else if (g_screen == RU_BROWSER) {
            if (p & BTN_UP) {
                if (g_browser_n > 1)
                    g_browser_sel = (g_browser_sel == 0) ? g_browser_n - 1 : g_browser_sel - 1;
                dirty = 1;
            } else if (p & BTN_DOWN) {
                if (g_browser_n > 1)
                    g_browser_sel = (g_browser_sel >= g_browser_n - 1) ? 0 : g_browser_sel + 1;
                dirty = 1;
            } else if (p & BTN_A) {
                /* Запуск эмулятора: диспетчер по id системы + ром из браузера.
                   Загружает ром с SD в RAM, вызывает ядро (если реализовано),
                   на выходе освобождает память. */
                if (g_browser_n > 0) {
                    fat_entry_t *list = fat_scratch();
                    char rompath[FAT_NAME_LEN + 16];
                    int k = 0;
                    const char *dir = system_rom_dir(g_sys);
                    rompath[k++] = '/';
                    rompath[k++] = 'r'; rompath[k++] = 'o'; rompath[k++] = 'm';
                    rompath[k++] = 's'; rompath[k++] = '/';
                    for (const char *s = dir; *s && k < (int)sizeof(rompath) - 1; s++)
                        rompath[k++] = *s;
                    rompath[k++] = '/';
                    for (const char *s = list[g_browser_sel].name;
                         *s && k < (int)sizeof(rompath) - 1; s++)
                        rompath[k++] = *s;
                    rompath[k] = 0;
                    int erc = emulator_run(g_sys, rompath, list[g_browser_sel].size);
                    if (erc != EMU_OK) {
                        /* ядро не реализовано или ошибка загрузки — короткий
                           лог, останемся в браузере (см. emulator.h) */
                        uart_print("emu: run rc=");
                        uart_print_int(erc);
                        uart_print("\r\n");
                    }
                    /* После выхода из эмулятора — вернуться в браузер. */
                    g_screen = RU_BROWSER;
                    dirty = 1;
                } else {
                    g_screen = RU_PLACEHOLDER;
                    dirty = 1;
                }
            } else if (p & BTN_B) {
                g_screen = RU_SYSTEMS;
                dirty = 1;
            }
        } else if (g_screen == RU_SETTINGS) {
            /* 0 = Transfer, 1 = Sound test, 2 = Storage test */
            if (p & BTN_DOWN) { g_settings_sel = (g_settings_sel + 1) % 3; dirty = 1; }
            else if (p & BTN_UP) { g_settings_sel = (g_settings_sel + 2) % 3; dirty = 1; }
            else if (p & BTN_A) {
                if (g_settings_sel == 0) {
                    g_screen = RU_TRANSFER;
                } else if (g_settings_sel == 1) {
                    g_screen = RU_SOUND;
                } else {
                    /* Запись/чтение/удаление WTEST.TXT на активном источнике
                       (SD, иначе USB-флешка): проверка fat_write_file/fat_delete. */
                    g_store_test_rc = fat_selftest_write();
                    uart_print("fat: selftest write rc=");
                    uart_print_int(g_store_test_rc);
                    uart_print("\r\n");
                }
                dirty = 1;
            } else if (p & BTN_B) {
                g_screen = RU_SYSTEMS;
                dirty = 1;
            }
        } else if (g_screen == RU_TRANSFER) {
            int n = h3_wifi_scan_ap_count();
            if (p & BTN_UP) {
                if (wifi_layer_started() && n > 0)
                    g_transfer_sel = (g_transfer_sel + n - 1) % n;
                dirty = 1;
            } else if (p & BTN_DOWN) {
                if (wifi_layer_started() && n > 0)
                    g_transfer_sel = (g_transfer_sel + 1) % n;
                dirty = 1;
            } else if (p & BTN_A) {
                if (!wifi_layer_started()) {
                    wifi_layer_start();   /* запуск WiFi = скан сетей (синхронно) */
                    dirty = 1;            /* список обновился — перерисовать экран */
                } else if (n > 0) {
                    wifi_layer_connect(g_transfer_sel);
                }
            } else if (p & BTN_START) {
                if (!wifi_layer_started()) {
                    wifi_layer_start();   /* запуск WiFi = скан сетей (синхронно) */
                    dirty = 1;
                } else {
                    wifi_layer_rescan();  /* повторный скан: обновить список */
                    dirty = 1;            /* перерисовать с новыми сетями */
                }
            } else if (p & BTN_B) {
                g_screen = RU_SETTINGS;
                dirty = 1;
            }
        } else if (g_screen == RU_SOUND) {
            /* UP/DOWN переключают частоту (440/697/1209), A=play/stop, B=back. */
            static const int freqs[3] = { 440, 697, 1209 };
            if (p & BTN_UP) {
                int idx = 0;
                while (idx < 2 && freqs[idx] != g_sound_freq) idx++;
                g_sound_freq = freqs[(idx + 2) % 3];   /* на шаг вверх */
                if (g_sound_on) { i2s_stop(); i2s_play_freq(g_sound_freq); }
                dirty = 1;
            } else if (p & BTN_DOWN) {
                int idx = 0;
                while (idx < 2 && freqs[idx] != g_sound_freq) idx++;
                g_sound_freq = freqs[(idx + 1) % 3];   /* на шаг вниз */
                if (g_sound_on) { i2s_stop(); i2s_play_freq(g_sound_freq); }
                dirty = 1;
            } else if (p & BTN_A) {
                if (g_sound_on) { i2s_stop(); g_sound_on = 0; }
                else { i2s_play_freq(g_sound_freq); g_sound_on = 1; }
                dirty = 1;
            } else if (p & BTN_B) {
                i2s_stop();
                g_sound_on = 0;
                g_screen = RU_SETTINGS;
                dirty = 1;
            }
        } else {
            /* RU_PLACEHOLDER / HELP / ABOUT */
            if (p & BTN_B) {
                g_screen = (g_screen == RU_PLACEHOLDER) ? RU_BROWSER : RU_SYSTEMS;
                dirty = 1;
            }
        }
    }
}