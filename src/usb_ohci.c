#include "usb.h"
#include "usb_ohci.h"
#include "h3_ccu.h"
#include "uart.h"
#include "sys.h"
#include <string.h>

/* OHCI-драйвер для H3 — 2 порта (PHY1: флешка 0x01C1B400; PHY2: клавиатура
 * 0x01C1C400). Паттерн эталона pico-retro usb_ohci.c (per-port пулы).
 *
 * Порт поднимается отдельно (эталон):
 *  - такты/PHY: GATING0/RST0 (PHY1: биты 25/29, PHY2: 26/30),
 *    CCU USB_CLK_CFG 0x0CC (bitN = reset PHY N, bit(8+N) = clock PHY N,
 *    bit(16+N) = clock OHCI N), цикл сброс->пауза->вкл->пауза;
 *    SIDDQ НЕ в CCU — снимается через pmuN+0x10 (0x01C1B810/0x01C1C810)
 *    бит1; passby (0x701) пишется в pmuN (0x01C1B800/0x01C1C800);
 *  - OHCI init БЕЗ HCR (fminterval=0x2edf+fsmps, periodstart, lsperiod);
 *  - root port PPS->PRS, ожидание CCS/PES;
 *  - control: ED(head/tail)+TD(setup/data/status/dummy), CLF, D-cache.
 */

#define OHCI1_BASE  0x01C1B400u   /* PHY1: флешка */
#define OHCI2_BASE  0x01C1C400u   /* PHY2: клавиатура */
#define PHY1_EHCI_BASE  0x01C1B000u
#define PHY2_EHCI_BASE  0x01C1C000u

enum {
    RH_PS_CCS   = (1u << 0),
    RH_PS_PES   = (1u << 1),
    RH_PS_PRS   = (1u << 4),
    RH_PS_PPS   = (1u << 8),
    RH_PS_LSDA  = (1u << 9),
    RH_PS_CSC   = (1u << 16),
    RH_PS_PRSC  = (1u << 20),
};

enum {
    ED_SKIP      = (1u << 14),
    ED_LOWSPEED  = (1u << 13),
    ED_FROM_TD   = (0u << 11),
    ED_OUT       = (1u << 11),
    ED_IN        = (2u << 11),
    TD_T_DATA0   = (2u << 24),
    TD_T_DATA1   = (3u << 24),
    TD_T_TOGGLE  = (0u << 24),   /* toggle из ED carry (bulk) */
    TD_DP_SETUP  = (0u << 19),
    TD_DP_OUT    = (1u << 19),
    TD_DP_IN     = (2u << 19),
    TD_R         = (1u << 18),
    TD_CC_SHIFT  = 28,
    TD_CC_NOTACC = 0xFu,
    TD_CC_NOERR  = 0x0u,
};

typedef struct {
    volatile uint32_t rev, ctrl, cmdstatus, intrstatus, intrenable,
                      intrdisable, hcca, peried, ctrlhead, ctrlcur,
                      bulkhead, bulkcur, donehead, fminterval, fmremaining,
                      fmnumber, periodstart, lsperiod, rha_des, rhb_des,
                      rhstatus, rhport[2];
} ohci_regs_t;

typedef struct __attribute__((aligned(16))) {
    uint32_t cfg; uint32_t tail; uint32_t head; uint32_t next;
} ohci_ed_t;

typedef struct __attribute__((aligned(32))) {
    uint32_t cfg; uint32_t cbp; uint32_t next; uint32_t be; uint32_t pad[4];
} ohci_td_t;

typedef struct __attribute__((aligned(256))) {
    uint32_t intr[32]; uint16_t framenumber; uint16_t pad;
    uint32_t donehead; uint8_t reserved[120];
} ohci_hcca_t;

#define OHCI_NPORTS 2
/* per-port пулы ED/TD/HCCA (эталон: каждый порт свой; иначе прерывания
 * одного порта ломали бы контроль другого). */
