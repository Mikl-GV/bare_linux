#ifndef USB_H
#define USB_H

#include <stdint.h>

/* USB-аппаратура Allwinner H3 (Orange Pi Lite).
 *
 * Контроллеры (Linux sunxi-h3-h5.dtsi / U-Boot clock_sun6i.h):
 *   OTG/MUSB  0x01C19000  (FEL-кабель; не трогаем)
 *   PHY1: EHCI 0x01C1B000 / OHCI 0x01C1B400  (флешка; pmu1 0x01C1B800)
 *   PHY2: EHCI 0x01C1C000 / OHCI 0x01C1C400  (клавиатура; pmu2 0x01C1C800)
 *   (0x01C1A000/0x01C1A400 — EHCI0/OHCI0 для OTG, не используются.)
 * В даташите/докah 0x01C1B000 часто называют «EHCI1»; в проекте используем
 * нейтрально «PHY1/PHY2», чтобы не спорить с нумерацией.
 *
 * Подъём с нуля (независимо от U-Boot): CCU gate/reset (BUS_CLK_GATING0/RST0:
 * PHY1 биты 25/29, PHY2 26/30), CCU USB_CLK_CFG (0x0CC: bitN = reset PHY N,
 * bit(8+N) = clock PHY N, bit(16+N) = clock OHCI N), сброс OHCI, root port
 * reset, затем enumeration.
 * SIDDQ (analog powerdown) живёт НЕ в CCU, а в pmuN+0x10 (0x01C1B810 /
 * 0x01C1C810) bit1 — сбрасывается в 0. Passby (AHB burst/ULPI bypass),
 * биты 10/9/8/0 = 0x701 — в pmuN (0x01C1B800 / 0x01C1C800). */

/* ---- CCU ---- */
#define USB_CCU          0x01C20000u
#define USB_CCU_GATING0  (*(volatile uint32_t *)(USB_CCU + 0x060))
#define USB_CCU_RST0     (*(volatile uint32_t *)(USB_CCU + 0x2C0))  /* BUS_SOFT_RST0 */
/* CCU 0x0CC — USB_CLK_CFG: reset/clock PHY0..3 и clock OHCI0..3. */
#define USB_CCU_PHY_CFG  (*(volatile uint32_t *)(USB_CCU + 0x0CC))

#define USB_CCU_PHY_RST(n)   (1u << ((n) & 0x07))
#define USB_CCU_PHY_CLK(n)   (1u << (8 + ((n) & 0x07)))
#define USB_CCU_OHCI_CLK(n)  (1u << (16 + ((n) & 0x07)))

/* ---- Базы контроллеров (по PHY, см. шапку) ---- */
#define USB_EHCI_BASE1    0x01C1B000u   /* PHY1: флешка */
#define USB_EHCI_BASE2    0x01C1C000u   /* PHY2: клавиатура */
#define USB_OHCI_BASE1    0x01C1B400u
#define USB_OHCI_BASE2    0x01C1C400u

/* SIDDQ-бит HCI_PHY_CTL (pmuN+0x10): 1 = analog powerdown. */
#define USB_PMU1_BASE     0x01C1B800u
#define USB_PMU2_BASE     0x01C1C800u
#define USB_PHY_CTL_SIDDQ (1u << 1)
/* Passby: AHB burst INCR8/INCR4/INCRX_ALIGN + ULPI bypass = 0x701. */
#define USB_PMU_PASSBY    0x701u

/* Отладочные принты USB: 0 = тишина в рабочей сборке (как WIFI_VERBOSE_LOG).
 * Итоговые строки (INQUIRY/block/MBR/kbd/port) печатаются напрямую. */
#ifndef USB_VERBOSE_LOG
#define USB_VERBOSE_LOG 0
#endif
#define USB_LOG(...) do { if (USB_VERBOSE_LOG) { uart_print(__VA_ARGS__); } } while (0)
#define USB_LOG_HEX(v) do { if (USB_VERBOSE_LOG) { uart_print_hex((uint32_t)(v)); } } while (0)
#define USB_LOG_INT(v) do { if (USB_VERBOSE_LOG) { uart_print_int((int)(v)); } } while (0)

/* ---- Структуры EHCI (qH/qTD, async-список) ----
 * Флешка High-Speed — обмен только через EHCI (OHCI обслуживает FS/LS).
 * Раскладка строго по EHCI spec 1.0 (Table 3-5 / 3-6), 32-байтное
 * выравнивание; все поля в uncached (.coherent).
 *
 * qH (48 байт):
 *   0  link      — гориз. указатель: [31:5] адрес, [2:1]=01(QH), [0]=T
 *   1  endpt     — endpoint characteristics
 *   2  cap       — endpoint capabilities
 *   3  cur       — overlay: current qTD
 *   4  next      — overlay: next qTD
 *   5  alt       — overlay: alt qTD
 *   6  token     — overlay: token
 *   7..11 buf    — overlay: buffer pointers
 */
