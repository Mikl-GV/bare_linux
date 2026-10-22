#ifndef SYSTEMS_H
#define SYSTEMS_H

#include <stddef.h>   /* NULL */

/* Реестр ретро-систем для меню MultiTool.
 * Состав и «Потянет H3» — из resources/emulation_h3_v2.xlsx.
 * dir — папка ромов системы в /roms/ на SD (NULL = у системы нет папки,
 * ромы не читаются). builtin = 1 — система работает без рома с SD
 * (BIOS/встроенный ROM в прошивке). */

enum {
    GROUP_PORTABLE = 0,
    GROUP_CONSOLE,
    GROUP_ARCADE,
    GROUP_COMPUTER,
    GROUP_COUNT
};

enum {
    STATUS_PLANNED = 0,     /* Низкая/Средняя «Потянет H3» */
    STATUS_READY,           /* Полная/Хорошая — можно делать */
    STATUS_COUNT
};

struct system_entry {
    const char *id;
    const char *name;
    const char *dir;         /* папка в /roms/ на SD, NULL = нет */
    int group;
    int status;              /* STATUS_READY / STATUS_PLANNED */
    int builtin;             /* 1 = работает без рома с SD */
};

#define SYS(id, name, group_, status_, builtin_) \
    { id, name, NULL, group_, status_, builtin_ }
#define SYS_DIR(id, name, dir_, group_, status_, builtin_) \
    { id, name, dir_, group_, status_, builtin_ }