static ohci_hcca_t g_hcca[OHCI_NPORTS] __attribute__((section(".coherent"), aligned(256)));
static ohci_ed_t   g_head_ed[OHCI_NPORTS] __attribute__((section(".coherent"), aligned(16)));
static ohci_td_t   g_td[OHCI_NPORTS][8] __attribute__((section(".coherent"), aligned(32)));
/* Interrupt-IN (HID) для клавиатуры (usb_ohci_intr_in_start/poll). */
static ohci_ed_t g_int_ed[OHCI_NPORTS][2] __attribute__((section(".coherent"), aligned(16)));
static ohci_td_t g_int_td[OHCI_NPORTS][2][2] __attribute__((section(".coherent"), aligned(32)));
/* Явный toggle interrupt-IN (0=DATA0, 1=DATA1). ED carry на H3 не
 * обновляется — без этого после первого отчёта получаем mismatch. */
static uint8_t g_int_toggle[OHCI_NPORTS][2];
static uint16_t    g_mps[OHCI_NPORTS] = { 8, 8 };

static int ohci_idx(uint32_t base);

/* EP0 max-packet на порт: вызывается после чтения device-дескриптора
 * (bMaxPacketSize0). Без этого FS-клавиатура с MPS 64 получит пакеты по 8.
 * Значение по умолчанию 8 (Low-Speed AVATTO). */
void usb_ohci_set_mps(uint32_t base, uint16_t mps)
{
    int idx = ohci_idx(base);
    if (idx < 0) return;
    if (mps < 8) mps = 8;
    if (mps > 64) mps = 64;
    g_mps[idx] = mps;
}

uint16_t usb_ohci_get_mps(uint32_t base)
{
    int idx = ohci_idx(base);
    return (idx < 0) ? 8 : g_mps[idx];
}

static int ohci_idx(uint32_t base)
{
    if (base == OHCI1_BASE) return 0;
    if (base == OHCI2_BASE) return 1;
    return -1;
}

extern void mmu_mark_uncached(uint32_t addr);

static void cache_clean(uint32_t addr, uint32_t size)
{
    addr &= ~0x1Fu;
    uint32_t end = addr + size + 32;
    for (; addr < end; addr += 32)
        __asm volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(addr));
    __asm volatile("dsb" ::: "memory");
}

static void cache_invalidate(uint32_t addr, uint32_t size)
{
    addr &= ~0x1Fu;
    uint32_t end = addr + size + 32;
    for (; addr < end; addr += 32)
        __asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(addr));
    __asm volatile("dsb" ::: "memory");
}

/* Запись в PHY через PHYCTL (0x01C19410, REG_PHYCTL_A33): адрес [15:8],
 * данные bit7, тактовый бит usbc = 1<<(2*phy_index). Так Linux
 * (sun4i_usb_phy_write) настраивает TX amplitude и disconnect threshold —
 * без них HS-устройство может не пройти chirp и сесть на FS. */
static void usb_phy_write(int phy_index, uint32_t addr, uint32_t data, int len)
{
    volatile uint32_t *ctl = (volatile uint32_t *)0x01C19410u;
    uint32_t usbc = 1u << (phy_index * 2);
    for (int i = 0; i < len; i++) {
        uint32_t v = *ctl;
        v = (v & ~(0xFFu << 8)) | (((addr + (uint32_t)i) & 0xFFu) << 8);
        *ctl = v;
        uint32_t t = *ctl;
        if (data & 1u) t |= 0x80u; else t &= ~0x80u;
        t &= ~usbc; *ctl = t;
        t |= usbc;  *ctl = t;
        t &= ~usbc; *ctl = t;
        data >>= 1;
    }
    udelay(100);
}

