#include "usb_kbd.h"
#include "usb_ohci.h"
#include "usb_ohci_intr.h"
#include "usb.h"
#include "pcf8574.h"
#include "uart.h"
#include "sys.h"
#include <string.h>

/* USB HID-клавиатура для меню. Паттерн эталона pico-retro usb_kbd.c:
 *  - enum (GET_DESC/SET_ADDRESS/SET_CONFIGURATION) делает usb_hc.c общим
 *    путём для обоих OHCI-портов; здесь — разбор config-дескриптора
 *    (интерфейс boot-keyboard 3,1,1 + EP IN) и SET_PROTOCOL(boot);
 *  - чтение отчёта 8 байт через Interrupt IN (OHCI периодический ED/TD);
 *  - перевод HID-сканкодов в BTN_* (см. pcf8574.h).
 *
 * Автоповтор (навигация меню): первый код сразу, повтор по таймеру.
 * Все DMA-буферы (report) — в .coherent (uncached): OHCI пишет их
 * напрямую в DRAM, кэшированный .bss/стек недопустим. */

/* Диагностика сырых HID-отчётов (8 байт + метка времени). 0 — выключена. */
#ifndef KBD_VERBOSE_LOG
#define KBD_VERBOSE_LOG 0
#endif

/* ---- setup-структуры, как эталон ---- */
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} usb_setup_t;

#define HID_SET_PROTOCOL  0x0Bu

/* Состояние клавиатуры: report[] в uncached (.coherent) — HC пишет его
 * через DMA (interrupt-IN TD). Остальные поля — обычный .bss. */
static struct {
    uint32_t base;
    uint8_t  addr;
    uint8_t  in_ep;
    uint8_t  report[8] __attribute__((aligned(32)));
    int      found;
    int      low_speed;   /* EP0 MPS 8: Low-Speed */
} g_kbd;

/* Маппинг HID-сканкодов -> BTN_* (минимум: навигация + подтверждение). */
static uint32_t kbd_scan_to_btn(uint8_t k)
{
    switch (k) {
    case 0x52: return BTN_UP;       /* Up */
    case 0x51: return BTN_DOWN;     /* Down */
    case 0x50: return BTN_LEFT;     /* Left */
    case 0x4F: return BTN_RIGHT;    /* Right */
    case 0x28: return BTN_A;        /* Enter */
    case 0x2C: return BTN_A;        /* Space */
    case 0x29: return BTN_B;        /* Esc */
    case 0x2B: return BTN_START;    /* Tab */
    case 0x0E: return BTN_SELECT;   /* Backspace */
    default:   return 0;
    }
}

static int usb_kbd_ctrl(const usb_setup_t *req, uint8_t *data,
                        uint32_t data_len, int dir_in)
{
    return usb_ohci_ctrl_transfer(g_kbd.base, g_kbd.addr,
                                  (const uint8_t *)req, 8,
                                  data, data_len, dir_in, 1000);
}

/* Разбор config-дескриптора после enum (usb_hc.c): ищем boot-клавиатуру
 * (interface class 3, subclass 1, proto 1) и её interrupt IN EP.
 * 0 — EP найден, <0 — это не HID-клавиатура. */
static int usb_kbd_parse(const uint8_t *cfg, uint32_t cfg_len)
{
    int r = (int)cfg_len;
    int pos = 0, type = 0;
    while (pos + 1 < r && pos + 1 < 256) {
        uint8_t len = cfg[pos], t = cfg[pos + 1];
        if (len == 0 || pos + len > r) break;
        if (t == 4 && len >= 9) {   /* interface: сброс состояния */
            type = 0;
            /* boot keyboard (class 3, subclass 1, proto 1) */
            if (cfg[pos+5] == 3 && cfg[pos+6] == 1 && cfg[pos+7] == 1) {
                type = 1;
                g_kbd.in_ep = 0;   /* ждём EP IN этого интерфейса */
            }
        } else if (t == 5 && len >= 7 && type == 1) {   /* endpoint */
            uint8_t epa = cfg[pos + 2], attr = cfg[pos + 3];
            if ((epa & 0x80) && attr == 0x03) {
                g_kbd.in_ep = epa & 0x7F;   /* interrupt IN клавиатуры */
            }
        }
        pos += len;
    }
    return g_kbd.in_ep ? 0 : -1;
}

/* ---- Interrupt-IN: периодический ED/TD на OHCI2 (по эталону) ---- */

static int g_intr_started = 0;

/* Каждый непустой отчёт донгла — событие. Донгл сам повторяет код при
 * удержании (~350 мс), а пустой отчёт означает отпускание. Свой таймер
 * автоповтора не нужен: он и давал «залипание», если отчёт об отпускании
 * терялся. Дедуп — только от точных дублей подряд. */