static const struct system_entry systems[] = {
    /* ---- Портативки ---- */
    SYS("portfolio",    "Atari Portfolio (1989)",         GROUP_PORTABLE, STATUS_PLANNED, 0),
    SYS_DIR("gameboy",  "Game Boy / Game Boy Color", "gb", GROUP_PORTABLE, STATUS_READY, 0),
    SYS_DIR("gba",      "Game Boy Advance",           "gba", GROUP_PORTABLE, STATUS_READY, 0),
    SYS("lynx",         "Atari Lynx (1989)",              GROUP_PORTABLE, STATUS_READY, 0),
    SYS_DIR("gamegear", "Sega Game Gear",            "gamegear", GROUP_PORTABLE, STATUS_READY, 0),
    SYS_DIR("ngp",      "Neo Geo Pocket / Color",     "ngp", GROUP_PORTABLE, STATUS_READY, 0),
    SYS("wonderswan",   "WonderSwan / Color",             GROUP_PORTABLE, STATUS_READY, 0),
    SYS("tandy100",     "Tandy Model 100 / PC-8201",      GROUP_PORTABLE, STATUS_PLANNED, 0),
    SYS("hp200lx",      "HP 200LX / Poqet PC",            GROUP_PORTABLE, STATUS_READY, 0),

    /* ---- Консоли ---- */
    SYS("a2600",        "Atari 2600 (1977)",              GROUP_CONSOLE, STATUS_READY, 0),
    SYS_DIR("nes",      "NES / Famicom",              "nes", GROUP_CONSOLE, STATUS_READY, 0),
    SYS("sg1000",       "SG-1000",                         GROUP_CONSOLE, STATUS_READY, 0),
    SYS("atari8",       "Atari 8-bit / XEGS",             GROUP_CONSOLE, STATUS_READY, 0),
    SYS("coleco",       "ColecoVision",                    GROUP_CONSOLE, STATUS_READY, 0),
    SYS("vectrex",      "Vectrex (1982)",                  GROUP_CONSOLE, STATUS_READY, 0),
    SYS("a7800",        "Atari 7800",                      GROUP_CONSOLE, STATUS_READY, 0),
    SYS_DIR("sms",      "Sega Master System",          "sms", GROUP_CONSOLE, STATUS_READY, 0),
    SYS("c64",          "Commodore 64",                    GROUP_CONSOLE, STATUS_READY, 0),
    SYS("megadrive",    "Sega Genesis / Mega Drive",       GROUP_CONSOLE, STATUS_READY, 0),
    SYS_DIR("pce",      "PC Engine / TurboGrafx-16", "pce", GROUP_CONSOLE, STATUS_READY, 0),
    SYS("neogeo",       "Neo Geo",                         GROUP_CONSOLE, STATUS_PLANNED, 0),
    SYS_DIR("snes",     "Super Nintendo",              "snes", GROUP_CONSOLE, STATUS_PLANNED, 0),
    SYS("3do",          "3DO",                             GROUP_CONSOLE, STATUS_PLANNED, 0),
    SYS("psx",          "PlayStation",                     GROUP_CONSOLE, STATUS_PLANNED, 0),
    SYS("n64",          "Nintendo 64",                     GROUP_CONSOLE, STATUS_PLANNED, 0),
    SYS("saturn",       "Sega Saturn",                     GROUP_CONSOLE, STATUS_PLANNED, 0),

    /* ---- Аркады ---- */
    SYS("invaders",     "Space Invaders (1978)",           GROUP_ARCADE, STATUS_READY, 0),
    SYS("pacman",       "Pac-Man (1980)",                  GROUP_ARCADE, STATUS_READY, 0),
    SYS("galaga",       "Galaxian / Galaga",               GROUP_ARCADE, STATUS_READY, 0),
    SYS("dkong",        "Donkey Kong",                     GROUP_ARCADE, STATUS_READY, 0),
    SYS("segasys1",     "Sega System 1/2",                 GROUP_ARCADE, STATUS_READY, 0),
    SYS("cps1",         "Capcom CPS1 (SF II)",             GROUP_ARCADE, STATUS_READY, 0),
    SYS_DIR("segasys",  "Sega System 16", "segasys",       GROUP_ARCADE, STATUS_READY, 0),
    SYS_DIR("toaplan",  "Toaplan / Cave (shmup)", "toaplan", GROUP_ARCADE, STATUS_READY, 0),
    SYS("cps2",         "Capcom CPS2",                     GROUP_ARCADE, STATUS_PLANNED, 0),
    SYS("taito_f3",     "Taito F3 (Darius Gaiden)",        GROUP_ARCADE, STATUS_PLANNED, 0),
    SYS("model1",       "Sega Model 1/2 (VF)",             GROUP_ARCADE, STATUS_PLANNED, 0),

    /* ---- Компьютеры ---- */
    SYS_DIR("zxspectrum", "ZX Spectrum 48K/128K", "zxspectrum", GROUP_COMPUTER, STATUS_READY, 1),
    SYS("msx",          "MSX / MSX2",                      GROUP_COMPUTER, STATUS_READY, 1),
    SYS("cpc",          "Amstrad CPC",                     GROUP_COMPUTER, STATUS_READY, 0),
    SYS("pcw",          "Amstrad PCW",                     GROUP_COMPUTER, STATUS_READY, 0),
    SYS("apple2",       "Apple II / II+ / IIe",            GROUP_COMPUTER, STATUS_READY, 0),
    SYS("bbcmicro",     "BBC Micro / Master",              GROUP_COMPUTER, STATUS_READY, 0),
    SYS("atarist",      "Atari ST",                        GROUP_COMPUTER, STATUS_READY, 0),
    SYS("amiga",        "Amiga 500/600",                   GROUP_COMPUTER, STATUS_READY, 0),
    SYS("macplus",      "Macintosh Plus",                  GROUP_COMPUTER, STATUS_READY, 0),
    SYS("x68000",       "Sharp X68000",                    GROUP_COMPUTER, STATUS_READY, 0),
    SYS("pcxt",         "PC XT / PCjr / Tandy 1000",       GROUP_COMPUTER, STATUS_READY, 0),
    SYS("t1100",        "Toshiba T1100 / T1100 Plus",      GROUP_COMPUTER, STATUS_READY, 0),
    SYS("ms1504",       "Электроника МС 1504",             GROUP_COMPUTER, STATUS_READY, 1),
    SYS("ql",           "Sinclair QL",                     GROUP_COMPUTER, STATUS_READY, 0),
    SYS("pc88",         "NEC PC-8801",                     GROUP_COMPUTER, STATUS_PLANNED, 0),
    SYS("pc98",         "NEC PC-9801",                     GROUP_COMPUTER, STATUS_PLANNED, 0),
    SYS("pc286",        "PC AT (286)",                     GROUP_COMPUTER, STATUS_PLANNED, 0),
    SYS("pc386",        "PC 386SX / 486SX",                GROUP_COMPUTER, STATUS_PLANNED, 0),
    SYS("fmtowns",      "FM Towns",                        GROUP_COMPUTER, STATUS_PLANNED, 0),
    SYS("iigs",         "Apple IIGS",                      GROUP_COMPUTER, STATUS_READY, 0),
    SYS_DIR("bk0010",   "БК-0010 / БК-0011М",        "bk0010", GROUP_COMPUTER, STATUS_READY, 1),
    SYS("uknc",         "УКНЦ / Корвет",                   GROUP_COMPUTER, STATUS_READY, 0),
    SYS("radio86rk",    "Радио-86РМ / Специалист",         GROUP_COMPUTER, STATUS_READY, 0),
    SYS("agat",         "Агат",                            GROUP_COMPUTER, STATUS_READY, 0),
    SYS("osborne",      "Osborne 1 / Kaypro (CP/M)",       GROUP_COMPUTER, STATUS_READY, 0),
    SYS("ti83",         "TI-83 и старые калькуляторы",     GROUP_COMPUTER, STATUS_READY, 0),
};

#define NUM_SYSTEMS ((int)(sizeof(systems) / sizeof(systems[0])))

/* Папка ромов системы (dir или id). */
static const char *system_rom_dir(int idx)
{
    return systems[idx].dir ? systems[idx].dir : systems[idx].id;
}

#endif /* SYSTEMS_H */