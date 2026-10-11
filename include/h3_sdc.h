#ifndef H3_SDC_H
#define H3_SDC_H

#include <stdint.h>

/* SD/MMC хост-контроллер Allwinner H3.
 *
 * Используется контроллер SDC1 (база 0x01C10000) для связи с WiFi-чипом
 * RTL8189FTV по шине SDIO. PG0..PG5 = SDC1 CLK/CMD/D0-D3.
 *
 * Низкоуровневый доступ: инициализация тактов/пинов/сброса, отправка команд
 * и чтение/запись данных через FIFO. Этого достаточно, чтобы реализовать
 * SDIO CMD52/CMD53 (байтовый/блочный доступ к функциям чипа).
 */

/* Регистры SD/MMC хоста (offset от базы контроллера) — даташит 5.3.7. */
#define SDC_GCTL   0x00   /* global control (soft+rst, fifo_rst, dma, int_en) */
#define SDC_CKCR   0x04   /* clock control (card clock en/div) */
#define SDC_TMOR   0x08   /* timeout */
#define SDC_CTYPE  0x0C   /* bus width */
#define SDC_BLKSZ  0x10   /* block size */
#define SDC_BYCNT  0x14   /* byte count */
#define SDC_CMDR   0x18   /* command */
#define SDC_ARG    0x1C   /* command argument */
#define SDC_RESP0  0x20   /* response 0 */
#define SDC_RESP1  0x24   /* response 1 */
#define SDC_RESP2  0x28   /* response 2 */
#define SDC_RESP3  0x2C   /* response 3 */
#define SDC_IMKR   0x30   /* interrupt mask */
#define SDC_MISR   0x34   /* masked interrupt status */
#define SDC_RISR   0x38   /* raw interrupt status (W1C) */
#define SDC_STAR   0x3C   /* status */
#define SDC_FWLR   0x40   /* fifo water level */
#define SDC_FUNS   0x44   /* function select (write 0xceAA для включения функций) */
#define SDC_HWRST  0x78   /* hardware reset control */
#define SDC_FIFO   0x200  /* data FIFO (read/write data) */

/* Биты SDC_GCTL. */
#define SDC_GCTL_SOFT_RST  (1u << 0)
#define SDC_GCTL_FIFO_RST  (1u << 1)
#define SDC_GCTL_DMA_RST   (1u << 2)
#define SDC_GCTL_RESET     (SDC_GCTL_SOFT_RST | SDC_GCTL_FIFO_RST | SDC_GCTL_DMA_RST)
#define SDC_GCTL_INT_ENB   (1u << 4)
#define SDC_GCTL_CD_DBC    (1u << 8)

/* Биты SDC_CKCR. */
#define SDC_CKCR_CCLK_MASK   (0xFFu << 0)
#define SDC_CKCR_CCLK_ENB    (1u << 16)
#define SDC_CKCR_CCLK_CTRL   (1u << 17)
#define SDC_CKCR_MASK_DATA0  (1u << 31)

/* Биты SDC_STAR: состояние FIFO. */
#define SDC_STAR_FIFO_EMPTY  (1u << 2)
#define SDC_STAR_FIFO_FULL   (1u << 3)

/* Биты SDC_RISR: raw interrupt status. */
#define SDC_RISR_CMD_DONE    (1u << 2)   /* command complete */
#define SDC_RISR_RESP_ERR    (1u << 1)   /* response error (timeout/crc) */
#define SDC_RISR_DATA_DONE   (1u << 3)   /* data transfer complete */
#define SDC_RISR_RX_REQ      (1u << 5)   /* data receive request (read ready) */
#define SDC_RISR_TX_REQ      (1u << 4)   /* data transmit request (write ready) */
#define SDC_RISR_RESP_CRC    (1u << 6)   /* response CRC error */
#define SDC_RISR_DATA_CRC    (1u << 7)   /* data CRC error */
#define SDC_RISR_RESP_TO     (1u << 8)   /* response timeout */
#define SDC_RISR_RAW_ISTA    (0x0000FDFAu) /* все ошибки ответа/данных/таймаутов */

/* Биты/поля SDC_CMDR (даташит 5.3.7.7). */
#define SDC_CMDR_START       (1u << 31)  /* START - запуск передачи команды */
#define SDC_CMDR_IDX_MASK    0x3Fu
#define SDC_CMDR_RESP_RCV    (1u << 6)   /* command with response */
#define SDC_CMDR_LONG_RESP   (1u << 7)   /* 1=long (136-bit) response */
#define SDC_CMDR_CHK_CRC     (1u << 8)   /* check response CRC */
#define SDC_CMDR_DATA_TRANS  (1u << 9)   /* data transfer (read/write) */
#define SDC_CMDR_TRANS_DIR   (1u << 10)  /* 1=write, 0=read */
#define SDC_CMDR_TRANS_MODE  (1u << 11)  /* 0=block, 1=stream */
#define SDC_CMDR_STOP_CMD    (1u << 12)  /* auto stop at end of data */
#define SDC_CMDR_WAIT_PRE    (1u << 13)  /* wait previous data transfer over */
#define SDC_CMDR_STOP_ABT    (1u << 14)  /* stop/abort command (CMD12/52 abort) */
#define SDC_CMDR_SEND_INIT   (1u << 15)  /* send init sequence 80 clocks */
#define SDC_CMDR_PRG_CLK     (1u << 21)  /* change clock (PRG_CLK) */

/* Инициализировать контроллер SDC1: такт (OSC24M), пины PG0-5, сброс. */
void h3_sdc1_init(void);