/* Полный цикл тактов/PHY для порта (1 или 2) — эталон usb_port_hw_init. */
static void usb_port_hw_init(int port)
{
    if (port < 1 || port > 2) return;
    uint32_t e_bit = (1u << (24 + port));
    uint32_t o_bit = (1u << (28 + port));
    uint32_t phy_rst = (1u << port);
    uint32_t phy_cal = (1u << (8 + port));
    uint32_t phy_clk = (1u << (16 + port));
    uint32_t ehci_base = (port == 1) ? PHY1_EHCI_BASE : PHY2_EHCI_BASE;

    H3_CCU_GATING0 &= ~(e_bit | o_bit);
    H3_CCU_RST0    &= ~(e_bit | o_bit);
    USB_CCU_PHY_CFG &= ~(phy_rst | phy_cal | phy_clk);
    __asm volatile("dsb" ::: "memory");
    udelay(10000);

    H3_CCU_GATING0 |= e_bit | o_bit;
    H3_CCU_RST0    |= e_bit | o_bit;
    USB_CCU_PHY_CFG |= phy_rst | phy_cal | phy_clk;
    __asm volatile("dsb" ::: "memory");
    udelay(30000);

    volatile uint32_t *icr = (volatile uint32_t *)(ehci_base + 0x800);
    icr[0] = 0x00000701u;
    icr[4] = 0;
    __asm volatile("dsb" ::: "memory");

    /* PHY tuning (Linux sun4i_usb_phy_init, H3, disc_thresh=3):
       TX amplitude tune = 0x14 (5 бит), disconnect threshold = 3 (2 бита).
       Без этого HS-флешка может определиться как FS. */
    usb_phy_write(port, 0x20, 0x14, 5);
    usb_phy_write(port, 0x2a, 3, 2);
}

/* Инициализация OHCI порта (эталон usb_ohci_init). */
static int usb_ohci_init_port(uint32_t base)
{
    int idx = ohci_idx(base);
    if (idx < 0) return -1;
    ohci_regs_t *ohci = (ohci_regs_t *)base;

    mmu_mark_uncached((uint32_t)&g_hcca[idx]);
    mmu_mark_uncached((uint32_t)&g_head_ed[idx]);
    mmu_mark_uncached((uint32_t)&g_td[idx][0]);

    uart_print("usb: ohci(idx=");
    uart_print_int(idx);
    uart_print(") rev=0x");
    uart_print_hex(ohci->rev);
    uart_print("\r\n");

    usb_port_hw_init(idx + 1);

    if (ohci->rev != 0x10) { uart_print("usb: OHCI dead\r\n"); return -1; }

    ohci->intrdisable = 0xFFFFFFFFu;
    memset(&g_hcca[idx], 0, sizeof(g_hcca[idx]));
    memset(&g_head_ed[idx], 0, sizeof(g_head_ed[idx]));
    memset(&g_td[idx], 0, sizeof(g_td[idx]));

    g_head_ed[idx].cfg = ED_SKIP | ((uint32_t)g_mps[idx] << 16);
    g_head_ed[idx].head = 1;
    g_head_ed[idx].tail = 1;

    ohci->hcca = (uint32_t)(uintptr_t)&g_hcca[idx];

    uint32_t fi = 0x2edf;
    uint32_t fsmps = (6 * (fi - 210)) / 7;
    ohci->fminterval = (fsmps << 16) | fi;
    ohci->fminterval ^= (1u << 31);
    ohci->periodstart = (fi * 9) / 10;
    ohci->lsperiod = 0x628;

    ohci->bulkhead = 0;
    ohci->bulkcur  = 0;
    ohci->peried   = 0;

    ohci->ctrl = (ohci->ctrl & ~(3u << 6)) | (2u << 6) | (1u << 4) | (1u << 5);
    /* bit4 = CLE (control list), bit5 = BLE (bulk list — нужен для MSD). */
    udelay(1000);

    ohci->rhstatus = (1u << 16);
    ohci->rhport[0] |= RH_PS_PPS;
    uint32_t potpgt = (ohci->rha_des >> 24) & 0xFF;
    udelay(potpgt * 2000 + 5000);

    uint32_t st = ohci->rhport[0];
    uart_print("usb: ohci(idx=");
    uart_print_int(idx);
    uart_print(") port=0x");
    uart_print_hex(st);
    uart_print("\r\n");
    if (st & RH_PS_CCS) {
        uart_print("usb: DEVICE on OHCI(idx=");
        uart_print_int(idx);
        uart_print(")!\r\n");
    }
    return 0;
}

