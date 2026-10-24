#ifndef USB_OHCI_H
#define USB_OHCI_H

#include <stdint.h>

/* OHCI-драйвер H3: 2 порта (PHY1 флешка 0x01C1B400, PHY2 клавиатура
 * 0x01C1C400). Паттерн pico-retro usb_ohci.c (per-port пулы).
 * Полный цикл тактов/PHY + OHCI init (без HCR) + root port power/reset
 * + control через ED/TD (head/tail, CLF, D-cache). */

/* Базы OHCI. */
#define USB_OHCI1_BASE  0x01C1B400u
#define USB_OHCI2_BASE  0x01C1C400u

/* Инициализация порта (такт/PHY + OHCI init). 0 при успехе. */
int usb_ohci_init_port1(void);
int usb_ohci_init_port2(void);

/* 1 если на порту устройство (CCS). */
int usb_ohci_port_connected(uint32_t base);
int usb_ohci_port_connected1(void);
int usb_ohci_port_connected2(void);

/* Сброс корневого порта (PRS, ждать снятия; CCS/PES проверка). 0 при успехе. */
int usb_ohci_port_reset_all1(void);
int usb_ohci_port_reset_all2(void);

/* EP0 max-packet на порт (по bMaxPacketSize0 из device-дескриптора).
 * По умолчанию 8; FS-устройства с MPS 16/32/64 требуют установки ДО
 * управления шиной. */
void     usb_ohci_set_mps(uint32_t base, uint16_t mps);
uint16_t usb_ohci_get_mps(uint32_t base);

/* Control-передача (EP0, адрес addr) на конкретном порту.
 * Возвращает число прочитанных байт или <0 при ошибке. */
int usb_ohci_ctrl_transfer(uint32_t base, uint8_t addr,
                           const uint8_t *setup, uint8_t setup_len,
                           uint8_t *data, uint32_t data_len, int dir_in,
                           uint32_t timeout_ms);

/* Control-передача с адресом 0 (до SET_ADDRESS) на конкретном порту. */
int usb_ohci_ctrl_addr0(uint32_t base, const uint8_t *setup, uint8_t setup_len,
                        uint8_t *data, uint32_t data_len, int dir_in);

/* Bulk-передача (MSD для FS/LS-устройств на OHCI). mps — wMaxPacketSize
 * эндпоинта из config-дескриптора. 0 при успехе. */
int usb_ohci_bulk_out(uint32_t base, uint8_t addr, uint8_t ep,
                      const uint8_t *data, uint32_t len, uint16_t mps);
int usb_ohci_bulk_in(uint32_t base, uint8_t addr, uint8_t ep,
                     uint8_t *data, uint32_t len, uint16_t mps);

/* Сбросить программные toggle bulk-эндпоинтов порта (после BOT Reset). */
void usb_ohci_bulk_toggle_reset(uint32_t base);

/* Текущий адрес устройства (SET_ADDRESS из enumeration). */
extern uint8_t g_usb_addr;

#endif /* USB_OHCI_H */