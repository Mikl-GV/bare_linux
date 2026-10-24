#include "usb_msd.h"
#include "usb.h"
#include "uart.h"
#include "sys.h"

/* USB Mass Storage BOT поверх нашего EHCI host (usb_hc.c).
 *
 * CBW/CSW протокол + READ(10)/WRITE(10). Данные ходят через bounce-буфер
 * .coherent (usb_msd_block_read/write копируют в/из caller-буфера):
 * буфер вызывающего может быть в обычном кэшируемом .bss/стеке, а EHCI
 * DMA пишет в физическую DRAM без ведома D-cache.
 *
 * ЛУ/конфигурация: interface ищется по классу (08/06/50), EP — только
 * внутри этого интерфейса. SET_CONFIGURATION и GET_MAX_LUN делает
 * вызывающий (usb_hc.c). */

/* Диагностика разбора config-дескриптора (интерфейс/эндпоинты) и CSW.
 * Ошибки (CBW/DATA/CSW fail) печатаются всегда — 0 только для «шумных»
 * строк рабочей сборки. */
#ifndef MSD_VERBOSE_LOG
#define MSD_VERBOSE_LOG 0
#endif

/* CBW/CSW буферы и bounce-сектор — uncached для DMA. */
static struct usb_msd_cbw g_cbw USB_UNCACHED;
static uint8_t g_csw_buf[13] USB_UNCACHED;
static uint8_t g_bounce[4096] USB_UNCACHED;   /* один блок устройства (до 4К) */

static struct usb_msd_dev g_dev_s;   /* активный dev для block-вызовов (не стек!) */
static struct usb_msd_dev *g_dev = &g_dev_s;
static int g_dev_valid = 0;

static int usb_msd_bulk_out(const uint8_t *data, uint32_t len)
{
    if (!g_dev_valid) return -1;
    return usb_bulk_out(g_dev->ep_out, g_dev->ep_out_mps, data, len);
}

static int usb_msd_bulk_in(uint8_t *data, uint32_t len)
{
    if (!g_dev_valid) return -1;
    return usb_bulk_in(g_dev->ep_in, g_dev->ep_in_mps, data, len);
}

/* Найти интерфейс MSD в config-дескрипторе и заполнить dev. */
int usb_msd_init(struct usb_msd_dev *dev, const uint8_t *cfg, uint32_t cfg_len)
{
    if (!dev || !cfg) return -1;
    dev->ep_in = dev->ep_out = 0;
    dev->ep_in_mps = dev->ep_out_mps = 0;
    dev->max_lun = 0;
    dev->block_count = 0;
    dev->block_size = 512;   /* сектор по умолчанию (BOT) */

    /* Парсим config: интерфейсы 0x04 / эндпоинты 0x05. EP принимаем ТОЛЬКО
     * внутри найденного MSD-интерфейса (на композите иначе берётся EP мыши). */
    uint32_t pos = 0;
    int in_msd = 0;
    while (pos + 2 <= cfg_len) {
        uint8_t bLen = cfg[pos];
        uint8_t bType = cfg[pos + 1];
        if (bLen == 0) break;
        if (pos + bLen > cfg_len) break;
        if (bType == 0x04) {   /* interface */
            uint8_t iclass = cfg[pos + 5];
            uint8_t isub   = cfg[pos + 6];
            uint8_t iprot  = cfg[pos + 7];
            in_msd = (iclass == 0x08 && isub == 0x06 && iprot == 0x50);
            if (in_msd && MSD_VERBOSE_LOG)
                uart_print("msd: iface found (class=8 sub=6 proto=0x50)\r\n");
        } else if (bType == 0x05 && in_msd) {   /* endpoint внутри MSD */
            uint8_t addr = cfg[pos + 2];
            uint8_t attr = cfg[pos + 3];
            uint16_t mps = (uint16_t)(cfg[pos + 4] | (cfg[pos + 5] << 8));
            mps &= 0x07FFu;
            if (attr == 0x02) {   /* bulk */
                if (addr & 0x80) { dev->ep_in  = addr & 0x0F; dev->ep_in_mps  = mps; }
                else             { dev->ep_out = addr & 0x0F; dev->ep_out_mps = mps; }
                if (MSD_VERBOSE_LOG) {
                    uart_print("msd: ep");
                    uart_print_int((int)(addr & 0x0F));
                    uart_print(addr & 0x80 ? " IN mps=" : " OUT mps=");
                    uart_print_int((int)mps);
                    uart_print("\r\n");
                }
            }
        }
        pos += bLen;
    }
    if (!dev->ep_in || !dev->ep_out) return -2;
    if (MSD_VERBOSE_LOG) {
        uart_print("msd: ep in=");
        uart_print_int((int)dev->ep_in);
        uart_print(" out=");
        uart_print_int((int)dev->ep_out);
        uart_print("\r\n");
    }
    return 0;
}

