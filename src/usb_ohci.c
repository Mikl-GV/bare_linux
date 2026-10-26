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
/* Bulk (MSD) для FS/LS-устройств: [порт][направление 0=OUT/1=IN][TD]. */
static ohci_ed_t g_bulk_ed[OHCI_NPORTS][2] __attribute__((section(".coherent"), aligned(16)));
static ohci_td_t g_bulk_td[OHCI_NPORTS][2][2] __attribute__((section(".coherent"), aligned(32)));
/* Явный toggle interrupt-IN (0=DATA0, 1=DATA1). ED carry на H3 не
 * обновляется — без этого после первого отчёта получаем mismatch. */
static uint8_t g_int_toggle[OHCI_NPORTS][2];
static uint16_t    g_mps[OHCI_NPORTS] = { 8, 8 };
/* Скорость и EP0-MPS запоминаем за адресом: после подключения LS-устройства
 * LSDA корневого порта описывает уже другое устройство, и ED, построенный по
 * нему, уезжает на флешку как Low-Speed (TD не обрабатывается — таймаут). */
static uint8_t     g_dev_ls[OHCI_NPORTS][128];
static uint8_t     g_dev_mps[OHCI_NPORTS][128];

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
extern void dma_clean_range(uint32_t addr, uint32_t len);
extern void dma_inval_range(uint32_t addr, uint32_t len);

/* Синхронизация для DMA. Раньше здесь был только L1 (DCCMVAC), и на H3 этого
 * НЕДОСТАТОЧНО: строка уходит в L2C-310, а HC читает DRAM напрямую. Из-за
 * этого HC «не видел» ED/TD (bc=0, TD NotAccessed), хотя CPU читал корректные
 * значения. dma_* чистят/инвалидируют и L1, и L2 (by-PA). */
static void cache_clean(uint32_t addr, uint32_t size)
{
    dma_clean_range(addr, size);
}