static uint8_t  g_emit_key = 0;        /* последний выданный HID-код */
static uint32_t g_emit_us = 0;         /* когда выдали */
static uint32_t g_last_rx_us = 0;      /* когда был последний отчёт */

#define KBD_WATCHDOG_US 5000000u       /* нет отчётов 5 с -> перезапуск IN */

int usb_kbd_init_on(uint32_t base, const uint8_t *cfg, uint32_t cfg_len)
{
    if (!cfg || cfg_len < 9) return -1;
    g_kbd.base = base;
    g_kbd.addr = 1;        /* enum в usb_hc.c уже выдал адрес 1 */
    g_kbd.found = 0;
    g_kbd.in_ep = 0;

    if (usb_kbd_parse(cfg, cfg_len) != 0) {
        uart_print("usb-kbd: no boot keyboard\r\n");
        return -1;
    }

    /* SET_PROTOCOL(boot): не критично, многие донглы шлют boot-отчёты сами. */
    usb_setup_t p = { 0x21, HID_SET_PROTOCOL, 0, 0, 0 };
    usb_kbd_ctrl(&p, 0, 0, 0);

    /* Low-Speed — по MPS EP0 порта (у LS всегда 8). */
    g_kbd.low_speed = (usb_ohci_get_mps(base) <= 8) ? 1 : 0;
    g_kbd.found = 1;
    uart_print("usb-kbd: -> KEYBOARD (ep=0x");
    uart_print_hex((uint32_t)g_kbd.in_ep);
    uart_print(" ls=");
    uart_print_int(g_kbd.low_speed);
    uart_print(")\r\n");

    /* interrupt-IN: LW-speed донгл читаем периодически (эталон: LS-донгл не
     * отвечает на GET_REPORT). report — в .coherent. */
    usb_ohci_intr_in_start(g_kbd.base, g_kbd.addr,
                           g_kbd.in_ep, g_kbd.report, 8, 0);
    g_intr_started = 1;
    g_emit_key = 0;
    g_emit_us = 0;
    g_last_rx_us = micros();
    return 0;
}

int usb_kbd_active(void)
{
    return g_kbd.found;
}

/* Каждый непустой отчёт донгла — событие: при удержании он сам повторяет
 * код (~350 мс), при отпускании шлёт пустой отчёт. Свой таймер автоповтора
 * не нужен — именно он и давал «залипание», когда отчёт об отпускании
 * терялся. Без отчётов нет событий. Дедуп 100 мс — только от точных
 * дублей подряд. */
#define KBD_DEDUP_US 100000u

uint32_t usb_kbd_poll(void)
{
    if (!g_kbd.found || !g_intr_started) return 0;

    int r = usb_ohci_intr_in_poll(g_kbd.base, g_kbd.report, 8, 0);
    uint32_t now_u = micros();

    if (r == 1) g_last_rx_us = now_u;

    if (r < 0) {
        /* TD завершился ошибкой (CRC/timeout) — перезапустить периодику. */
        usb_ohci_intr_in_start(g_kbd.base, g_kbd.addr,
                               g_kbd.in_ep, g_kbd.report, 8, 0);
        g_last_rx_us = now_u;
        return 0;
    }
    if (r != 1) {
        /* Watchdog: донгл шлёт отчёты регулярно (в т.ч. пустые). Если
           отчётов нет 5 с — periodic-ED/TD застрял, перезапускаем. */
        if ((uint32_t)(now_u - g_last_rx_us) >= KBD_WATCHDOG_US) {
            usb_ohci_intr_in_start(g_kbd.base, g_kbd.addr,
                                   g_kbd.in_ep, g_kbd.report, 8, 0);
            g_last_rx_us = now_u;
        }
        return 0;
    }

    if (KBD_VERBOSE_LOG) {
        uart_print("kbd t=");
        uart_print_int((int)(now_u / 1000u));
        uart_print(" rep");
        for (int i = 0; i < 8; i++) {
            uart_print(" ");
            uart_print_hex((uint32_t)g_kbd.report[i]);
        }
        uart_print("\r\n");
    }

    /* 6 кейкодов (report[2..7]) + модификаторы report[0]. */
    uint8_t k = g_kbd.report[2];
    if (k == 0) k = g_kbd.report[3];
    if (k == 0) k = g_kbd.report[4];
    if (k == 0) k = g_kbd.report[5];
    if (k == 0) k = g_kbd.report[6];
    if (k == 0) k = g_kbd.report[7];

    if (k == 0) {                 /* пустой отчёт — отпускание */
        g_emit_key = 0;
        return 0;
    }
    if (k == g_emit_key && (uint32_t)(now_u - g_emit_us) < KBD_DEDUP_US)
        return 0;                 /* точный дубль подряд */
    g_emit_key = k;
    g_emit_us = now_u;
    return kbd_scan_to_btn(k);
}