/* Выполнить CBW и прочитать CSW. data==0/data_len==0 — передача без данных.
 * Возвращает 0 при успехе. */
static int usb_msd_cmd(struct usb_msd_dev *dev, const uint8_t *cdb, uint8_t cb_len,
                       uint32_t dir_in, uint32_t xlen,
                       uint8_t *data, uint32_t data_len)
{
    if (!dev) return -1;
    g_cbw.signature         = USB_MSD_CBW_SIG;
    g_cbw.tag               = (uint32_t)0x11223344u + (uint32_t)dev->ep_in;
    g_cbw.data_transfer_len = xlen;
    g_cbw.flags             = dir_in ? USB_MSD_CBW_DIR_IN : USB_MSD_CBW_DIR_OUT;
    g_cbw.lun               = 0;
    g_cbw.cb_len            = cb_len;
    for (int i = 0; i < 16; i++) g_cbw.cb[i] = (i < cb_len) ? cdb[i] : 0;

    if (usb_msd_bulk_out((const uint8_t *)&g_cbw, 31) != 0) {
        uart_print("msd: op=0x");
        uart_print_hex(cdb[0]);
        uart_print(" CBW fail\r\n");
        return -2;
    }
    if (xlen > 0) {
        if (dir_in) {
            if (usb_msd_bulk_in(data, data_len) != 0) {
                uart_print("msd: op=0x");
                uart_print_hex(cdb[0]);
                uart_print(" DATA-IN fail len=");
                uart_print_int((int)data_len);
                uart_print("\r\n");
                return -3;
            }
        } else {
            if (usb_msd_bulk_out(data, data_len) != 0) {
                uart_print("msd: op=0x");
                uart_print_hex(cdb[0]);
                uart_print(" DATA-OUT fail len=");
                uart_print_int((int)data_len);
                uart_print("\r\n");
                return -3;
            }
        }
    }
    if (usb_msd_bulk_in(g_csw_buf, 13) != 0) {
        uart_print("msd: op=0x");
        uart_print_hex(cdb[0]);
        uart_print(" CSW fail\r\n");
        return -4;
    }
    struct usb_msd_csw *csw = (struct usb_msd_csw *)g_csw_buf;
    if (csw->signature != USB_MSD_CSW_SIG || csw->status != 0) {
        /* TUR (0x00) законно отвечает CHECK CONDITION, пока носитель не
           готов (ретраи — usb_msd_wait_ready); его дамп только шумит. */
        if (MSD_VERBOSE_LOG || cdb[0] != 0x00) {
            uart_print("msd: csw op=0x");
            uart_print_hex(cdb[0]);
            uart_print(" sig=0x");
            uart_print_hex(csw->signature);
            uart_print(" tag=0x");
            uart_print_hex(csw->tag);
            uart_print(" status=");
            uart_print_int((int)csw->status);
            uart_print(" residue=");
            uart_print_int((int)csw->residue);
            uart_print("\r\n");
        }
        return -5;
    }
    return 0;
}

/* READ(10): count блоков по dev->block_size c lba в data (блоки по 512:
 * накопители с block_size != 512 не поддерживаются — FAT оперирует 512). */
int usb_msd_read10(struct usb_msd_dev *dev, uint32_t lba,
                   uint32_t count, uint8_t *data)
{
    if (!dev || !data || count == 0) return -1;
    uint32_t bs = dev->block_size ? dev->block_size : 512;
    uint8_t cdb[10] = {
        0x28, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)(lba),
        0,                                  /* byte6: reserved/group */
        (uint8_t)(count >> 8), (uint8_t)(count),   /* byte7..8: transfer length */
        0
    };
    return usb_msd_cmd(dev, cdb, 10, 1, count * bs, data, count * bs);
}