/* Сброс корневого порта (эталон usb_ohci_port_reset). */
static int usb_ohci_port_reset_port(uint32_t base)
{
    ohci_regs_t *ohci = (ohci_regs_t *)base;
    ohci->rhport[0] |= RH_PS_PRS;
    udelay(50000);
    for (uint32_t t = 0; t < 200000; t++) {
        uint32_t st = ohci->rhport[0];
        if (!(st & RH_PS_PRS)) {
            ohci->rhport[0] = RH_PS_CSC | RH_PS_PRSC;
            udelay(100000);
            st = ohci->rhport[0];
            uart_print("usb: port after reset=0x");
            uart_print_hex(st);
            uart_print("\r\n");
            if (!(st & RH_PS_CCS)) { uart_print("usb: device gone\r\n"); return -1; }
            if (!(st & RH_PS_PES)) { uart_print("usb: PES not set\r\n"); return -1; }
            return 0;
        }
        udelay(1);
    }
    uart_print("usb: port reset timeout\r\n");
    return -1;
}

/* Control-передача на порту (эталон usb_ohci_ctrl_transfer). */
static int usb_ohci_ctrl_port(uint32_t base, uint8_t addr,
                              const uint8_t *setup, uint8_t setup_len,
                              uint8_t *data, uint32_t data_len, int dir_in,
                              uint32_t timeout_ms, int verbose)
{
    int idx = ohci_idx(base);
    if (idx < 0) return -1;
    ohci_regs_t *ohci = (ohci_regs_t *)base;
    if (!setup || setup_len == 0) return -1;
    if (data_len > 0 && !data) return -1;

    memset(g_td[idx], 0, sizeof(g_td[idx]));
    ohci_td_t *t_setup = &g_td[idx][0];
    ohci_td_t *t_data  = &g_td[idx][1];
    ohci_td_t *t_stat  = &g_td[idx][2];
    ohci_td_t *t_dummy = &g_td[idx][3];
    ohci_ed_t *ed = &g_head_ed[idx];

    int low_speed = (ohci->rhport[0] & RH_PS_LSDA) ? 1 : 0;

    ed->cfg = (addr & 0x7F)
            | (low_speed ? ED_LOWSPEED : 0)
            | ED_FROM_TD
            | ED_SKIP
            | ((uint32_t)g_mps[idx] << 16);
    ed->next = 0;

    t_setup->cfg = (TD_CC_NOTACC << TD_CC_SHIFT) | TD_T_DATA0 | TD_DP_SETUP;
    t_setup->cbp = (uint32_t)(uintptr_t)setup;
    t_setup->be  = (uint32_t)(uintptr_t)(setup + setup_len - 1);
    t_setup->next = (uint32_t)(uintptr_t)t_data;

    if (data_len > 0) {
        uint32_t dp = dir_in ? TD_DP_IN : TD_DP_OUT;
        t_data->cfg = (TD_CC_NOTACC << TD_CC_SHIFT) | TD_T_DATA1 | dp | (dir_in ? TD_R : 0);
        t_data->cbp = (uint32_t)(uintptr_t)data;
        t_data->be  = (uint32_t)(uintptr_t)(data + data_len - 1);
        t_data->next = (uint32_t)(uintptr_t)t_stat;
        t_stat->cfg = (TD_CC_NOTACC << TD_CC_SHIFT) | TD_T_DATA1 | (dir_in ? TD_DP_OUT : TD_DP_IN);
        t_stat->cbp = t_stat->be = 0;
        t_stat->next = (uint32_t)(uintptr_t)t_dummy;
    } else {
        t_data->cfg = (TD_CC_NOTACC << TD_CC_SHIFT) | TD_T_DATA1 | (dir_in ? TD_DP_OUT : TD_DP_IN);
        t_data->cbp = t_data->be = 0;
        t_data->next = (uint32_t)(uintptr_t)t_dummy;
    }
    t_dummy->cfg = (TD_CC_NOTACC << TD_CC_SHIFT);
    t_dummy->cbp = t_dummy->be = 0;
    t_dummy->next = (uint32_t)(uintptr_t)t_dummy;

    ed->head = (uint32_t)(uintptr_t)t_setup;
    ed->tail = (uint32_t)(uintptr_t)t_dummy;
    ed->cfg &= ~ED_SKIP;

    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    cache_clean((uint32_t)(uintptr_t)g_td[idx], sizeof(g_td[idx]));
    cache_clean((uint32_t)(uintptr_t)setup, setup_len);
    if (data_len > 0 && !dir_in) cache_clean((uint32_t)(uintptr_t)data, data_len);
    __asm volatile("dmb" ::: "memory");

    ohci->ctrlhead = (uint32_t)(uintptr_t)ed;
    ohci->ctrlcur  = (uint32_t)(uintptr_t)ed;
    ohci->cmdstatus = (1u << 1);   /* CLF */

    ohci_td_t *last = (data_len > 0) ? t_stat : t_data;
    uint32_t elapsed = 0;
    for (;;) {
        cache_invalidate((uint32_t)(uintptr_t)g_td[idx], sizeof(g_td[idx]));
        if (((last->cfg >> TD_CC_SHIFT) & 0xFu) != TD_CC_NOTACC) break;
        udelay(1000);
        if (++elapsed > timeout_ms) {
            if (verbose) {
                uart_print("usb: ctrl TIMEOUT idx=");
                uart_print_int(idx);
                uart_print(" addr=");
                uart_print_int((int)addr);
                uart_print(" dir=");
                uart_print_int(dir_in);
                uart_print(" len=");
                uart_print_int((int)data_len);
                uart_print("\r\n");
                uart_print("  ed.head=0x");
                uart_print_hex(ed->head);
                uart_print(" ed.tail=0x");
                uart_print_hex(ed->tail);
                uart_print(" ed.cfg=0x");
                uart_print_hex(ed->cfg);
                uart_print("\r\n");
                uart_print("  td setup cc=0x");
                uart_print_hex(t_setup->cfg >> TD_CC_SHIFT);
                uart_print(" data cc=0x");
                uart_print_hex((data_len > 0 ? t_data->cfg : last->cfg) >> TD_CC_SHIFT);
                uart_print(" stat cc=0x");
                uart_print_hex(t_stat->cfg >> TD_CC_SHIFT);
                uart_print("\r\n");
                uart_print("  cmdstatus=0x");
                uart_print_hex(ohci->cmdstatus);
                uart_print(" ctrlcur=0x");
                uart_print_hex(ohci->ctrlcur);
                uart_print("\r\n");
            }
            return -1;
        }
    }

    uint32_t cc;
    cc = t_setup->cfg >> TD_CC_SHIFT;
    if (cc != TD_CC_NOERR) return -1;
    cc = (last->cfg >> TD_CC_SHIFT) & 0xFu;
    if (cc != TD_CC_NOERR) return -1;

    if (data_len > 0 && dir_in) cache_invalidate((uint32_t)(uintptr_t)data, data_len);
    return (int)data_len;
}