static void cache_invalidate(uint32_t addr, uint32_t size)
{
    dma_inval_range(addr, size);
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

/* Инициализация тактов/PHY порта (1 или 2) — эталон mainline Linux
 * drivers/phy/allwinner/phy-sun4i-usb.c (sun4i_usb_phy_init) + U-Boot.
 *
 * Главное отличие от старой версии: SIDDQ (pmuN+0x10 bit1) снимается в
 * самом начале, ДО сброса тактов/PHY и ДО root-port reset. В старой
 * версии SIDDQ чистился позже (в usb_ehci_init_bus), уже ПОСЛЕ сброса
 * PHY — на уже инициализированной U-Boot'ом шине это ломало HS-chirp:
 * флешка уходила на FS/OHCI, и bulk на OHCI не работал (td NotAccessed).
 */
static void usb_port_hw_init(int port)
{
    if (port < 1 || port > 2) return;
    uint32_t e_bit = (1u << (24 + port));
    uint32_t o_bit = (1u << (28 + port));
    uint32_t phy_rst = (1u << port);
    uint32_t phy_cal = (1u << (8 + port));
    uint32_t phy_clk = (1u << (16 + port));
    uint32_t ehci_base = (port == 1) ? PHY1_EHCI_BASE : PHY2_EHCI_BASE;
    uint32_t pmu_base  = (port == 1) ? USB_PMU1_BASE : USB_PMU2_BASE;

    /* 1) SIDDQ = 0: вывести PHY из powerdown ПЕРВЫМ (эталон sun4i_usb_phy_init:
          «val &= ~PHY_CTL_H3_SIDDQ; writel(...)» в начале init, до калибровки). */
    *(volatile uint32_t *)(pmu_base + 0x10) &= ~USB_PHY_CTL_SIDDQ;
    __asm volatile("dsb" ::: "memory");
    udelay(1000);

    /* 2) Такты/сбросы (ensure enabled; U-Boot мог уже поднять — идемпотентно). */
    H3_CCU_GATING0 |= e_bit | o_bit;
    H3_CCU_RST0    |= e_bit | o_bit;
    USB_CCU_PHY_CFG |= phy_rst | phy_cal | phy_clk;
    __asm volatile("dsb" ::: "memory");
    udelay(30000);

    /* 3) Passby (AHB burst + ULPI bypass = 0x701) — эталон sun4i_usb_phy_passby. */
    volatile uint32_t *icr = (volatile uint32_t *)(ehci_base + 0x800);
    icr[0] = 0x00000701u;
    icr[4] = 0;
    __asm volatile("dsb" ::: "memory");

    /* 4) PHY tuning (Linux sun4i_usb_phy_init, H3, disc_thresh=3):
       TX amplitude tune = 0x20 (5 бит) = 0x14, disconnect threshold
       0x2a (2 бита) = 3. Без этого HS-флешка может определиться как FS. */
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
    /* Bulk и interrupt-структуры тоже обязаны быть некэшируемыми: их читает
       сам HC (DMA). Раньше они полагались на общий обход .coherent в i2s_init —
       зависимость от порядка инициализации; помечаем явно. */
    mmu_mark_uncached((uint32_t)&g_bulk_ed[idx][0]);
    mmu_mark_uncached((uint32_t)&g_bulk_td[idx][0][0]);
    mmu_mark_uncached((uint32_t)&g_int_ed[idx][0]);
    mmu_mark_uncached((uint32_t)&g_int_td[idx][0][0]);

    usb_port_hw_init(idx + 1);

    if (ohci->rev != 0x10) { uart_print("usb: OHCI dead\r\n"); return -1; }

    ohci->intrdisable = 0xFFFFFFFFu;

    /* ПРИВЕСТИ HC В UsbReset АППАРАТНЫМ СБРОСОМ (HcCommandStatus.HCR).
       Запись HCFS=UsbReset из Operational игнорируется (спецификация OHCI
       разрешает этот переход только из Suspend), поэтому HCCA не
       переключалась: в логе fr=0 при hcca=0x40101E00 — HC продолжал писать
       done_head в HCCA U-Boot, наш сброс WDH уходил «в пустоту» и bulk не
       продвигался. HCR сбрасывает внутреннее состояние HC (указатели
       списков, тайминги), root port при этом НЕ трогается. */
    ohci->cmdstatus = (1u << 0);                       /* HCR */
    __asm volatile("dsb" ::: "memory");
    for (uint32_t t = 0; t < 1000u && (ohci->cmdstatus & 1u); t++) udelay(10);
    udelay(2000);

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
    /* Списки control тоже обнуляем: U-Boot после usb start оставил здесь свои
       ED в уже освобождённой памяти. HC продолжает обходить их и в кванты,
       отведённые bulk (CBSR), не переключается. Наши control-передачи всё
       равно ставят ctrlhead/ctrlcur в своей передаче. */
    ohci->ctrlhead = 0;
    ohci->ctrlcur  = 0;
    ohci->donehead = 0;

    ohci->ctrl = (ohci->ctrl & ~(3u << 6)) | (2u << 6) | (1u << 4) | (1u << 5);
    /* bit4 = CLE (control list), bit5 = BLE (bulk list — нужен для MSD). */
    udelay(3000);

    /* Подтверждение, что HC реально работает с НАШЕЙ HCCA: HccaFramenumber
       инкрементируется каждую мс. HcFmNumber читаем из MMIO (кэш не влияет),
       HccaFramenumber — С ИНВАЛИДАЦИЕЙ кэша: если он не растёт, а регистр
       растёт, значит либо HC пишет в чужую HCCA, либо наша секция кэшируемая.
       L1-дескриптор 0x40100000 показывает, применилась ли пометка uncached. */
    {
        uint32_t fm0 = ohci->fmnumber & 0xFFFFu;
        cache_invalidate((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
        uint16_t f0 = g_hcca[idx].framenumber;
        udelay(5000);
        uint32_t fm1 = ohci->fmnumber & 0xFFFFu;
        cache_invalidate((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
        uint16_t f1 = g_hcca[idx].framenumber;

        uint32_t l1b;
        __asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(l1b));
        l1b &= ~0x3FFFu;
        uint32_t desc = ((volatile uint32_t *)l1b)[(0x40100000u >> 20) & 0xFFFu];

        uart_print("usb: ohci");
        uart_print_int(idx + 1);
        uart_print(" fm ");
        uart_print_int((int)fm0);
        uart_print("->");
        uart_print_int((int)fm1);
        uart_print(" fr ");
        uart_print_int(f0);
        uart_print("->");
        uart_print_int(f1);
        uart_print(" l1=0x");
        uart_print_hex(desc);
        uart_print(" ctl=0x");
        uart_print_hex(ohci->ctrl);
        uart_print("\r\n");
    }

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
    return 0;
}

/* Аппаратный сброс контроллера (HcCommandStatus.HCR). Зависшие current-ED:
 * лог показывал ctrlcur=0x40101F40 (чужой адрес) и ED.SKIP, когда списки
 * переставали опустошаться. HCR снимает состояние HC БЕЗ сброса root port —
 * устройство остаётся подключено.
 *
 * ВАЖНО (лог WU.1.176): HCR НА ЭТОМ H3 СБРАСЫВАЕТ HcHCCA и указатели
 * списков в 0. Без их восстановления HC писал done-head по адресу 0
 * (hcca=0x00000000, fr замер, списки не продвигались). Поэтому после HCR
 * обязательно восстанавливаем HcHCCA и зануляем все head/current/done —
 * это разрывает NAK-loop на устройстве/застрявший ED и возвращает SB. */
void usb_ohci_hc_reset(uint32_t base)
{
    int idx = ohci_idx(base);
    if (idx < 0) return;
    ohci_regs_t *ohci = (ohci_regs_t *)base;

    ohci->cmdstatus |= (1u << 0);              /* HCR = 1 */
    __asm volatile("dsb" ::: "memory");
    for (uint32_t t = 0; t < 100000u; t++) {
        if (!(ohci->cmdstatus & (1u << 0))) break;   /* HC сбросил флаг */
        udelay(1);
    }
    uint32_t fi = 0x2edf;
    uint32_t fsmps = (6 * (fi - 210)) / 7;
    ohci->ctrl = (ohci->ctrl & ~(3u << 6)) | (2u << 6);  /* HCFS=Operational */
    ohci->fminterval = (fsmps << 16) | fi;
    ohci->fminterval ^= (1u << 31);
    ohci->periodstart = (fi * 9) / 10;
    ohci->lsperiod = 0x628;

    /* Вернуть HCCA (HCR обнулил) — иначе done-head пишется в 0. */
    ohci->hcca = (uint32_t)(uintptr_t)&g_hcca[idx];
    /* Занулить ВСЕ указатели списков: current/head/done в регистрах. */
    ohci->ctrlhead = 0;
    ohci->ctrlcur  = 0;
    ohci->bulkhead = 0;
    ohci->bulkcur  = 0;
    ohci->donehead = 0;
    /* Сбросить done-очередь в HCCA (в RAM, не кэшируемо через L2-операции). */
    g_hcca[idx].donehead = 0;
    /* dma_clean_range чистит и L1, и L2 (PL310) — HC читает donehead из DRAM. */
    dma_clean_range((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));

    ohci->ctrl |= (1u << 4) | (1u << 5);       /* CLE + BLE */
    __asm volatile("dsb" ::: "memory");
    udelay(1000);

    /* hc_reset не трогает HcInterruptStatus. Если там «залип» WDH (Writeback
     * Done Head, bit1) — HC считает done-очередь необслуженной и не продвигает
     * списки: наш bulk-TD вечно NotAccessed (лог: isr=0x06 без сброса).
     * W1C: записываем 1 во все биты, чтобы снять и WDH, и прочие флаги. */
     ohci->intrstatus = 0xFFFFFFFFu;
     __asm volatile("dsb" ::: "memory");
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

    /* Та же гонка, что и в bulk: пока HC держит control-список, ED переписывать
       нельзя — контроллер застревает на старом ED (ctrlcur != наш), и дальше
       валятся все control-передачи, включая BOT-recovery. Гасим CLE и ждём. */
    ohci->ctrl &= ~(1u << 4);
    __asm volatile("dsb" ::: "memory");
    for (uint32_t w = 0; w < 2000u && ohci->ctrlcur; w++) udelay(10);

    memset(g_td[idx], 0, sizeof(g_td[idx]));
    ohci_td_t *t_setup = &g_td[idx][0];
    ohci_td_t *t_data  = &g_td[idx][1];
    ohci_td_t *t_stat  = &g_td[idx][2];
    ohci_td_t *t_dummy = &g_td[idx][3];
    ohci_ed_t *ed = &g_head_ed[idx];

    /* Адрес 0 — устройство, которое сейчас на корневом порту: его скорость
       и есть LSDA. Для остальных адресов берём запомненное при enum. */
    uint8_t a = (uint8_t)(addr & 0x7F);
    if (a == 0) {
        g_dev_ls[idx][0]  = (ohci->rhport[0] & RH_PS_LSDA) ? 1u : 0u;
        g_dev_mps[idx][0] = (uint8_t)g_mps[idx];
    }
    if (!g_dev_mps[idx][a]) {
        g_dev_ls[idx][a]  = g_dev_ls[idx][0];
        g_dev_mps[idx][a] = g_dev_mps[idx][0];
    }
    int low_speed = g_dev_ls[idx][a];

    ed->cfg = (addr & 0x7F)
            | (low_speed ? ED_LOWSPEED : 0)
            | ED_FROM_TD
            | ED_SKIP
            | ((uint32_t)g_dev_mps[idx][a] << 16);
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

    /* ОЧИСТИТЬ DONE-ОЧЕРЕДЬ ПЕРЕД ЗАПУСКОМ CONTROL. После сбоя bulk
       (CSW fail) в HCCA.DoneHead/регистре остаётся TD, и HC не продвигает
       НИКАКОЙ список, пока она непуста — из-за этого BOT recovery висел
       на «ctrl TIMEOUT ... cur=0x40102200, cmdst=0x02». Зеркально bulk:
       инвалидация -> обнулить donehead в HCCA и регистре -> W1C WDH. */
    cache_invalidate((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
    g_hcca[idx].donehead = 0;
    cache_clean((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
    ohci->donehead = 0;
    __asm volatile("dsb" ::: "memory");
    ohci->intrstatus = (1u << 1);   /* W1C: снять WDH */

    ohci->ctrlhead = (uint32_t)(uintptr_t)ed;
    ohci->ctrlcur  = (uint32_t)(uintptr_t)ed;
    ohci->ctrl |= (1u << 4);        /* CLE: список снова активен */
    __asm volatile("dsb" ::: "memory");
    ohci->cmdstatus = (1u << 1);   /* CLF */

    ohci_td_t *last = (data_len > 0) ? t_stat : t_data;
    uint32_t elapsed = 0;
    for (;;) {
        cache_invalidate((uint32_t)(uintptr_t)g_td[idx], sizeof(g_td[idx]));
        if (((last->cfg >> TD_CC_SHIFT) & 0xFu) != TD_CC_NOTACC) break;
        udelay(1000);
        if (++elapsed > timeout_ms) {
            if (verbose) {
                uart_print("usb: ctrl TIMEOUT addr=");
                uart_print_int((int)addr);
                uart_print(" dir=");
                uart_print_int(dir_in);
                uart_print(" len=");
                uart_print_int((int)data_len);
                uart_print(" cc=");
                uart_print_hex(t_setup->cfg >> TD_CC_SHIFT);
                uart_print("/");
                uart_print_hex((data_len > 0 ? t_data->cfg : last->cfg) >> TD_CC_SHIFT);
                uart_print("/");
                uart_print_hex(t_stat->cfg >> TD_CC_SHIFT);
                uart_print(" cmdst=0x");
                uart_print_hex(ohci->cmdstatus);
                uart_print(" cur=0x");
                uart_print_hex(ohci->ctrlcur);
                uart_print(" ed=0x");
                uart_print_hex(ed->cfg);
                uart_print(" h=");
                uart_print_int((int)(ed->head & 1u));
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
    if (setup[1] == 0x05u) {   /* SET_ADDRESS: переносим скорость на новый адрес */
        uint8_t na = (uint8_t)(setup[2] & 0x7F);
        if (na) {
            g_dev_ls[idx][na]  = g_dev_ls[idx][0];
            g_dev_mps[idx][na] = g_dev_mps[idx][0];
        }
    }
    return (int)data_len;
}

/* ---- Bulk (MSD) на OHCI для FS/LS-устройств ---- */
/* Явный toggle bulk-эндпоинта (0=DATA0, 1=DATA1).
 * ОДИН на весь BOT-поток CBW(OUT)->DATA(IN)->CSW(IN): ED carry на H3
 * обновляется ненадёжно, а единый на порт счётчик спецификации противоречит:
 * bulk-эндпоинт — это пара (номер, направление), у EP-OUT и EP-IN
 * НЕЗАВИСИМЫЕ toggle. Последняя попытка «один toggle на порт» дала
 * «short IN got=23 want=36» уже на INQUIRY (первый IN-пакет после CBW
 * ушёл с DATA1 вместо DATA0). Переключение — ПО НАПРАВЛЕНИЮ, после КАЖДОЙ
 * успешной передачи по числу пакетов. */
static uint8_t g_bulk_toggle[OHCI_NPORTS][2];

static int usb_ohci_bulk(uint32_t base, uint8_t addr, uint8_t ep,
                         const uint8_t *out, uint8_t *in,
                         uint32_t len, uint16_t mps, int dir_in)
{
    int idx = ohci_idx(base);
    if (idx < 0 || len == 0 || mps == 0) return -1;
    ohci_regs_t *ohci = (ohci_regs_t *)base;
    /* Bulk-передач на Low-Speed не бывает (LS — только control/interrupt),
     * поэтому ED_LOWSPEED тут не ставим никогда. Раньше флаг брался из
     * RH_PS_LSDA корневого порта, и после подключения LS-клавиатуры
     * bulk-ED флешки уезжал как LS — TD оставался NotAccessed (таймаут). */
    int low_speed = 0;
    int di = dir_in ? 1 : 0;
    ohci_ed_t *ed = &g_bulk_ed[idx][di];
    ohci_td_t *td = &g_bulk_td[idx][di][0];
    ohci_td_t *dum = &g_bulk_td[idx][di][1];
    uint8_t tog = g_bulk_toggle[idx][di];

    /* Пока HC держит список, ED переписывать нельзя: контроллер может
       дочитать его ровно в момент, когда мы готовим следующий transfer
       (на высокой частоте ядро обгоняет HC — TD оставался NotAccessed и
       передача висела до таймаута). Гасим BLE и ждём освобождения ED. */
    ohci->ctrl &= ~(1u << 5);
    __asm volatile("dsb" ::: "memory");
    for (uint32_t w = 0; w < 2000u && ohci->bulkcur; w++) udelay(10);

    memset(&g_bulk_td[idx][di], 0, sizeof(g_bulk_td[idx][di]));
    /* Направление — СТРОГО из TD (ED_FROM_TD), дословно как в работающем
       control-пути (usb_ohci_ctrl_port: ed->cfg = ... | ED_FROM_TD, и TD
       несёт DP_IN/DP_OUT). На этом H3-OHCI запись направления в ED
       (ED_IN/ED_OUT) приводит к тому, что HC не берёт ED в current
       (bcur=0, TD NotAccessed) — контроллер согласован только с
       каноном «ED=from-TD + TD.DP». */
    ed->cfg = (uint32_t)(addr & 0x7F)
            | ((uint32_t)(ep & 0x0F) << 7)
            | ED_FROM_TD
            | (low_speed ? ED_LOWSPEED : 0)
            | ((uint32_t)mps << 16);
    ed->next = 0;

    /* IN: БЕЗ TD_R (Buffer Rounding). R=1 завершает TD по первому же NAK,
       не дожимает блок до 512 — от ровно 5 полных пакетов по 64 отдаёт
       «short IN got=320 want=512, cc=0x08» (медленная флешка на холодном
       старте делает паузу перед 6-м пакетом) и десинкает BOT. Без R HC
       опрашивает сквозь NAK, пока буфер не заполнится или придёт короткий.
       OUT-передачи остаются с DP_OUT (R для них не значим). */
    td->cfg = (TD_CC_NOTACC << TD_CC_SHIFT)
            | (tog ? TD_T_DATA1 : TD_T_DATA0)
            | (dir_in ? TD_DP_IN : TD_DP_OUT);
    td->cbp = (uint32_t)(uintptr_t)(dir_in ? in : out);
    td->be  = td->cbp + len - 1;
    td->next = (uint32_t)(uintptr_t)dum;

    dum->cfg = (TD_CC_NOTACC << TD_CC_SHIFT);
    dum->cbp = dum->be = 0;
    /* САМОЦИКЛ dummy — как в РАБОТАЮЩЕМ control-пути (t_dummy->next = &t_dummy).
       На этом H3-OHCI единственный работоспособный паттерн — дословно по
       usb_ohci_ctrl_port: ED.current = адрес ED, список с самозамкнутым
       dummy, enable через RMW. Терминатор (next=0) здесь не работает:
       HC ждёт циклического dummy, чтобы завершить TD через WDH. */
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

    /* ПАУЗА ПЕРЕД ЗАПУСКОМ СПИСКА. L2-синхронизация (by-PA) и запись ED/TD
       занимают заметное время; если включить BLE сразу, HC успевает прочитать
       ED/TD раньше, чем они устоялись в DRAM — получаем CSW cc=8 (DataUnderrun)
       и зависание HC. В отладочной сборке эту роль играл трековый UART-лог
       (микропаузы); без него гонка воспроизводится стабильно. */
    udelay(200);

    /* ЗЕРКАЛЬНО control-пути: bulkcurrent = адрес ED (не 0), BLE через RMW,
       затем BLF прямой записью. done_head НЕ трогаем — control его не трогает
       и работает; лишние W1C/обнуления только рассинхронизируют HC. */
    ohci->bulkhead = (uint32_t)(uintptr_t)ed;
    ohci->bulkcur  = (uint32_t)(uintptr_t)ed;
    ohci->ctrl |= (1u << 5);        /* BLE */
    __asm volatile("dsb" ::: "memory");

    /* DONE-ОЧЕРЕДЬ ОБЯЗАТЕЛЬНА К ОБНУЛЕНИЮ ДО BLF.
       Трейс показал: в HccaDoneHead лежит завершённый control-TD
       (hccadh=0x40102000, dh=0x40102020), оставшийся от enum/BOT-recovery.
       Пока done-очередь непуста, HC НЕ продвигает списки — он забирает наш
       ED в current и сразу отпускает (bc=0x40101C00 -> 0), TD остаётся
       NotAccessed. При этом .coherent фактически КЭШИРУЕМАЯ
       (l1=0xFFFFFFFF — пометка uncached не применилась), поэтому читать
       donehead без invalidate нельзя: видим 0 и «не замечаем» реальную
       очередь. Читаем с invalidate, обнуляем в DRAM с clean, снимаем WDH. */
    cache_invalidate((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
    if (g_hcca[idx].donehead != 0) {
        g_hcca[idx].donehead = 0;
        cache_clean((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
    }
    /* ОЧИСТИТЬ И РЕГИСТР HcDoneHead (0x30), не только HCCA.DoneHead.
       Полный лог показал: dh=0x40102020 залипает навсегда (control-TD g_td[1]),
       пока HCD чистит лишь HCCA. Пока HcDoneHead != 0, HC считает
       done-очередь неполной: берёт наш ED в current (bc=0x40101C00) и сразу
       отпускает, TD остаётся NotAccessed, WDH (isr bit1) висит постоянно. */
    ohci->donehead = 0;
    __asm volatile("dsb" ::: "memory");
    ohci->intrstatus = (1u << 1);    /* W1C: WDH */
    __asm volatile("dsb" ::: "memory");
    /* BLF: прямая запись (HcCommandStatus меняет сам HC). */
    ohci->cmdstatus = (1u << 2);   /* BLF */
    __asm volatile("dsb" ::: "memory");

    uint32_t elapsed = 0;
    for (;;) {
        /* Обслужить done-очередь HC: HCCA.DoneHead + регистр HcDoneHead.
           Чистим ОБА (иначе dh=0x40102020 висит и HC не выполняет TD),
           затем WDH (W1C) и повторный BLF. Чтение — с invalidate:
           .coherent фактически кэшируемая (l1=0xFFFFFFFF). */
        cache_invalidate((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
        if (g_hcca[idx].donehead != 0 || ohci->donehead != 0) {
            g_hcca[idx].donehead = 0;
            cache_clean((uint32_t)(uintptr_t)&g_hcca[idx], sizeof(g_hcca[idx]));
            ohci->donehead = 0;
            __asm volatile("dsb" ::: "memory");
            ohci->intrstatus = (1u << 1);   /* W1C: снять WDH */
            __asm volatile("dsb" ::: "memory");
            /* BLF после освобождения done-очереди — вернуть обработку списка
               (HC очищает BLF, когда забирает head-ED). */
            ohci->cmdstatus = (1u << 2);
            __asm volatile("dsb" ::: "memory");
        }
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
            uart_print(" ctl=0x");
            uart_print_hex(ohci->ctrl);
            uart_print(" isr=0x");
            uart_print_hex(ohci->intrstatus);
            uart_print(" edcfg=0x");
            uart_print_hex(ed->cfg);
            uart_print(" bh=0x");
            uart_print_hex(ohci->bulkhead);
            uart_print(" bcur=0x");
            uart_print_hex(ohci->bulkcur);
            uart_print(" td=0x");
            uart_print_hex(td->cfg);
            /* hcca — какой адрес HCCA реально видит HC; fr — HccaFramenumber
               (HC пишет его каждую мс): если он 0, HC работает не с нашей HCCA. */
            uart_print(" hcca=0x");
            uart_print_hex(ohci->hcca);
            uart_print(" fr=");
            uart_print_int((int)g_hcca[idx].framenumber);
            uart_print(" intr0=0x");
            uart_print_hex(g_hcca[idx].intr[0]);
            uart_print("\r\n");
            return -1;
        }
    }
    uint32_t cc = (td->cfg >> TD_CC_SHIFT) & 0xFu;
    if (dir_in) {
        cache_invalidate((uint32_t)(uintptr_t)td, sizeof(td[0]));
        cache_invalidate((uint32_t)(uintptr_t)in, len);
        /* НЕДОБОР DATA-СТАДИИ — первопричина десинка BOT и CSW fail
           (cc=0x08, h=1) при чтении каталога: INQUIRY/MBR (одиночные
           передачи) проходят, серия bulk->CSW падает. С R=1 HC может
           завершить TD с CC=0, приняв меньше len, — остаток остаётся в
           трубе и следующий CSW читает мусор.
           Читаем cbp/be ОБЯЗАТЕЛЬНО с инвалидацией TD (иначе через кэш
           видим 0 и получаем «accepted = адрес», как было в прошлой
           попытке). cbp==0 — буфер исчерпан полностью, считаем успехом. */
        uint32_t cbp = td->cbp;
        uint32_t accepted = len;
        if (cbp != 0 && td->be >= cbp) accepted = td->be - cbp + 1u;
        if (cc == TD_CC_NOERR && accepted != len) {
            uart_print("ohci-bulk: short IN got=");
            uart_print_int((int)accepted);
            uart_print(" want=");
            uart_print_int((int)len);
            uart_print("\r\n");
            cc = 0x08u;   /* DataUnderrun: ошибка + BOT recovery на слое выше */
        }
    }
    if (cc == TD_CC_NOERR) {
        /* Следующий toggle: число пакетов в передаче (BOT: точные длины).
           Счётчик раздельный по направлениям, как у устройства в паре
           EP (номер+направление — независимые эндпоинты). */
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
        /* Сколько реально принято/передано: CBP указывает за последний
           обработанный байт, остаток = BE - CBP. Без этого не отличить
           «пришло меньше, чем просили» (DataUnderrun) от «пакет не дошёл». */
        uart_print(" cbp=0x");
        uart_print_hex(td->cbp);
        uart_print(" be=0x");
        uart_print_hex(td->be);
        uart_print("\r\n");
        /* СБРОС HALTED — ТОЛЬКО ПОСЛЕ ТОГО, КАК HC ОТПУСТИЛ ED.
           Порядок критичен: сначала гасим BLE и ждём bulkcur==0 (HC закончил
           работу с ED), и лишь потом пишем HeadP (это снимает бит H) и чистим
           bulkhead. Обратный порядок (сначала head, потом BLE) — гонка с
           работающим HC: он читает переписываемый ED, залипает, и следом
           валится control (лог: ctrl TIMEOUT ... cur=0x40102200, cmdst=0x02). */
        ohci->ctrl &= ~(1u << 5);                  /* BLE off */
        __asm volatile("dsb" ::: "memory");
        for (uint32_t w = 0; w < 2000u && ohci->bulkcur; w++) udelay(10);
        ed->head = (uint32_t)(uintptr_t)td;        /* снять Halted */
        cache_clean((uint32_t)(uintptr_t)ed, sizeof(*ed));
g_bulk_toggle[idx][di] = 0;
        ohci->bulkhead = 0;
        ohci->bulkcur  = 0;
        __asm volatile("dsb" ::: "memory");
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