/* WRITE(10): записать count блоков из data. */
int usb_msd_write10(struct usb_msd_dev *dev, uint32_t lba,
                    uint32_t count, const uint8_t *data)
{
    if (!dev || !data || count == 0) return -1;
    uint32_t bs = dev->block_size ? dev->block_size : 512;
    uint8_t cdb[10] = {
        0x2A, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)(lba),
        0,                                  /* byte6: reserved/group */
        (uint8_t)(count >> 8), (uint8_t)(count),   /* byte7..8: transfer length */
        0
    };
    return usb_msd_cmd(dev, cdb, 10, 0, count * bs, (uint8_t *)data, count * bs);
}

/* INQUIRY (36 байт). */
int usb_msd_inquiry(struct usb_msd_dev *dev, uint8_t *buf)
{
    if (!dev || !buf) return -1;
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    return usb_msd_cmd(dev, cdb, 6, 1, 36, buf, 36);
}

/* TEST UNIT READY (0x00): без данных, только CSW. */
int usb_msd_test_unit_ready(struct usb_msd_dev *dev)
{
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return usb_msd_cmd(dev, cdb, 6, 0, 0, 0, 0);
}

/* Готовность с ретраями: флешка сразу после SET_CONFIGURATION отвечает
 * CHECK CONDITION (CSW status=1), пока не инициализировала носитель. */
int usb_msd_wait_ready(struct usb_msd_dev *dev, int tries)
{
    if (!dev) return -1;
    if (tries < 1) tries = 1;
    for (int i = 0; i < tries; i++) {
        if (usb_msd_test_unit_ready(dev) == 0) return 0;
        udelay(100000);
    }
    return -1;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* READ CAPACITY(10) (0x25): 8 байт — last LBA (BE32), размер блока (BE32). */
int usb_msd_read_capacity(struct usb_msd_dev *dev,
                          uint32_t *last_lba, uint32_t *block_len)
{
    if (!dev) return -1;
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int rc = usb_msd_cmd(dev, cdb, 10, 1, 8, g_bounce, 8);
    if (rc != 0) return rc;
    uint32_t last = be32(g_bounce);
    uint32_t blk  = be32(g_bounce + 4);
    if (last_lba) *last_lba = last;
    if (block_len) *block_len = blk;
    if (blk >= 512 && blk <= sizeof(g_bounce)) dev->block_size = blk;
    dev->block_count = last + 1;
    return 0;
}

/* Блочные обёртки для FAT (fat_set_block_source): LBA — 512-байтные сектора.
 * Накопитель может иметь блок 4096 (4Kn): читаем/пишем блок устройства и
 * вырезаем нужные 512 байт; для записи — read-modify-write. */
int usb_msd_block_read(uint32_t lba, void *buf)
{
    if (!g_dev_valid || !buf) return -1;
    uint32_t bs = g_dev_s.block_size ? g_dev_s.block_size : 512;
    uint32_t ratio = bs / 512;
    if (ratio == 0) ratio = 1;
    uint32_t dlba = lba / ratio;
    uint32_t off  = (lba % ratio) * 512;
    if (usb_msd_read10(&g_dev_s, dlba, 1, g_bounce) != 0) return -1;
    uint8_t *d = (uint8_t *)buf;
    for (uint32_t i = 0; i < 512; i++) d[i] = g_bounce[off + i];
    return 0;
}

int usb_msd_block_write(uint32_t lba, const void *buf)
{
    if (!g_dev_valid || !buf) return -1;
    uint32_t bs = g_dev_s.block_size ? g_dev_s.block_size : 512;
    uint32_t ratio = bs / 512;
    if (ratio == 0) ratio = 1;
    uint32_t dlba = lba / ratio;
    uint32_t off  = (lba % ratio) * 512;
    if (ratio > 1) {
        if (usb_msd_read10(&g_dev_s, dlba, 1, g_bounce) != 0) return -1;
    }
    const uint8_t *s = (const uint8_t *)buf;
    for (uint32_t i = 0; i < 512; i++) g_bounce[off + i] = s[i];
    return usb_msd_write10(&g_dev_s, dlba, 1, g_bounce);
}

/* Зафиксировать активный накопитель (после usb_msd_init; вызывается из
 * usb_hc.c после полного enum+SET_CONFIGURATION). 0 при успехе. */
int usb_msd_activate(struct usb_msd_dev *dev)
{
    if (!dev || !dev->ep_in || !dev->ep_out) { g_dev_valid = 0; return -1; }
    g_dev_s = *dev;          /* копия: вызывающий может быть на стеке */
    g_dev = &g_dev_s;
    g_dev_valid = 1;
    return 0;
}

int usb_msd_active(void)
{
    return g_dev_valid;
}