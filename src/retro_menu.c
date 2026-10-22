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
#include "version.h"

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
static int g_settings_sel = 0;           /* 0=Transfer, 1=Sound test */
static int g_sound_on = 0;
static int g_sound_freq = 440;
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

    int maxp = sys_max_rows();
    if (g_sel >= g_row_count) g_sel = g_row_count - 1;
    if (g_sel < 0) g_sel = 0;
    if (g_scroll > g_sel) g_scroll = g_sel;
    if (g_sel >= g_scroll + maxp) g_scroll = g_sel - maxp + 1;
    if (g_scroll < 0) g_scroll = 0;

    int y = SYSTEMS_TOP;
    for (int r = g_scroll; r < g_row_count && r < g_scroll + maxp; r++) {
        int sel = (r == g_sel);
        if (g_rows[r].type == RT_GROUP) {
            text_clipped(60, y, (g_rows[r].idx == GROUP_CONSOLE) ? "Consoles" :
                         (g_rows[r].idx == GROUP_ARCADE) ? "Arcade" :
                         (g_rows[r].idx == GROUP_COMPUTER) ? "Computers" : "Portable",
                         0x0044CCFF, 0x000D1A33);
            y += ROW_H;
        } else if (g_rows[r].type == RT_SYS) {
            int i = g_rows[r].idx;
            uint32_t clr = (systems[i].status == STATUS_READY)
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
    fb_text(60, SYSTEMS_BOT + 8, "A=select  B=back  START=exit", 0x00A0C0D0, 0x000D1A33);
    fb_text(FB_W - 8 * 11 - 8, FB_H - 8 - 4, "FW v" FW_VERSION_STR, 0x00505070, 0x000D1A33);
    fb_clean();
}

static void load_browser(void)
{
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
        fb_text(60, 140, "Folder is empty", 0x00FFAA00, 0x000D1A33);
        fb_text(60, 168, "Put ROM files here (B=back):", 0x00FFFFFF, 0x000D1A33);
        char path[FAT_NAME_LEN + 16];
        int pl = 0;
        const char *pfx = "/roms/";
        while (*pfx && pl < (int)sizeof(path) - 1) path[pl++] = *pfx++;
        for (const char *s = system_rom_dir(g_sys); *s && pl < (int)sizeof(path) - 1; s++) path[pl++] = *s;
        path[pl] = 0;
        text_clipped(60, 196, path, 0x00666666, 0x000D1A33);
    } else {
        int scroll = 0;
        if (g_browser_sel >= maxp) scroll = g_browser_sel - maxp + 1;
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
    fb_text(60, 190, "(заглушка, B=back)", 0x00FFFFFF, 0x000D1A33);
    fb_clean();
}

static void render_settings_help(void)
{
    fb_fill(0x000D1A33);
    fb_rect(0, 0, FB_W - 1, 64, 0x00336699);
    fb_text(8, 24, g_screen == RU_SETTINGS ? "Settings" : g_screen == RU_HELP ? "Help" : "About",
            0x00FFFFFF, 0x00336699);
    if (g_screen == RU_SETTINGS) {
        int y = 140;
        for (int i = 0; i < 2; i++) {
            int sel = (i == g_settings_sel);
            uint32_t clr = sel ? 0x00FFFF00 : 0x00FFFFFF;
            if (sel) fill_row(y, 0x00181818);
            fb_text(70, y, i == 0 ? "Transfer (WiFi to SD)" : "Sound test",
                    clr, sel ? 0x00181818 : 0x000D1A33);
            y += ROW_H;
        }
        fb_text(60, 210, "A=open  B=back", 0x00A0C0D0, 0x000D1A33);
    } else if (g_screen == RU_HELP) {
        fb_text(60, 140, "A=select  B=back  START=exit", 0x00FFFFFF, 0x000D1A33);
        fb_text(60, 170, "Choose system, then a ROM from SD", 0x00FFFFFF, 0x000D1A33);
        fb_text(60, 200, "Built-in systems run without SD", 0x00FFFFFF, 0x000D1A33);
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

static uint32_t wait_btn(void)
{
    uint32_t btns = pcf8574_read();
    uint32_t pressed = btns & ~g_prev;
    g_prev = btns;
    return pressed;
}

void retro_menu_run(void)
{
    build_rows();
    g_sel = 0;
    g_scroll = 0;
    g_screen = RU_SYSTEMS;
    g_placeholder_sel = 0;
    g_prev = 0;

    for (;;) {
        if (g_screen == RU_SYSTEMS) render_systems();
        else if (g_screen == RU_BROWSER) render_browser();
        else if (g_screen == RU_BUILTIN) render_builtin();
        else if (g_screen == RU_PLACEHOLDER) render_placeholder();
        else if (g_screen == RU_TRANSFER) render_transfer();
        else if (g_screen == RU_SOUND) render_sound();
        else render_settings_help();

        uint32_t p = wait_btn();
        if (p == 0) {
            wifi_layer_poll();
            udelay(10000);
            continue;
        }
        if (g_screen == RU_SYSTEMS) {
            if (p & BTN_UP)   { g_sel = (g_sel + g_row_count - 1) % g_row_count; }
            else if (p & BTN_DOWN) { g_sel = (g_sel + 1) % g_row_count; }
            else if (p & BTN_A) {
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
            } else if (p & BTN_START) {
                return;                 /* выход из меню */
            }
        } else if (g_screen == RU_BUILTIN) {
            if (p & BTN_UP)   { g_placeholder_sel = 0; }
            else if (p & BTN_DOWN) { g_placeholder_sel = 1; }
            else if (p & BTN_A) {
                if (g_placeholder_sel == 1) {
                    load_browser();
                    g_screen = RU_BROWSER;
                } else {
                    g_screen = RU_PLACEHOLDER;
                }
            } else if (p & BTN_B) {
                g_screen = RU_SYSTEMS;
            }
        } else if (g_screen == RU_BROWSER) {
            if (p & BTN_UP) {
                if (g_browser_n > 1)
                    g_browser_sel = (g_browser_sel == 0) ? g_browser_n - 1 : g_browser_sel - 1;
            } else if (p & BTN_DOWN) {
                if (g_browser_n > 1)
                    g_browser_sel = (g_browser_sel >= g_browser_n - 1) ? 0 : g_browser_sel + 1;
            } else if (p & BTN_A) {
                g_screen = RU_PLACEHOLDER;
            } else if (p & BTN_B) {
                g_screen = RU_SYSTEMS;
            }
        } else if (g_screen == RU_SETTINGS) {
            /* 0 = Transfer, 1 = Sound test; здесь же обработка. */
            if (p & BTN_DOWN) { g_settings_sel = 1; }
            else if (p & BTN_UP) { g_settings_sel = 0; }
            else if (p & BTN_A) {
                if (g_settings_sel == 0) {
                    g_screen = RU_TRANSFER;
                } else {
                    g_screen = RU_SOUND;
                }
            } else if (p & BTN_B) {
                g_screen = RU_SYSTEMS;
            }
        } else if (g_screen == RU_TRANSFER) {
            int n = h3_wifi_scan_ap_count();
            if (p & BTN_UP) {
                if (wifi_layer_started() && n > 0)
                    g_transfer_sel = (g_transfer_sel + n - 1) % n;
            } else if (p & BTN_DOWN) {
                if (wifi_layer_started() && n > 0)
                    g_transfer_sel = (g_transfer_sel + 1) % n;
            } else if (p & BTN_A) {
                if (!wifi_layer_started()) {
                    wifi_layer_start();
                } else if (n > 0) {
                    wifi_layer_connect(g_transfer_sel);
                }
            } else if (p & BTN_START) {
                if (!wifi_layer_started()) {
                    wifi_layer_start();
                } else {
                    wifi_layer_rescan();
                }
            } else if (p & BTN_B) {
                g_screen = RU_SETTINGS;
            }
        } else if (g_screen == RU_SOUND) {
            /* Тест звука: A=play/stop, B=back (из старого main.c). */
            if (p & BTN_A) {
                if (g_sound_on) { i2s_stop(); g_sound_on = 0; }
                else { i2s_play_freq(g_sound_freq); g_sound_on = 1; }
            } else if (p & BTN_B) {
                i2s_stop();
                g_sound_on = 0;
                g_screen = RU_SETTINGS;
            }
        } else {
            /* RU_PLACEHOLDER / HELP / ABOUT */
            if (p & BTN_B) {
                g_screen = (g_screen == RU_PLACEHOLDER) ? RU_BROWSER : RU_SYSTEMS;
            }
        }
    }
}