/* Отправить команду без данных (с контролем CRC и ответом). */
int h3_sdc_cmd_no_data(uint32_t cmd, uint32_t arg, uint32_t resp_reg);

/* Низкоуровневый вариант: явно задать has_resp (1/0) и check_crc (1/0).
   Для SDIO-команд (CMD5/3/7/52/53) CRC не проверяется. */
int h3_sdc_cmd_no_data_flags(uint32_t cmd, uint32_t arg, uint32_t resp_reg,
                             int has_resp, int check_crc);

/* Отправить команду с данными (блочное чтение/запись). mode: 0=read, 1=write.
   data: указатель на буфер (для read), blkcnt — число блоков по blksz.
   Возвращает 0 при успехе, не-0 — ошибка. */
int h3_sdc_block_transfer(uint32_t cmd, uint32_t arg, uint32_t blksz,
                          uint32_t blkcnt, int write, void *data);

/* SDIO CMD52 — байтовый доступ к регистру функции чипа (чтение).
   Возвращает 0 при успехе и кладёт значение в *out; не-0 — ошибка. */
int h3_sdc_cmd52_read(uint32_t func, uint32_t reg, uint8_t *out);

/* SDIO CMD52 — байтовый доступ к регистру функции чипа (запись).
   Возвращает 0 при успехе; не-0 — ошибка. */
int h3_sdc_cmd52_write(uint32_t func, uint32_t reg, uint8_t val);

/* Полная инициализация SDIO-карты: CMD5 (IO_SEND_OP_COND), CMD3 (RCA),
   CMD7 (select). Должна быть выполнена до CMD52/CMD53. Возвращает 0 при
   успехе; не-0 — карта не ответила/ошибка. */
int h3_sdio_init_chip(void);

/* SDIO CMD53 — блочное чтение адресного пространства функции чипа.
   addr — стартовый адрес, blks — число блоков по 512 байт, data — буфер.
   Чтение в блок-режиме с автоинкрементом адреса. Возвращает 0 при успехе;
   не-0 — ошибка. Требует завершённой инициализации SDIO (h3_sdio_init_chip). */
int h3_sdc_cmd53_read(uint32_t func, uint32_t addr, uint32_t blks, void *data);

/* SDIO CMD53 — блочная запись в адресное пространство функции чипа.
   Используется для загрузки прошивки. Возвращает 0 при успехе; не-0 — ошибка. */
int h3_sdc_cmd53_write(uint32_t func, uint32_t addr, uint32_t blks, const void *data);

/* Прочитать CIS (Card Information Structure) чипа: CCCR 0x09-0x0B даёт адрес
   Common CIS, затем по нему идём по CIS-туплам, читая каждый через CMD52
   (байтовый доступ функции 0; функция 0 не поддерживает блочный CMD53).
   Ищем TPLID_MANFID (0x20) и кладём 16-бит код производителя в *manf_id
   (0x024C = Realtek). В *cis_addr_out кладётся адрес CIS. Возвращает 0 при
   успехе, иначе — ошибка. Вызывается после h3_sdio_init_chip(). */
int h3_sdio_read_cis(uint16_t *manf_id, uint32_t *cis_addr_out);

/* Переключить шину SDIO между 1-bit (four_bit=0) и 4-bit (four_bit=1).
   Обновляет и логический режим в CCCR 0x07, и аппаратную ширину SDC_CTYPE.
   Возвращает 0 при успехе; не-0 — ошибка. */
int h3_sdio_set_bus_width(int four_bit);

/* Прочитать 32-бит регистр WLAN-функции (function 1) чипа через CMD52.
   reg — ММIO-адрес регистра чипа (напр. REG_SYS_CFG=0xF0). Реальный SDIO-адрес
   = (8<<13) | (reg & 0x7FFF) (WLAN IOREG, device id 8). Кладёт значение в *val.
   Возвращает 0 при успехе; не-0 — ошибка. */
int h3_sdc_read_wlan_reg32(uint32_t reg, uint32_t *val);

/* Прочитать 16-бит регистр WLAN-функции (function 1) через CMD52.
   Аналогично read_wlan_reg32, но возвращает 2 байта. */
int h3_sdc_read_wlan_reg16(uint32_t reg, uint16_t *val);

/* Специфичная для чипа последовательность включения (power-on) RTL8189FTV:
   переводит карту из CARDEMU в ACTIVE перед загрузкой прошивки. Использует
   CMD52 к function 1. Возвращает 0 при успехе, не-0 — ошибка (см. коды). */
int h3_wifi_power_on_chip(void);

/* Загрузить прошивку чипа через SDIO (протокол MCUFWDL).
   fw — массив прошивки (RTL8189FTV NIC, ~0x4970 байт), size — размер.
   Возвращает 0 при успехе; не-0 — ошибка (см. коды в h3_sdc.c). */
int h3_wifi_download_fw(const void *fw, uint32_t size);

/* Регистры чипа (hal8188freg.h). */
#define REG_MCUFWDL_OFF      0x0080
#define FW_8188F_START       0x1000
#define MCUFWDL_RDY          (1u << 1)
#define FWDL_ChkSum_rpt      (1u << 2)
#define WINTINI_RDY          (1u << 6)
#define RAM_DL_SEL           (1u << 7)

/* Диагностика последней блочной передачи (заполняются при ошибке CMD53). */
extern uint32_t sdc_diag_risr;
extern uint32_t sdc_diag_sta;
extern uint32_t sdc_diag_cmdr;
extern uint32_t sdc_diag_cmd;
extern uint32_t sdc_diag_arg;

#endif /* H3_SDC_H */