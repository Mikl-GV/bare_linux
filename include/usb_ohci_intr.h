#ifndef USB_OHCI_INTR_H
#define USB_OHCI_INTR_H

#include <stdint.h>

/* Периодический interrupt-IN (HID) на OHCI — паттерн эталона pico-retro
 * usb_ohci.c (usb_ohci_intr_in_start / usb_ohci_intr_in_poll).
 * Иcпользуется для HID-клавиатуры/мыши full/low-speed. */

/* Запуск interrupt-IN: addr — адрес устройства, ep — номер EP (без 0x80),
 * buf — DMA-буфер отчёта (uncached), len — макс. пакет (обычно 8),
 * slot — 0 для первого HID (клавиатура). Возвращает 0 при успехе. */
int usb_ohci_intr_in_start(uint32_t base, uint8_t addr, uint8_t ep,
                           uint8_t *buf, uint16_t len, int slot);

/* Опрос: 1 = свежий пакет в buf; 0 = пакета нет; -1 = ошибка/переарм нужен. */
int usb_ohci_intr_in_poll(uint32_t base, uint8_t *buf, uint16_t len, int slot);

#endif /* USB_OHCI_INTR_H */