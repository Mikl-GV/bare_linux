#ifndef SYSTEMS_H
#define SYSTEMS_H

#include <stddef.h>   /* NULL */

/* Реестр ретро-систем для меню MultiTool.
 * Состав, группа и «Потянет H3» — из resources/emulation_h3_v2.xlsx.
 * dir — папка ромов системы в /roms/ на SD (NULL = у системы нет папки,
 * ромы не читаются). builtin = 1 — система работает без рома с SD
 * (BIOS/встроенный ROM в прошивке).
 * desc — краткое описание платформы: процесскор, ОЗУ, частота, год.
 *
 * Порядок в каждой группе — по году выпуска (сначала старше), затем
 * по имени (алфавит). Сортировка — см. также сортировку в меню. */

enum {
    GROUP_PORTABLE = 0,   /* портативные игровые консоли */
    GROUP_CONSOLE,        /* домашние консоли */
    GROUP_ARCADE,         /* аркадные автоматы */
    GROUP_COMPUTER,       /* компьютеры */
    GROUP_LAPTOP,         /* ноутбуки / карманные ПК / калькуляторы */
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
    const char *desc;        /* проц/ОЗУ/частота/год */
};

#define SYS(id, name, group_, status_, builtin_, desc_) \
    { id, name, NULL, group_, status_, builtin_, desc_ }
#define SYS_DIR(id, name, dir_, group_, status_, builtin_, desc_) \
    { id, name, dir_, group_, status_, builtin_, desc_ }

