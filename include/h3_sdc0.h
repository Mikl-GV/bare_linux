#ifndef H3_SDC0_H
#define H3_SDC0_H

#include <stdint.h>

/* SD/MMC хост-контроллер SDC0 (база 0x01C0F000) — слот SD-карты Orange Pi Lite.
 * PC0..PC6 = SDC0 CLK/CMD/D0-D3 (function 2).
 *
 * Полный подъём с нуля (без наследования от U-Boot): CCU gate+reset,
 * модульный такт SDMMC0_CLK (OSC24M на init), пины, сброс контроллера,
 * инициализация карты (CMD0/CMD8/ACMD41/CMD2/CMD3/CMD7/ACMD6/CMD16/CMD58),
 * чтение/запись секторов через FIFO (PIO, 1 блок за раз).
 */

/* Результат инициализации карты. */
struct sdc0_info {
    uint8_t  sdhc;        /* 1 = SDHC/SDXC (секторная адресация), 0 = SDSC */
    uint8_t  wide4;       /* 1 = 4-bit режим включён */
    uint32_t total_sectors; /* объём карты в секторах (из CSD) */
};

/* Полная инициализация: такт/пины/сброс + init карты + 4-bit.
 * Заполняет *info. Возвращает 0 при успехе, <0 при ошибке. */
int  h3_sdc0_init(struct sdc0_info *info);

/* Прочитать один сектор (512 байт) по LBA (секторная или байтовая
 * адресация выбирается автоматически по типу карты). 0 = успех, <0 = ошибка. */
int  h3_sdc0_read_sector(uint32_t lba, void *buf);

/* Записать один сектор (512 байт). 0 = успех, <0 = ошибка. */
int  h3_sdc0_write_sector(uint32_t lba, const void *buf);

#endif /* H3_SDC0_H */