/* ---- Bulk (MSD) на OHCI для FS/LS-устройств ---- */
static ohci_ed_t g_bulk_ed[OHCI_NPORTS][2] __attribute__((section(".coherent"), aligned(16)));
static ohci_td_t g_bulk_td[OHCI_NPORTS][2][2] __attribute__((section(".coherent"), aligned(32)));
/* Явные toggle (0=DATA0, 1=DATA1) на порт/направление: ED carry на H3
 * обновляется ненадёжно, из-за чего CSW уходил с неверным toggle (CC=3). */
static uint8_t g_bulk_toggle[OHCI_NPORTS][2];

static int usb_ohci_bulk(uint32_t base, uint8_t addr, uint8_t ep,
                         const uint8_t *out, uint8_t *in,
                         uint32_t len, uint16_t mps, int dir_in)
{
    int idx = ohci_idx(base);
    if (idx < 0 || len == 0 || mps == 0) return -1;
    ohci_regs_t *ohci = (ohci_regs_t *)base;
    int low_speed = (ohci->rhport[0] & RH_PS_LSDA) ? 1 : 0;
    int di = dir_in ? 1 : 0;
    ohci_ed_t *ed = &g_bulk_ed[idx][di];
    ohci_td_t *td = &g_bulk_td[idx][di][0];
    ohci_td_t *dum = &g_bulk_td[idx][di][1];
    uint8_t tog = g_bulk_toggle[idx][di];

    memset(&g_bulk_td[idx][di], 0, sizeof(g_bulk_td[idx][di]));
    /* Направление задаём в ED (D=IN/OUT), а не «из TD» (D=00): эталон и
     * Linux для bulk всегда пишут D в ED; с D=00 H3-OHCI терял IN-передачу
     * (TD оставался NotAccessed, снаружи это выглядело как таймаут). */
    ed->cfg = (uint32_t)(addr & 0x7F)
            | ((uint32_t)(ep & 0x0F) << 7)
            | (dir_in ? ED_IN : ED_OUT)
            | (low_speed ? ED_LOWSPEED : 0)
            | ((uint32_t)mps << 16);
    ed->next = 0;

    td->cfg = (TD_CC_NOTACC << TD_CC_SHIFT)
            | (tog ? TD_T_DATA1 : TD_T_DATA0)
            | (dir_in ? (TD_DP_IN | TD_R) : TD_DP_OUT);
    td->cbp = (uint32_t)(uintptr_t)(dir_in ? in : out);
    td->be  = td->cbp + len - 1;
    td->next = (uint32_t)(uintptr_t)dum;

    dum->cfg = (TD_CC_NOTACC << TD_CC_SHIFT);
    dum->cbp = dum->be = 0;
    dum->next = (uint32_t)(uintptr_t)dum;

    ed->cfg |= ED_SKIP;
    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    ed->head = (uint32_t)(uintptr_t)td;
    ed->tail = (uint32_t)(uintptr_t)dum;
    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    ed->cfg &= ~ED_SKIP;
    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    cache_clean((uint32_t)(uintptr_t)&g_bulk_td[idx][di], sizeof(g_bulk_td[idx][di]));
    if (!dir_in) cache_clean((uint32_t)(uintptr_t)out, len);
    __asm volatile("dmb" ::: "memory");

    ohci->bulkhead = (uint32_t)(uintptr_t)ed;
    ohci->bulkcur  = (uint32_t)(uintptr_t)ed;
    ohci->cmdstatus |= (1u << 2);   /* BLF */

    uint32_t elapsed = 0;
    for (;;) {
        cache_invalidate((uint32_t)(uintptr_t)td, sizeof(td[0]));
        if (((td->cfg >> TD_CC_SHIFT) & 0xFu) != TD_CC_NOTACC) break;
        udelay(1000);
        if (++elapsed > 2000) {
            /* TD не завершён: либо НС не видел ED, либо устройство бесконечно
               NAK'ает (toggle/фаза BOT). Печатаем состояние для разбора. */
            uart_print("ohci-bulk: TIMEOUT dir=");
            uart_print_int(dir_in);
            uart_print(" ep=");
            uart_print_int((int)ep);
            uart_print(" len=");
            uart_print_int((int)len);
            uart_print(" edhead=0x");
            uart_print_hex(ed->head);
            uart_print(" edtail=0x");
            uart_print_hex(ed->tail);
            uart_print(" cmdst=0x");
            uart_print_hex(ohci->cmdstatus);
            uart_print("\r\n");
            return -1;
        }
    }
    uint32_t cc = (td->cfg >> TD_CC_SHIFT) & 0xFu;
    if (dir_in) cache_invalidate((uint32_t)(uintptr_t)in, len);
    if (cc == TD_CC_NOERR) {
        /* Следующий toggle: число пакетов в передаче (BOT: точные длины). */
        uint32_t packets = (len + mps - 1) / mps;
        g_bulk_toggle[idx][di] = (uint8_t)(tog ^ (packets & 1u));
    }
    if (cc != TD_CC_NOERR) {
        uart_print("ohci-bulk: fail dir=");
        uart_print_int(dir_in);
        uart_print(" ep=");
        uart_print_int((int)ep);
        uart_print(" cc=0x");
        uart_print_hex(cc);
        uart_print(" len=");
        uart_print_int((int)len);
        uart_print(" h=");
        uart_print_int((int)(ed->head & 1u));   /* ED.Halted */
        uart_print("\r\n");
    }
    return (cc == TD_CC_NOERR) ? 0 : -1;
}

