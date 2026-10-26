#ifndef USB_MSD_H
#define USB_MSD_H

#include <stdint.h>

/* USB Mass Storage (BOT — Bulk-Only Transport), спецификация
 * "USB Mass Storage Class Bulk-Only Transport" v1.0.
 *
 * Работает поверх EHCI host (usb_hc.c): контрольные передачи делает
 * вызывающий (enumeration в usb_hc.c: SET_ADDRESS, GET_DESCRIPTOR,
 * SET_CONFIGURATION), здесь — только CBW/CSW + READ(10)/WRITE(10).
 * После полного enum вызывается usb_msd_activate() — с этого момента
 * активен блочный источник (usb_msd_block_read/write). Данные ходят
 * через bounce-буфер .coherent, поэтому FAT-буфер (обычный .bss) можно
 * передавать напрямую.
 */

/* CBW (31 байт) + CSW (13 байт) — Bulk-only. */
#define USB_MSD_CBW_SIG   0x43425355u   /* 'USBC' little-endian */
#define USB_MSD_CSW_SIG   0x53425355u   /* 'USBS' */

/* bmCBWFlags */
#define USB_MSD_CBW_DIR_IN  0x80u       /* host -> device data IN */
#define USB_MSD_CBW_DIR_OUT 0x00u

#pragma pack(push, 1)
struct usb_msd_cbw {
    uint32_t signature;      /* CBW_SIG */
    uint32_t tag;            /* host-specified, echoes in CSW */
    uint32_t data_transfer_len;
    uint8_t  flags;          /* DIR_IN/OUT */
    uint8_t  lun;            /* 0..15 */
    uint8_t  cb_len;         /* 1..16 */
    uint8_t  cb[16];         /* SCSI CDB */
};

struct usb_msd_csw {
    uint32_t signature;      /* CSW_SIG */
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;         /* 0 = success */
};
#pragma pack(pop)

/* Эндпоинты интерфейса (заполняются usb_msd_init по config-дескриптору). */
struct usb_msd_dev {
    uint8_t  ep_in;          /* bulk IN (чтение данных с флешки) */
    uint8_t  ep_out;         /* bulk OUT (CBW) */
    uint16_t ep_in_mps;      /* wMaxPacketSize bulk IN из дескриптора */
    uint16_t ep_out_mps;     /* wMaxPacketSize bulk OUT из дескриптора */
    uint8_t  max_lun;        /* всегда 0 (GET_MAX_LUN не обязателен) */
    uint32_t block_count;    /* число секторов (не заполняется: см. READ CAPACITY) */
    uint32_t block_size;     /* 512 (FAT оперирует 512-байтными секторами) */
};

/* Найти первый MSD-интерфейс (класс 0x08, подкласс 0x06 BOT, протокол 0x50)
 * и заполнить dev: bulk-EP берутся ТОЛЬКО внутри этого интерфейса.
 * Возвращает 0 при успехе, <0 при ошибке. */
int usb_msd_init(struct usb_msd_dev *dev, const uint8_t *config_desc,
                 uint32_t config_len);

/* READ(10): прочитать count секторов (512) начиная с lba в data.
 * data — буфер (не короче count*512). Возвращает 0 при успехе. */
int usb_msd_read10(struct usb_msd_dev *dev, uint32_t lba,
                   uint32_t count, uint8_t *data);

/* WRITE(10): записать count секторов (512) из data. 0 при успехе. */
int usb_msd_write10(struct usb_msd_dev *dev, uint32_t lba,
                    uint32_t count, const uint8_t *data);

/* INQUIRY (диагностика): вернуть 36-байт данные в buf. 0 при успехе. */
int usb_msd_inquiry(struct usb_msd_dev *dev, uint8_t *buf);

/* TEST UNIT READY (0x00) — готовность носителя. 0 при успехе. */
int usb_msd_test_unit_ready(struct usb_msd_dev *dev);

/* TEST UNIT READY с ретраями: сразу после SET_CONFIGURATION носитель
 * отвечает CHECK CONDITION, пока не «проснулся». tries = число попыток
 * (пауза 100 мс между ними). 0 — готов. */
int usb_msd_wait_ready(struct usb_msd_dev *dev, int tries);

/* READ CAPACITY(10) (0x25): last_lba и размер блока (512/4096...).
 * Заполняет dev->block_count/block_size. 0 при успехе. */
int usb_msd_read_capacity(struct usb_msd_dev *dev,
                          uint32_t *last_lba, uint32_t *block_len);

/* Зафиксировать накопитель как активный блочный источник (копия dev
 * хранится внутри; вызывающий может передать стековый dev). Вызывается
 * после полного enumeration + SET_CONFIGURATION. */
int usb_msd_activate(struct usb_msd_dev *dev);

/* BOT Reset Recovery: сбросить состояние и toggle bulk-труб после Stall или
 * десинка (короткий приём блока оставил в трубке остаток данных). */
void usb_msd_bot_recover(void);
int usb_msd_active(void);

/* Блочные обёртки для FAT (fat_set_block_source): читать/писать сектор с
 * активного USB-накопителя. LBA — сектора (512 байт), как в SD. 0 при
 * успехе. Реализация копирует через bounce-буфер .coherent (см. шапку). */
int usb_msd_block_read(uint32_t lba, void *buf);
int usb_msd_block_write(uint32_t lba, const void *buf);

#endif /* USB_MSD_H */