struct ehci_qh {
    uint32_t qh_link;
    uint32_t qh_endpt;
    uint32_t qh_cap;
    uint32_t qh_cur;
    uint32_t qh_next;
    uint32_t qh_alt;
    uint32_t qh_token;
    uint32_t qh_buf[5];
} __attribute__((aligned(32)));

/* qTD (32 байта):
 *   0 next, 1 alt, 2 token, 3..7 buffer pointers */
struct ehci_qtd {
    uint32_t qt_next;
    uint32_t qt_alt;
    uint32_t qt_token;
    uint32_t qt_buf[5];
} __attribute__((aligned(32)));

/* qTD token (EHCI spec Table 3-8):
 *   [31:16]=Total Bytes, [15]=I (Interrupt on Complete),
 *   [14:12]=Cerr, [11:10]=PID (00=OUT, 01=IN, 10=SETUP),
 *   [7]=Active, [6]=Halted, [5]=Data Buffer Error, [4]=Babble Detected,
 *   [3]=Transaction Error, [2]=Missed Micro-Frame,
 *   [1]=Split Transaction State, [0]=Ping State/ERR. */
#define EHCI_QTD_PID_OUT   0u
#define EHCI_QTD_PID_IN    1u
#define EHCI_QTD_PID_SETUP 2u

#define EHCI_QTD_ACTIVE   (1u << 7)
#define EHCI_QTD_HALTED   (1u << 6)
#define EHCI_QTD_DBE      (1u << 5)
#define EHCI_QTD_BABBLE   (1u << 4)
#define EHCI_QTD_XACTERR  (1u << 3)
/* Маска «передача завершилась неудачно»: Active ещё висит, либо взведён
 * любой из битов статуса/ошибки (Halted/DBE/Babble/XactErr/...). */
#define EHCI_QTD_ERR_MASK (EHCI_QTD_ACTIVE | 0x7Fu)

/* qH endpt (dword1, EHCI spec Table 3-5):
 *   [6:0]=Device Address, [11:8]=Endpoint Number,
 *   [14:12]=EPS (0=FS, 2=HS), [15]=H, [26:16]=MaxPacket, [27]=C (control),
 *   [31:28]=NAK Counter Reload. */
#define EHCI_QH_EPS_HS    (2u << 12)
#define EHCI_QH_C         (1u << 27)
#define EHCI_QH_MAXP(m)   ((uint32_t)(m) << 16)
#define EHCI_QH_EPN(n)    (((uint32_t)((n) & 0x0F)) << 8)

/* Секция uncached для DMA-дескрипторов и буферов (как .coherent в проекте). */
#define USB_UNCACHED __attribute__((section(".coherent"), aligned(64)))

/* Control-передача через EHCI (EP0, qH+qTD в async-списке).
 * setup[8], in_len/in_buf или out_data/out_len. 0 = успех. */
int usb_ehci_ctrl(const uint8_t *setup, const uint8_t *out_data, uint32_t out_len,
                  uint8_t *in_buf, uint32_t in_len);

/* Bulk (MSD): ep — номер эндпоинта (без бита 0x80), mps — wMaxPacketSize
 * ИМЕННО этого эндпоинта из config-дескриптора устройства (FS bulk —
 * 8/16/32/64, HS — 512). MPS передаётся с каждым вызовом: таблица «EP ->
 * MPS» общая на все устройства, и клавиатура с FS-флешкой затирали бы
 * значения друг друга.
 * bulk OUT: шлёт data[0..len); bulk IN: читает len байт в data. 0 = успех. */
int usb_bulk_out(uint8_t ep, uint16_t mps, const uint8_t *data, uint32_t len);
int usb_bulk_in(uint8_t ep, uint16_t mps, uint8_t *data, uint32_t len);

/* Инициализировать USB host (полный подъём с нуля — такты/PHY/порты свои).
 * Приведёт обе шины в RUN; вернёт число найденных устройств. */
int usb_hc_init(void);

/* Пере-инициализировать USB-флешку (пункт меню «USB reinit»): повторный
 * подъём порта 1 (reset + enum + MSD) и активация накопителя без перезагрузки.
 * 0 при успехе, -1 если устройство не принято. */
int usb_hc_reinit_flash(void);

/* Хелперы: текущие базы выбранной шины (устанавливаются usb_hc_init). */
void usb_select_bus(int bus);

#endif /* USB_H */