int usb_ohci_bulk_out(uint32_t base, uint8_t addr, uint8_t ep,
                      const uint8_t *data, uint32_t len, uint16_t mps)
{
    return usb_ohci_bulk(base, addr, ep, data, 0, len, mps, 0);
}

int usb_ohci_bulk_in(uint32_t base, uint8_t addr, uint8_t ep,
                     uint8_t *data, uint32_t len, uint16_t mps)
{
    return usb_ohci_bulk(base, addr, ep, 0, data, len, mps, 1);
}

void usb_ohci_bulk_toggle_reset(uint32_t base)
{
    int idx = ohci_idx(base);
    if (idx < 0) return;
    g_bulk_toggle[idx][0] = 0;
    g_bulk_toggle[idx][1] = 0;
}

/* ---- публичный API (2 порта) ---- */

int usb_ohci_init_port1(void) { return usb_ohci_init_port(OHCI1_BASE); }
int usb_ohci_init_port2(void) { return usb_ohci_init_port(OHCI2_BASE); }

int usb_ohci_port_connected(uint32_t base)
{
    ohci_regs_t *ohci = (ohci_regs_t *)base;
    return (ohci->rhport[0] & RH_PS_CCS) ? 1 : 0;
}

int usb_ohci_port_connected1(void) { return usb_ohci_port_connected(OHCI1_BASE); }
int usb_ohci_port_connected2(void) { return usb_ohci_port_connected(OHCI2_BASE); }