static const struct system_entry systems[] = {
    /* ========== Портативки (по году) ========== */
    SYS("gamegear",     "Sega Game Gear",              GROUP_PORTABLE, STATUS_READY, 0,
        "1990 , Z80 3.57 MHz , 8KB RAM , VDP 192x168"),
    SYS("gameboy",      "Game Boy / Game Boy Color",   GROUP_PORTABLE, STATUS_READY, 0,
        "1989 (GB) / 1998 (GBC) , SM83 4.19/8.4 MHz , 8KB+8KB VRAM"),
    SYS("lynx",         "Atari Lynx",                  GROUP_PORTABLE, STATUS_READY, 0,
        "1989 , 65SC02 4 MHz , 64KB RAM , Suzy blitter"),
    SYS("wonderswan",   "WonderSwan / Color",          GROUP_PORTABLE, STATUS_READY, 0,
        "1999 / 2000 , NEC V30 3.07 MHz , 64KB RAM"),
    SYS_DIR("ngp",      "Neo Geo Pocket / Color", "ngp", GROUP_PORTABLE, STATUS_READY, 0,
        "1998 / 1999 , TLCS900H 6.1 MHz , 12KB RAM , 224x144"),
    SYS_DIR("gba",      "Game Boy Advance",        "gba", GROUP_PORTABLE, STATUS_READY, 0,
        "2001 , ARM7TDMI 16.78 MHz , 256KB WRAM+96KB VRAM"),

    /* ========== Консоли (по году) ========== */
    SYS("a2600",        "Atari 2600",                  GROUP_CONSOLE, STATUS_READY, 0,
        "1977 , 6507 1.19 MHz , 128B RAM , TIA"),
    SYS_DIR("a5200",    "Atari 5200",              "a5200", GROUP_CONSOLE, STATUS_READY, 0,
        "1982 , 6502C 1.79 MHz , 16KB RAM , ANTIC+GTIA"),
    SYS("a7800",        "Atari 7800",                  GROUP_CONSOLE, STATUS_READY, 0,
        "1984 , 6502C 1.79 MHz , 4KB RAM , MARIA"),
    SYS("sg1000",       "SG-1000",                     GROUP_CONSOLE, STATUS_READY, 0,
        "1983 , Z80 3.58 MHz , 1-16KB , TMS9918"),
    SYS_DIR("nes",      "NES / Famicom",           "nes", GROUP_CONSOLE, STATUS_READY, 0,
        "1983 , 6502 1.79 MHz , 2KB+2KB VRAM , PPU"),
    SYS("coleco",       "ColecoVision",                GROUP_CONSOLE, STATUS_READY, 0,
        "1982 , Z80 3.58 MHz , 1KB+16KB VRAM , TMS9918"),
    SYS("vectrex",      "Vectrex",                     GROUP_CONSOLE, STATUS_READY, 0,
        "1982 , 68A09 1.5 MHz , 1KB , vector display"),
    SYS("atari8",       "Atari 8-bit / XEGS",          GROUP_CONSOLE, STATUS_READY, 0,
        "1979 , 6502 1.79 MHz , 48-64KB , ANTIC+GTIA"),
    SYS_DIR("sms",      "Sega Master System",      "sms", GROUP_CONSOLE, STATUS_READY, 0,
        "1985 , Z80 3.57 MHz , 8KB , VDP 256x192"),
    SYS("megadrive",    "Sega Genesis / Mega Drive",   GROUP_CONSOLE, STATUS_READY, 0,
        "1988 , 68000 7.6 MHz + Z80 , 64KB+8KB , YM2612"),
    SYS_DIR("pce",      "PC Engine / TurboGrafx-16","pce", GROUP_CONSOLE, STATUS_READY, 0,
        "1987 , HuC6280 7.16 MHz , 8KB , 2 layers"),
    SYS("c64",          "Commodore 64",                GROUP_CONSOLE, STATUS_READY, 0,
        "1982 , 6510 1 MHz , 64KB , VIC-II+SID"),
    SYS_DIR("snes",     "Super Nintendo",          "snes", GROUP_CONSOLE, STATUS_PLANNED, 0,
        "1990 , 65C816 3.58 MHz + SPC700 , 128KB , Mode7"),
    SYS("neogeo",       "Neo Geo",                     GROUP_CONSOLE, STATUS_PLANNED, 0,
        "1990 , 68000 12 MHz + Z80 , 64KB , big sprites"),
    SYS("3do",          "3DO",                         GROUP_CONSOLE, STATUS_PLANNED, 0,
        "1993 , ARM60 12.5 MHz , 2MB+1MB , CEL blitter"),
    SYS("psx",          "PlayStation",                 GROUP_CONSOLE, STATUS_PLANNED, 0,
        "1994 , R3000A 33.8 MHz , 2MB+1MB VRAM , GTE"),
    SYS("saturn",       "Sega Saturn",                 GROUP_CONSOLE, STATUS_PLANNED, 0,
        "1994 , 2x SH-2 28.6 MHz , 2MB , VDP1+2"),
    SYS("n64",          "Nintendo 64",                 GROUP_CONSOLE, STATUS_PLANNED, 0,
        "1996 , R4300i 93.75 MHz , 4MB RDRAM , RCP"),

    /* ========== Аркады (по году) ========== */
    SYS("invaders",     "Space Invaders",              GROUP_ARCADE, STATUS_READY, 0,
        "1978 , 8080 2 MHz , 8KB , discrete video"),
    SYS("pacman",       "Pac-Man",                     GROUP_ARCADE, STATUS_READY, 0,
        "1980 , Z80 3 MHz , 3KB , tilemap 36x28"),
    SYS("galaga",       "Galaxian / Galaga",           GROUP_ARCADE, STATUS_READY, 0,
        "1981 , Z80 x2 , 4KB , starfield"),
    SYS("dkong",        "Donkey Kong",                 GROUP_ARCADE, STATUS_READY, 0,
        "1981 , Z80 + 8035 sound , 4KB"),
    SYS("segasys1",     "Sega System 1/2",             GROUP_ARCADE, STATUS_READY, 0,
        "1983 , Z80 + SN76489 , tiles"),
    SYS_DIR("toaplan",  "Toaplan / Cave (shmup)","toaplan", GROUP_ARCADE, STATUS_READY, 0,
        "1986-95 , 68000 10-16 MHz , YM2610/OKI"),
    SYS("cps1",         "Capcom CPS1",                 GROUP_ARCADE, STATUS_READY, 0,
        "1988 , 68000 12 MHz + Z80 , YM2151+MSM6295"),
    SYS_DIR("segasys",  "Sega System 16",          "segasys", GROUP_ARCADE, STATUS_READY, 0,
        "1985 , 68000 10 MHz + Z80 , YM2151"),
    SYS("taito_f3",     "Taito F3",                    GROUP_ARCADE, STATUS_PLANNED, 0,
        "1992 , 68EC020 16 MHz , many sprites"),
    SYS("model1",       "Sega Model 1/2",              GROUP_ARCADE, STATUS_PLANNED, 0,
        "1992 , 68000 + 3D chips , geometry HW"),
    SYS("cps2",         "Capcom CPS2",                 GROUP_ARCADE, STATUS_PLANNED, 0,
        "1993 , 68000 16 MHz , QSound DSP"),
    SYS("fbneo",        "FB Neo (Arcade)",             GROUP_ARCADE, STATUS_PLANNED, 0,
        "2005 , multi-system emulator , arcade ROMsets"),
    SYS_DIR("cave",     "Cave (68K) STGs",         "cave", GROUP_ARCADE, STATUS_READY, 0,
        "1994 , 68000 12-16 MHz , bullet-hell shmups"),

    /* ========== Компьютеры (по году) ========== */
    SYS_DIR("atari800",  "Atari 8-bit (400/800/XL/XE)", "atari800", GROUP_COMPUTER, STATUS_READY, 0,
        "1979 , 6502 1.79 MHz , 48-64KB , ANTIC+GTIA"),
    SYS("apple2",       "Apple II / II+ / IIe",        GROUP_COMPUTER, STATUS_READY, 0,
        "1977 , 6502 1 MHz , 48-64KB , video from RAM"),
    SYS("bbcmicro",     "BBC Micro / Master",          GROUP_COMPUTER, STATUS_READY, 0,
        "1981 , 6502 2 MHz , 32KB , CRTC+ULA"),
    SYS("enterprise",   "Enterprise 64/128",           GROUP_COMPUTER, STATUS_READY, 0,
        "1985 , Z80 4 MHz , 64-128KB , Nick+Dave"),
    SYS("zxspectrum",   "ZX Spectrum 48K/128K",        GROUP_COMPUTER, STATUS_READY, 1,
        "1982 , Z80 3.5 MHz , 48/128KB , ULA 256x192"),
    SYS("c64",          "Commodore 64",                GROUP_COMPUTER, STATUS_READY, 0,
        "1982 , 6510 1 MHz , 64KB , VIC-II+SID 8580"),
    SYS("msx",          "MSX / MSX2",                  GROUP_COMPUTER, STATUS_READY, 1,
        "1983 , Z80 3.58 MHz , 64-128KB , V9938"),
    SYS("cpc",          "Amstrad CPC",                 GROUP_COMPUTER, STATUS_READY, 0,
        "1984 , Z80 4 MHz , 64-128KB , CRTC+GA"),
    SYS("ql",           "Sinclair QL",                 GROUP_COMPUTER, STATUS_READY, 0,
        "1984 , 68008 7.5 MHz , 128KB , ZX8301"),
    SYS("pcw",          "Amstrad PCW",                 GROUP_COMPUTER, STATUS_READY, 0,
        "1985 , Z80 4 MHz , 256KB , text mono"),
    SYS("atarist",      "Atari ST",                    GROUP_COMPUTER, STATUS_READY, 0,
        "1985 , 68000 8 MHz , 512KB-4MB , Shifter"),
    SYS("amiga",        "Amiga 500/600",               GROUP_COMPUTER, STATUS_READY, 0,
        "1985 , 68000 7 MHz , 512KB , Agnus+Paula"),
    SYS("macplus",      "Macintosh Plus",              GROUP_COMPUTER, STATUS_READY, 0,
        "1986 , 68000 8 MHz , 1MB , mono 512x342"),
    SYS("x68000",       "Sharp X68000",                GROUP_COMPUTER, STATUS_READY, 0,
        "1987 , 68000 10 MHz , 1-4MB , 32768 pal"),
    SYS("radio86rk",    "Radio-86RK / Specialist",     GROUP_COMPUTER, STATUS_READY, 0,
        "1986 , KR580VM80A (8080) 2 MHz , 32-64KB"),
    SYS("agat",         "Agat",                        GROUP_COMPUTER, STATUS_READY, 0,
        "1984 , 6502 1 MHz , 48-128KB , Apple II-clone"),
    SYS("pcxt",         "PC XT / PCjr / Tandy",        GROUP_COMPUTER, STATUS_READY, 0,
        "1983 , 8088 4.77-7.16 MHz , 256-640KB , CGA/MDA/TGA"),
    SYS_DIR("bk0010",   "BK-0010 / BK-0011M",     "bk0010", GROUP_COMPUTER, STATUS_READY, 1,
        "1985 , K1801VM2 (PDP-11) 4 MHz , 32-128KB"),
    SYS("uknc",         "UKNC / Korvet",               GROUP_COMPUTER, STATUS_READY, 0,
        "1987 , K1801VM1/2 (PDP-11) , 64-128KB"),
    SYS("pc88",         "NEC PC-8801",                 GROUP_COMPUTER, STATUS_PLANNED, 0,
        "1981 , 8086/V30 5-8 MHz , 64KB , uPD7220"),
    SYS("pc98",         "NEC PC-9801",                 GROUP_COMPUTER, STATUS_PLANNED, 0,
        "1982 , 8086/V30/V50 5-16 MHz , 128KB+ , YM2608"),
    SYS("iigs",         "Apple IIGS",                  GROUP_COMPUTER, STATUS_READY, 0,
        "1986 , 65C816 2.8 MHz , 256KB , VGC+ES5503"),
    SYS("pc286",        "PC AT (286)",                 GROUP_COMPUTER, STATUS_PLANNED, 0,
        "1984 , 80286 6-12 MHz , 1-16MB , EGA/VGA"),
    SYS("pc386",        "PC 386SX / 486SX",            GROUP_COMPUTER, STATUS_PLANNED, 0,
        "1988 , 80386/486 16-33 MHz , 1-16MB , VGA"),
    SYS("fmtowns",      "FM Towns",                    GROUP_COMPUTER, STATUS_PLANNED, 0,
        "1989 , 386DX 16-33 MHz , 2-4MB , FM 6v"),

    /* ========== Ноутбуки / карманные ПК (по году) ========== */
    SYS("osborne",      "Osborne 1 / Kaypro (CP/M)",   GROUP_LAPTOP, STATUS_READY, 0,
        "1981 , Z80 4 MHz , 64KB , mono display"),
    SYS("tandy100",     "Tandy Model 100 / PC-8201",   GROUP_LAPTOP, STATUS_PLANNED, 0,
        "1983 , 80C85 2.4 MHz , 8-32KB , text LCD"),
    SYS("t1100",        "Toshiba T1100 / T1100 Plus",  GROUP_LAPTOP, STATUS_READY, 0,
        "1985 , 80C88 4.77 MHz , 64-256KB , mono CGA"),
    SYS("ms1504",       "Electronica MS 1504",         GROUP_LAPTOP, STATUS_READY, 1,
        "1991 , 80C88 4.77 MHz , 64-256KB , LCD clone T1100"),
    SYS("hp200lx",      "HP 200LX / Poqet PC",         GROUP_LAPTOP, STATUS_READY, 0,
        "1994 , 80C186 16 MHz , 512KB-2MB , CGA 640x200"),
    SYS("portfolio",    "Atari Portfolio",             GROUP_LAPTOP, STATUS_PLANNED, 0,
        "1989 , 80C88 4.92 MHz , 128KB , 240x64 mono"),
    SYS("ti83",         "TI-83 and old calculators",   GROUP_LAPTOP, STATUS_READY, 0,
        "1996 , Z80 6 MHz , 32KB , LCD 96x64"),
};

#define NUM_SYSTEMS ((int)(sizeof(systems) / sizeof(systems[0])))

/* Папка ромов системы (dir или id). */
static const char *system_rom_dir(int idx)
{
    return systems[idx].dir ? systems[idx].dir : systems[idx].id;
}

#endif /* SYSTEMS_H */