int usb_ohci_port_reset_all1(void) { return usb_ohci_port_reset_port(OHCI1_BASE); }
int usb_ohci_port_reset_all2(void) { return usb_ohci_port_reset_port(OHCI2_BASE); }

int usb_ohci_ctrl_addr0(uint32_t base, const uint8_t *setup, uint8_t setup_len,
                        uint8_t *data, uint32_t data_len, int dir_in)
{
    return usb_ohci_ctrl_port(base, 0, setup, setup_len, data, data_len, dir_in, 2000, 1);
}

int usb_ohci_ctrl_transfer(uint32_t base, uint8_t addr,
                           const uint8_t *setup, uint8_t setup_len,
                           uint8_t *data, uint32_t data_len, int dir_in,
                           uint32_t timeout_ms)
{
    return usb_ohci_ctrl_port(base, addr, setup, setup_len, data, data_len, dir_in, timeout_ms, 1);
}

uint8_t g_usb_addr = 0;

/* ---- Периодический interrupt-IN (HID) — паттерн эталона pico-retro ----
 * ED с DIR=IN + MPS в периодическом списке (HCCA interrupt table),
 * TD (DP_IN + R, NOTACC). Poll: CC != NOTACC -> свежий пакет; переарм. */

int usb_ohci_intr_in_start(uint32_t base, uint8_t addr, uint8_t ep,
                           uint8_t *buf, uint16_t len, int slot)
{
    if (slot < 0 || slot >= 2 || !buf || len == 0) return -1;
    int idx = ohci_idx(base);
    if (idx < 0) return -1;
    ohci_regs_t *ohci = (ohci_regs_t *)base;
    int low_speed = (ohci->rhport[0] & RH_PS_LSDA) ? 1 : 0;

    /* Периодический список выключить на время переконфигурации ED/TD:
       перезапуск может вызываться watchdog'ом, когда ED уже в списке. */
    ohci->ctrl &= ~(1u << 2);   /* PLE = 0 */
    __asm volatile("dsb" ::: "memory");

    ohci_ed_t *ed  = &g_int_ed[idx][slot];
    ohci_td_t *td  = &g_int_td[idx][slot][0];
    ohci_td_t *dum = &g_int_td[idx][slot][1];

    memset(ed, 0, sizeof(*ed));
    memset(&g_int_td[idx][slot], 0, sizeof(g_int_td[idx][slot]));

    ed->cfg = (addr & 0x7F)
            | ((uint32_t)(ep & 0x0F) << 7)
            | ED_IN                 /* DIR=IN */
            | (low_speed ? ED_LOWSPEED : 0)
            | ((uint32_t)len << 16);
    ed->head = (uint32_t)(uintptr_t)td;
    ed->tail = (uint32_t)(uintptr_t)dum;
    ed->next = 0;

    td->cfg = (TD_CC_NOTACC << TD_CC_SHIFT)
            | (g_int_toggle[idx][slot] ? TD_T_DATA1 : TD_T_DATA0)
            | TD_DP_IN | TD_R;
    td->cbp = (uint32_t)(uintptr_t)buf;
    td->be  = (uint32_t)(uintptr_t)(buf + len - 1);
    td->next = (uint32_t)(uintptr_t)dum;

    dum->cfg = (TD_CC_NOTACC << TD_CC_SHIFT);
    dum->cbp = dum->be = 0;
    dum->next = (uint32_t)(uintptr_t)dum;

    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    cache_clean((uint32_t)(uintptr_t)&g_int_td[idx][slot], sizeof(g_int_td[idx][slot]));
    cache_clean((uint32_t)(uintptr_t)buf, len);

    if (slot == 0) {
        g_int_ed[idx][0].next = (uint32_t)(uintptr_t)&g_int_ed[idx][1];
        cache_clean((uint32_t)(uintptr_t)&g_int_ed[idx][0], sizeof(g_int_ed[idx][0]));
        for (int i = 0; i < 32; i++) g_hcca[idx].intr[i] = (uint32_t)(uintptr_t)&g_int_ed[idx][0];
        cache_clean((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
        __asm volatile("dmb" ::: "memory");
        ohci->peried = (uint32_t)(uintptr_t)&g_int_ed[idx][0];
    }
    /* Включить периодический список (для обоих слотов: PLE глобальный). */
    ohci->ctrl |= (1u << 2);
    __asm volatile("dsb" ::: "memory");
    return 0;
}

int usb_ohci_intr_in_poll(uint32_t base, uint8_t *buf, uint16_t len, int slot)
{
    if (slot < 0 || slot >= 2 || !buf || len == 0) return -1;
    int idx = ohci_idx(base);
    if (idx < 0) return -1;
    ohci_td_t *td = &g_int_td[idx][slot][0];

    cache_invalidate((uint32_t)(uintptr_t)td, sizeof(td[0]));
    uint32_t cc = td->cfg >> TD_CC_SHIFT;

    if (cc == TD_CC_NOTACC) return 0;   /* передача ещё идёт / пакета нет */

    /* Переарм (пакет или ошибка): вернуть TD в цепочку.
       ED активен в периодическом списке — менять head/tail можно только
       при SKIP=1 (иначе HC может не увидеть новый head и клавиатура
       «замолкает»). */
    /* Следующий пакет — с противоположным toggle (один пакет на отчёт:
       HID-отчёт 8 байт при MPS 8/16/64). При ошибке toggle не меняем. */
    if (cc == TD_CC_NOERR) g_int_toggle[idx][slot] ^= 1;

    uint32_t cfg = (TD_CC_NOTACC << TD_CC_SHIFT)
                 | (g_int_toggle[idx][slot] ? TD_T_DATA1 : TD_T_DATA0)
                 | TD_DP_IN | TD_R;
    td->cfg = cfg;
    td->cbp = (uint32_t)(uintptr_t)buf;
    td->be  = (uint32_t)(uintptr_t)(buf + len - 1);
    td->next = (uint32_t)(uintptr_t)&g_int_td[idx][slot][1];
    cache_clean((uint32_t)(uintptr_t)td, sizeof(td[0]));
    cache_clean((uint32_t)(uintptr_t)buf, len);

    ohci_ed_t *ed = &g_int_ed[idx][slot];
    ed->cfg |= ED_SKIP;
    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    ed->head = (uint32_t)(uintptr_t)td;
    ed->tail = (uint32_t)(uintptr_t)&g_int_td[idx][slot][1];
    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
    ed->cfg &= ~ED_SKIP;
    cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));

    if (cc != TD_CC_NOERR) return -1;   /* ошибка завершения */

    cache_invalidate((uint32_t)(uintptr_t)buf, len);
    return 1;
}