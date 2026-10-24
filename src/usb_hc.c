#include "usb.h"
#include "usb_ohci.h"
#include "usb_kbd.h"
#include "usb_msd.h"
#include "fat.h"
#include "h3_ccu.h"
#include "uart.h"
#include "sys.h"

/* USB host-контроллер H3: полный подъём с нуля (FEL+SPL — наследовать
 * нечего). Пути:
 *   - PHY1 (0x01C1B000/400): HS-флешка — EHCI (control/bulk через
 *     async-список qH/qTD);
 *   - PHY2 (0x01C1C000/400): клавиатура (LS/FS) — OHCI.
 * Порядок: OHCI обоих портов (такт/PHY/root port), затем EHCI PHY1.
 */

/* Текущие базы выбранной шины (устанавливаются usb_select_bus). */
static uint32_t usb_ehci_base = USB_EHCI_BASE1;
static uint32_t usb_ohci_base = USB_OHCI_BASE1;

static uint32_t usb_ehci_ops = 0;   /* сдвиг на operational-регистры EHCI */
static uint32_t usb_ehci_nports = 1;
static uint32_t g_ehci_speed = 2;   /* 0=FS, 1=LS, 2=HS (PORTSC[27:26]) */
static int g_msd_via_ohci = 0;      /* 1 = активный MSD на OHCI (FS/LS) */
static uint32_t g_msd_ohci_base = USB_OHCI_BASE1;  /* порт, где найден MSD */

void usb_select_bus(int bus)
{
    usb_ehci_base = (bus == 0) ? USB_EHCI_BASE1 : USB_EHCI_BASE2;
    usb_ohci_base = (bus == 0) ? USB_OHCI_BASE1 : USB_OHCI_BASE2;
    /* Пересчитать operational-базу EHCI: usb_hc_init опрашивает ПОСЛЕ
       usb_ehci_init_bus, а последний вызов мог быть для другой шины. */
    uint8_t caplen = *(volatile uint8_t *)(usb_ehci_base + 0x00);
    usb_ehci_ops = usb_ehci_base + (uint32_t)caplen;
}

/* ---- EHCI: структуры и инициализация ----
 * HS-флешка видна в EHCI (root порт 0). Обмен (control/bulk) — только
 * через EHCI async-список qH/qTD. */

static struct ehci_qh  g_ehci_qh_buf  USB_UNCACHED;
static struct ehci_qtd g_ehci_qtd_buf[4] USB_UNCACHED;
static uint8_t g_ehci_buf[512] USB_UNCACHED;   /* bounce для OUT/control */
static uint8_t g_setup_req[8] USB_UNCACHED;    /* SETUP (qTD) */

/* Accessor'ы операционных регистров EHCI (ops = base + caplen). */
static inline volatile uint32_t *ehci_reg(uint32_t off)
{
    return (volatile uint32_t *)(uintptr_t)(usb_ehci_ops + off);
}
static inline uint32_t ehci_rd(uint32_t off) { return *ehci_reg(off); }
static inline void     ehci_wr(uint32_t off, uint32_t v) { *ehci_reg(off) = v; }

#define EHCI_CMDR()   ehci_rd(0x00)
#define EHCI_CMDW(v)  ehci_wr(0x00, (v))
#define EHCI_STSR()   ehci_rd(0x04)
#define EHCI_STSW(v)  ehci_wr(0x04, (v))   /* USBSTS: W1C */
#define EHCI_ASYNCLW(v) ehci_wr(0x18, (v))
#define EHCI_ASYNCLR() ehci_rd(0x18)
#define EHCI_PORTSCR()  ehci_rd(0x44)
#define EHCI_PORTSCW(v) ehci_wr(0x44, (v))
#define EHCI_CFGW(v)    ehci_wr(0x40, (v))   /* CONFIGFLAG: route ports to HC */

/* Запустить async-цепочку qTD (определение ниже). */
static int ehci_run_chain(struct ehci_qh *qh, struct ehci_qtd *t, int n);

/* Сброс EHCI: USBCMD.HCReset=1, ждать сброса. */
static int usb_ehci_hc_reset(void)
{
    EHCI_CMDW(EHCI_CMDR() | (1u << 1));   /* HCReset */
    for (uint32_t t = 0; t < 0xFFFFFu; t++)
        if (!(EHCI_CMDR() & (1u << 1))) return 0;
    return -1;
}

/* Поднять EHCI на шине: cap->ops, сброс, nPorts, порт reset, async-список.
   HS-флешка обслуживается EHCI (OHCI HS не ведёт): активируем PHY1 (bus 0).
   PHY2 остаётся на OHCI (клавиатура Low-Speed). */
static int usb_ehci_init_bus(int bus)
{
    usb_select_bus(bus);
    uint8_t caplen = *(volatile uint8_t *)(usb_ehci_base + 0x00);
    uint32_t hcsparams = *(volatile uint32_t *)(usb_ehci_base + 0x04);
    usb_ehci_ops   = usb_ehci_base + (uint32_t)caplen;
    usb_ehci_nports = (hcsparams & 0x0F) ? (hcsparams & 0x0F) : 1;
    USB_LOG("ehci: caplen=");
    USB_LOG_INT(caplen);
    USB_LOG(" hcsparams=0x");
    USB_LOG_HEX(hcsparams);
    USB_LOG("\r\n");

    if (usb_ehci_hc_reset() != 0) {
        uart_print("ehci: reset fail\r\n");
        return -1;
    }
    /* Конфигурация: запустить HC (RS=1), без периодического списка. */
    EHCI_CMDW(0);             /* RS=0, clean */
    __asm volatile("dsb" ::: "memory");

    /* SIDDQ PHY1 (pmu1+0x10 bit1) — снять, иначе PHY в powerdown и порт
       рапортует не то. Passby (0x701) уже выставлен в usb_port_hw_init
       (usb_ohci.c), здесь только SIDDQ. */
    {
        volatile uint32_t *phy1_ctl = (volatile uint32_t *)(USB_PMU1_BASE + 0x10);
        *phy1_ctl &= ~USB_PHY_CTL_SIDDQ;   /* SIDDQ = 0 */
        __asm volatile("dsb" ::: "memory");
        udelay(1000);
        USB_LOG("ehci: phy1_ctl=0x");
        USB_LOG_HEX(*phy1_ctl);
        USB_LOG("\r\n");
    }

    EHCI_CMDW(1u << 0);       /* RS=1 */
    __asm volatile("dsb" ::: "memory");
    EHCI_CFGW(1);             /* CONFIGFLAG: все порты маршрутизируются на HC */
    __asm volatile("dsb" ::: "memory");
    udelay(10000);

    /* Питание порта. Сам reset делает usb_ehci_port_reset() после проверки
       CCS: в EHCI бит PR (bit8) НЕ снимается аппаратно — его очищает
       программное обеспечение (в отличие от OHCI). */
    udelay(10000);
    USB_LOG("ehci: cmd=0x");
    USB_LOG_HEX(EHCI_CMDR());
    USB_LOG(" sts=0x");
    USB_LOG_HEX(EHCI_STSR());
    USB_LOG(" portsc=0x");
    USB_LOG_HEX(EHCI_PORTSCR());
    USB_LOG("\r\n");

    /* Сброс USBSTS (W1C). Если HC в Host System Error (HSE=bit2),
       port reset не выполняется. */
    EHCI_STSW(EHCI_STSR());
    __asm volatile("dsb" ::: "memory");
    udelay(1000);

    EHCI_PORTSCW(1u << 12);                              /* PP = 1 */
    __asm volatile("dsb" ::: "memory");
    udelay(20000);
    return 0;
}

/* Построить qTD для одной стадии (EHCI spec: next/alt/token/buf[5]).
 * Буфер делится на 4К-страницы (qt_buf[0..4]); HC сам перешагивает
 * границы страниц, но указатели младше/равные длине должны быть заданы.
 * Длина qTD <= 20 КБ (5 страниц); большие передачи — цепочкой qTD. */
static void ehci_qtd_fill(struct ehci_qtd *q, const uint8_t *buf, uint32_t len,
                          uint32_t pid, uint32_t next)
{
    q->qt_next = next;
    q->qt_alt  = 1;                                  /* T=1: alt terminate */
    q->qt_token = (len << 16) | (3u << 12) | (pid << 10) | EHCI_QTD_ACTIVE;
    for (int i = 0; i < 5; i++) q->qt_buf[i] = 0;
    if (buf && len) {
        uintptr_t a = (uintptr_t)buf;
        q->qt_buf[0] = (uint32_t)a;
        uintptr_t end = a + len - 1;
        uintptr_t page = (a + 0xFFFu) & ~0xFFFu;
        for (int i = 1; i < 5 && page <= end; i++, page += 0x1000u)
            q->qt_buf[i] = (uint32_t)page;
    }
}

/* Запустить async-список и дождаться завершения цепочки qTD[0..n-1]
 * (Active снят). Возвращает 0 при успехе; при любой ошибке (в т.ч. STALL/
 * Halted) — -1. */
static int ehci_wait_done(struct ehci_qtd *t, int n)
{
    EHCI_ASYNCLW((uint32_t)(uintptr_t)&g_ehci_qh_buf);
    __asm volatile("dsb" ::: "memory");
    EHCI_CMDW(EHCI_CMDR() | (1u << 5) | (1u << 0));   /* ASE=1, RS=1 */
    __asm volatile("dsb" ::: "memory");

    for (uint32_t spin = 0; spin < 0xFFFFFu; spin++) {
        int done = 1;
        for (int i = 0; i < n; i++)
            if (t[i].qt_token & EHCI_QTD_ACTIVE) { done = 0; break; }
        if (done) break;
    }
    EHCI_CMDW(EHCI_CMDR() & ~(1u << 5));   /* ASE=0 */
    __asm volatile("dsb" ::: "memory");

    for (int i = 0; i < n; i++)
        if (t[i].qt_token & EHCI_QTD_ERR_MASK) return -1;
    return 0;
}

/* Control-передача через EHCI (async). setup[8]; IN или OUT стадия.
 * qH (g_ehci_qh_buf) + до 3 qTD: SETUP -> IN/OUT -> STATUS.
 * mps — EP0 max-packet (8..64) из device-дескриптора. */
static int usb_ehci_ctrl_mps(const uint8_t *setup, const uint8_t *out_data,
                             uint32_t out_len, uint8_t *in_buf, uint32_t in_len,
                             uint16_t mps)
{
    if (!setup) return -1;
    struct ehci_qh *qh = &g_ehci_qh_buf;
    struct ehci_qtd *t = g_ehci_qtd_buf;

    /* qH: EP0 control, HS, MaxPacket=mps (spec-раскладка). */
    qh->qh_link  = 1;                                   /* T=1: конец списка */
    qh->qh_endpt = (uint32_t)(g_usb_addr & 0x7Fu)       /* Device Address */
                 | EHCI_QH_EPN(0)                        /* EP0 */
                 | ((g_ehci_speed == 2) ? EHCI_QH_EPS_HS : 0)
                 | EHCI_QH_C                            /* C: control EP */
                 | EHCI_QH_MAXP(mps);
    qh->qh_cap   = 0;
    qh->qh_alt   = 1;                                   /* alt terminate */
    qh->qh_token = 0;
    for (int i = 0; i < 5; i++) qh->qh_buf[i] = 0;

    /* qTD0: SETUP (pid=2) */
    for (int i = 0; i < 8; i++) g_setup_req[i] = setup[i];
    ehci_qtd_fill(&t[0], g_setup_req, 8, EHCI_QTD_PID_SETUP, 0);
    t[0].qt_next = (uint32_t)(uintptr_t)&t[1];

    /* qTD1: IN/OUT или сразу STATUS. */
    if (in_len) {
        ehci_qtd_fill(&t[1], in_buf, in_len, EHCI_QTD_PID_IN, 0);
        t[1].qt_next = (uint32_t)(uintptr_t)&t[2];
        ehci_qtd_fill(&t[2], 0, 0, EHCI_QTD_PID_OUT, 1);   /* STATUS: OUT */
        return ehci_run_chain(qh, t, 3);
    }
    if (out_data) {
        for (uint32_t i = 0; i < out_len && i < sizeof(g_ehci_buf); i++)
            g_ehci_buf[i] = out_data[i];
        if (out_len > sizeof(g_ehci_buf)) return -1;        /* не лезем дальше */
        ehci_qtd_fill(&t[1], g_ehci_buf, out_len, EHCI_QTD_PID_OUT, 0);
        t[1].qt_next = (uint32_t)(uintptr_t)&t[2];
        ehci_qtd_fill(&t[2], 0, 0, EHCI_QTD_PID_IN, 1);     /* STATUS: IN */
        return ehci_run_chain(qh, t, 3);
    }
    /* Без данных: один STATUS (IN). */
    ehci_qtd_fill(&t[1], 0, 0, EHCI_QTD_PID_IN, 1);
    return ehci_run_chain(qh, t, 2);
}

/* Запустить async-цепочку qTD (очищенную qh), дождаться, вернуть код. */
static int ehci_run_chain(struct ehci_qh *qh, struct ehci_qtd *t, int n)
{
    qh->qh_cur   = (uint32_t)(uintptr_t)&t[0];
    qh->qh_next  = 0;
    qh->qh_token = 0;
    return ehci_wait_done(t, n);
}

int usb_ehci_ctrl(const uint8_t *setup, const uint8_t *out_data, uint32_t out_len,
                  uint8_t *in_buf, uint32_t in_len)
{
    return usb_ehci_ctrl_mps(setup, out_data, out_len, in_buf, in_len, 64);
}

/* ---- Bulk IN/OUT через EHCI (для MSD) ----
 * qH с адресом bulk-EP + qTD: данные (до 20КБ, иначе цепочка) -> 0 при
 * успехе. */
static int usb_ehci_bulk(int dir_in, uint8_t ep, uint16_t mps,
                         const uint8_t *out_data, uint8_t *in_data,
                         uint32_t len)
{
    if (len > sizeof(g_ehci_buf) && !dir_in) return -1;
    struct ehci_qh *qh = &g_ehci_qh_buf;
    struct ehci_qtd *t = g_ehci_qtd_buf;

    qh->qh_link  = 1;
    qh->qh_endpt = (uint32_t)(g_usb_addr & 0x7Fu)
                 | EHCI_QH_EPN(ep)
                 | ((g_ehci_speed == 2) ? EHCI_QH_EPS_HS : 0)
                 | EHCI_QH_MAXP(mps);
    qh->qh_cap   = 0;
    qh->qh_alt   = 1;
    qh->qh_token = 0;
    for (int i = 0; i < 5; i++) qh->qh_buf[i] = 0;

    if (dir_in) {
        if (len > 20u * 1024u) return -1;   /* один qTD максимум 20КБ */
        ehci_qtd_fill(&t[0], in_data, len, EHCI_QTD_PID_IN, 1);
    } else {
        for (uint32_t i = 0; i < len; i++) g_ehci_buf[i] = out_data[i];
        ehci_qtd_fill(&t[0], g_ehci_buf, len, EHCI_QTD_PID_OUT, 1);
    }
    return ehci_run_chain(qh, t, 1);
}

int usb_bulk_out(uint8_t ep, uint16_t mps, const uint8_t *data, uint32_t len)
{
    if (g_msd_via_ohci)
        return usb_ohci_bulk_out(g_msd_ohci_base, g_usb_addr, ep,
                                 data, len, mps);
    return usb_ehci_bulk(0, ep, mps, data, 0, len);
}

int usb_bulk_in(uint8_t ep, uint16_t mps, uint8_t *data, uint32_t len)
{
    if (g_msd_via_ohci)
        return usb_ohci_bulk_in(g_msd_ohci_base, g_usb_addr, ep,
                                data, len, mps);
    return usb_ehci_bulk(1, ep, mps, 0, data, len);
}

/* ---- EHCI-порт (PHY1, флешка HS) ---- */
static int usb_ehci_port_connected(int bus)
{
    usb_select_bus(bus);
    return (EHCI_PORTSCR() & (1u << 0)) ? 1 : 0;   /* CCS */
}

static int usb_ehci_port_reset(int bus)
{
    usb_select_bus(bus);
    uint32_t ps = EHCI_PORTSCR();
    if (!(ps & (1u << 0))) return -1;   /* CCS */

    /* Питание, затем старт сброса (PR=1). */
    EHCI_PORTSCW(1u << 12);                              /* PP = 1 */
    __asm volatile("dsb" ::: "memory");
    udelay(20000);
    EHCI_PORTSCW((1u << 12) | (1u << 8));                /* PP | PR */
    __asm volatile("dsb" ::: "memory");
    udelay(50000);                                       /* >= 10 мс по спеке */

    /* EHCI: PR не снимается сам — очищаем вручную (сохраняя PE/PP). */
    ps = EHCI_PORTSCR();
    EHCI_PORTSCW((ps & ~(1u << 8)) | (1u << 12));
    __asm volatile("dsb" ::: "memory");
    udelay(20000);   /* дать HC завершить reset и обновить speed/PED */

    /* Ждём завершения: PE=1 (HS) либо PO=1 (порт ушёл компаньону FS/LS). */
    for (uint32_t t = 0; t < 100; t++) {
        ps = EHCI_PORTSCR();
        if ((ps & (1u << 2)) || (ps & (1u << 13))) break;
        udelay(1000);
    }
    USB_LOG("ehci: port reset portsc=0x");
    USB_LOG_HEX(ps);
    USB_LOG(" pe=");
    USB_LOG_INT((ps >> 2) & 1);
    USB_LOG(" po=");
    USB_LOG_INT((ps >> 13) & 1);
    USB_LOG(" speed=");
    USB_LOG_INT((ps >> 26) & 3);   /* EHCI: 0=FS, 1=LS, 2=HS */
    USB_LOG("\r\n");
    g_ehci_speed = (ps >> 26) & 3;
    return (ps & (1u << 2)) ? 0 : -1;   /* PE */
}

/* Enum + MSD + MBR через EHCI (HS-устройство на PHY1). */
static void usb_ehci_enumerate_msd(void)
{
    usb_select_bus(0);
    static uint8_t buf[64] USB_UNCACHED;
    static uint8_t cfg9[9] USB_UNCACHED;
    static uint8_t cfg_full[256] USB_UNCACHED;
    static uint8_t mbr[512] USB_UNCACHED;
    static struct usb_msd_dev mdev;
    uint8_t setup[8];

    /* 1. GET_DESCRIPTOR(DEVICE) 8 байт, адрес 0 */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x01;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x08; setup[7]=0x00;
    g_usb_addr = 0;
    int mps = (g_ehci_speed == 2) ? 64 : 8;
    if (usb_ehci_ctrl_mps(setup, 0, 0, buf, 8, (uint16_t)mps) != 0) {
        uart_print("usb-ehci: dev desc fail\r\n");
        return;
    }
    mps = buf[7];
    if (mps < 8) mps = 8;
    if (mps > 64) mps = 64;
    uart_print("usb-ehci: mps=");
    uart_print_int(mps);
    uart_print("\r\n");

    /* 2. SET_ADDRESS(1) */
    setup[0]=0x00; setup[1]=0x05; setup[2]=0x01; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x00; setup[7]=0x00;
    if (usb_ehci_ctrl_mps(setup, 0, 0, 0, 0, (uint16_t)mps) != 0) {
        uart_print("usb-ehci: set addr fail\r\n");
        return;
    }
    g_usb_addr = 1;
    udelay(5000);

    /* 3. Полный device-дескриптор */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x01;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x12; setup[7]=0x00;
    if (usb_ehci_ctrl_mps(setup, 0, 0, buf, 18, (uint16_t)mps) != 0) {
        uart_print("usb-ehci: full desc fail\r\n");
        return;
    }
    uart_print("usb-ehci: vid=0x");
    uart_print_hex((uint32_t)(buf[8] | (buf[9] << 8)));
    uart_print(" pid=0x");
    uart_print_hex((uint32_t)(buf[10] | (buf[11] << 8)));
    uart_print("\r\n");

    /* 4. Config 9 -> полный конфиг */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x02;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x09; setup[7]=0x00;
    if (usb_ehci_ctrl_mps(setup, 0, 0, cfg9, 9, (uint16_t)mps) != 0) {
        uart_print("usb-ehci: cfg desc fail\r\n");
        return;
    }
    uint32_t wlen = (uint32_t)(cfg9[2] | (cfg9[3] << 8));
    if (wlen > sizeof(cfg_full)) wlen = sizeof(cfg_full);
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x02;
    setup[4]=0x00; setup[5]=0x00;
    setup[6]=(uint8_t)wlen; setup[7]=(uint8_t)(wlen >> 8);
    if (usb_ehci_ctrl_mps(setup, 0, 0, cfg_full, wlen, (uint16_t)mps) != 0) {
        uart_print("usb-ehci: cfg full fail\r\n");
        return;
    }

    /* 5. SET_CONFIGURATION(1) */
    setup[0]=0x00; setup[1]=0x09; setup[2]=0x01; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x00; setup[7]=0x00;
    if (usb_ehci_ctrl_mps(setup, 0, 0, 0, 0, (uint16_t)mps) != 0) {
        uart_print("usb-ehci: SET_CONFIGURATION fail\r\n");
        return;
    }
    udelay(5000);

    /* 6. MSD-интерфейс + параметры носителя */
    if (usb_msd_init(&mdev, cfg_full, wlen) != 0) {
        uart_print("usb-ehci: no MSD interface\r\n");
        return;
    }
    g_msd_via_ohci = 0;
    uint32_t last = 0, blk = 0;
    if (usb_msd_wait_ready(&mdev, 3) != 0)
        uart_print("usb-ehci: TUR fail\r\n");
    if (usb_msd_read_capacity(&mdev, &last, &blk) == 0) {
        uart_print("usb-ehci: block=");
        uart_print_int((int)blk);
        uart_print(" last_lba=");
        uart_print_int((int)last);
        uart_print("\r\n");
    } else {
        uart_print("usb-ehci: READ CAPACITY fail\r\n");
    }
    if (usb_msd_activate(&mdev) != 0) {
        uart_print("usb-ehci: msd activate fail\r\n");
        return;
    }
    if (usb_msd_block_read(0, mbr) != 0) {
        uart_print("usb-ehci: MBR read fail\r\n");
        return;
    }
    uart_print("usb-ehci: MBR sig=0x");
    uart_print_hex((uint32_t)(mbr[510] | (mbr[511] << 8)));
    uart_print("\r\n");
}

/* ---- Enumeration на OHCI-порту (общая часть для MSD и HID) ----
 * GET_DESC(8) -> SET_ADDRESS(1) -> GET_DESC(full) -> GET_CONFIG(9/full) ->
 * SET_CONFIGURATION(1). После успеха устройство стоит на адресе 1 с
 * конфигурацией 1, а cfg содержит полный config-дескриптор: MSD берёт из
 * него bulk-EP, HID — interrupt IN клавиатуры. Порт задаётся base. */
static int usb_ohci_enum_common(uint32_t base, uint8_t *cfg, uint32_t cfg_size,
                                uint32_t *cfg_len)
{
    static uint8_t buf[64] USB_UNCACHED;
    static uint8_t cfg9[9] USB_UNCACHED;
    uint8_t setup[8];

    /* 1. GET_DESCRIPTOR(DEVICE) 8 байт, адрес 0 */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x01;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x08; setup[7]=0x00;
    if (usb_ohci_ctrl_addr0(base, setup, 8, buf, 8, 1) < 0) {
        uart_print("usb-ohci: dev desc fail\r\n");
        return -1;
    }
    int mps = buf[7];
    if (mps < 8) mps = 8;
    if (mps > 64) mps = 64;
    usb_ohci_set_mps(base, (uint16_t)mps);
    uart_print("usb-ohci: mps=");
    uart_print_int(mps);
    uart_print("\r\n");

    /* 2. SET_ADDRESS(1) */
    setup[0]=0x00; setup[1]=0x05; setup[2]=0x01; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x00; setup[7]=0x00;
    if (usb_ohci_ctrl_addr0(base, setup, 8, 0, 0, 0) < 0) {
        uart_print("usb-ohci: set addr fail\r\n");
        return -1;
    }
    g_usb_addr = 1;
    udelay(5000);

    /* 3. Полный device-дескриптор (VID/PID) */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x01;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x12; setup[7]=0x00;
    if (usb_ohci_ctrl_transfer(base, 1, setup, 8, buf, 18, 1, 1000) < 0) {
        uart_print("usb-ohci: full desc fail\r\n");
        return -1;
    }
    uart_print("usb-ohci: vid=0x");
    uart_print_hex((uint32_t)(buf[8] | (buf[9] << 8)));
    uart_print(" pid=0x");
    uart_print_hex((uint32_t)(buf[10] | (buf[11] << 8)));
    uart_print("\r\n");

    /* 4. Config 9 -> полный конфиг */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x02;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x09; setup[7]=0x00;
    if (usb_ohci_ctrl_transfer(base, 1, setup, 8, cfg9, 9, 1, 1000) < 0) {
        uart_print("usb-ohci: cfg desc fail\r\n");
        return -1;
    }
    uint32_t wlen = (uint32_t)(cfg9[2] | (cfg9[3] << 8));
    if (wlen > cfg_size) wlen = cfg_size;
    if (wlen < 9) {
        uart_print("usb-ohci: cfg len bad\r\n");
        return -1;
    }
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x02;
    setup[4]=0x00; setup[5]=0x00;
    setup[6]=(uint8_t)wlen; setup[7]=(uint8_t)(wlen >> 8);
    if (usb_ohci_ctrl_transfer(base, 1, setup, 8, cfg, wlen, 1, 1000) < 0) {
        uart_print("usb-ohci: cfg full fail\r\n");
        return -1;
    }

    /* 5. SET_CONFIGURATION(1) */
    setup[0]=0x00; setup[1]=0x09; setup[2]=0x01; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x00; setup[7]=0x00;
    if (usb_ohci_ctrl_transfer(base, 1, setup, 8, 0, 0, 0, 1000) < 0) {
        uart_print("usb-ohci: SET_CONFIGURATION fail\r\n");
        return -1;
    }
    udelay(100000);   /* устройству нужно время после конфигурации */

    *cfg_len = wlen;
    return 0;
}

/* MSD на найденном порту: BOT recovery + проверка команд носителя. */
static int usb_ohci_msd_start(uint32_t base, struct usb_msd_dev *dev)
{
    static uint8_t inq[36] USB_UNCACHED;
    static uint8_t mbr[512] USB_UNCACHED;
    uint8_t s[8];

    g_msd_via_ohci = 1;
    g_msd_ohci_base = base;
    usb_ohci_bulk_toggle_reset(base);

    /* Активировать накопитель ДО первых команд: usb_bulk_* работают только
       с активным dev (иначе молча возвращают -1). */
    if (usb_msd_activate(dev) != 0) {
        uart_print("usb-ohci: msd activate fail\r\n");
        return -1;
    }

    /* BOT recovery: GET_MAX_LUN (может не поддерживаться), Bulk-Only Reset
       (0xFF) — сбрасывает BOT-состояние и toggle, CLEAR_FEATURE(HALT) на
       оба bulk-EP (wIndex — номер EP с битом направления). Устройство может
       остаться в BOT-ошибке после прошлого Stall: VBUS при перезагрузке SoC
       не пропадает. */
    s[0]=0xA1; s[1]=0xFE; s[2]=0; s[3]=0; s[4]=0; s[5]=0; s[6]=1; s[7]=0;
    uint8_t lun = 0;
    usb_ohci_ctrl_transfer(base, 1, s, 8, &lun, 1, 1, 1000);
    s[0]=0x21; s[1]=0xFF; s[2]=0; s[3]=0; s[4]=0; s[5]=0; s[6]=0; s[7]=0;
    usb_ohci_ctrl_transfer(base, 1, s, 8, 0, 0, 0, 1000);
    for (int i = 0; i < 2; i++) {
        uint8_t idx = i ? (uint8_t)(dev->ep_in | 0x80) : dev->ep_out;
        s[0]=0x02; s[1]=0x01; s[2]=0; s[3]=0; s[4]=idx; s[5]=0; s[6]=0; s[7]=0;
        usb_ohci_ctrl_transfer(base, 1, s, 8, 0, 0, 0, 1000);
    }
    usb_ohci_bulk_toggle_reset(base);   /* ClearFeature сбрасывает toggle */
    uart_print("usb-ohci: BOT recovery done\r\n");

    /* INQUIRY — проверка, что bulk-канал и команды живы. */
    if (usb_msd_inquiry(dev, inq) == 0) {
        uart_print("usb-ohci: INQUIRY ok: ");
        for (int i = 8; i < 32 && inq[i]; i++) {
            char c = (char)inq[i];
            if (c < 0x20 || c > 0x7E) c = '.';
            char t[2] = { c, 0 };
            uart_print(t);
        }
        uart_print("\r\n");
    } else {
        uart_print("usb-ohci: INQUIRY fail\r\n");
    }

    /* Готовность (до 3 попыток: носитель «просыпается» после конфигурации)
       и размер блока (4Kn-флешки: 4096). */
    uint32_t last = 0, blk = 0;
    if (usb_msd_wait_ready(dev, 3) != 0)
        uart_print("usb-ohci: TUR fail\r\n");
    if (usb_msd_read_capacity(dev, &last, &blk) == 0) {
        uart_print("usb-ohci: block=");
        uart_print_int((int)blk);
        uart_print(" last_lba=");
        uart_print_int((int)last);
        uart_print("\r\n");
    } else {
        uart_print("usb-ohci: READ CAPACITY fail\r\n");
    }

    if (usb_msd_block_read(0, mbr) != 0) {
        uart_print("usb-ohci: MBR read fail\r\n");
        return -1;
    }
    uart_print("usb-ohci: MBR sig=0x");
    uart_print_hex((uint32_t)(mbr[510] | (mbr[511] << 8)));
    uart_print("\r\n");
    return 0;
}

/* Один OHCI-порт: сброс, enum, определение класса по config-дескриптору и
 * запуск нужного драйвера. Порт не важен: MSD и HID опознаются по
 * interface-дескрипторам (флешка может стоять в любом из двух разъёмов).
 * Возвращает 1, если устройство принято драйвером. */
static int usb_ohci_claim_port(int port)
{
    uint32_t base = (port == 1) ? USB_OHCI_BASE1 : USB_OHCI_BASE2;
    static uint8_t cfg[256] USB_UNCACHED;
    static struct usb_msd_dev mdev;
    uint32_t wlen = 0;

    if (!usb_ohci_port_connected(base)) {
        uart_print("usb: port");
        uart_print_int(port);
        uart_print(": no device (CCS=0)\r\n");
        return 0;
    }
    int rr = (port == 1) ? usb_ohci_port_reset_all1()
                         : usb_ohci_port_reset_all2();
    if (rr != 0) {
        uart_print("usb: port");
        uart_print_int(port);
        uart_print(": reset fail\r\n");
        return 0;
    }
    /* Port reset печатает своё состояние сам (usb_ohci.c), поэтому строку
       порта печатаем целиком после него — иначе лог склеивается. */
    uart_print("usb: port");
    uart_print_int(port);
    uart_print(": DEVICE on OHCI, enum...\r\n");
    if (usb_ohci_enum_common(base, cfg, sizeof(cfg), &wlen) != 0) return 0;

    /* MSD-интерфейс (class 8/6/0x50) -> драйвер накопителя. */
    if (usb_msd_init(&mdev, cfg, wlen) == 0) {
        if (usb_ohci_msd_start(base, &mdev) == 0) return 1;
        uart_print("usb: msd init fail\r\n");
        return 0;
    }
    /* Иначе — HID boot-клавиатура (class 3/1/1). */
    if (usb_kbd_init_on(base, cfg, wlen) == 0) return 1;

    uart_print("usb: unknown device class\r\n");
    return 0;
}

int usb_hc_init(void)
{
    uart_print("usb: hc init (ohci 2 ports)\r\n");

    /* Сначала OHCI-init обоих портов: он включает такты и PHY
       (usb_port_hw_init), без них EHCI читается как нули. Порт reset
       через OHCI для PHY1 не делаем — HS-флешку сбрасывает EHCI. */
    usb_ohci_init_port1();
    usb_ohci_init_port2();

    /* PHY1: флешка High-Speed — EHCI (OHCI HS не ведёт, даёт STALL). */
    usb_ehci_init_bus(0);

    int found = 0;
    int port1_claimed = 0;

    /* Порт 1: HS -> EHCI; FS/LS -> компаньон OHCI (EHCI их не ведёт). */
    if (usb_ehci_port_connected(0)) {
        uart_print("usb: DEVICE on EHCI(0), port reset...\r\n");
        int rr = usb_ehci_port_reset(0);
        if (rr == 0 && g_ehci_speed == 2) {
            found++;
            port1_claimed = 1;
            uart_print("usb: port1 ready (HS), ehci enumerate...\r\n");
            usb_ehci_enumerate_msd();
        } else {
            /* Отдать порт компаньону: PO=1, затем обычный OHCI-путь. */
            uart_print("usb: port1 FS/LS -> OHCI\r\n");
            EHCI_PORTSCW(EHCI_PORTSCR() | (1u << 13));   /* PO = 1 */
            __asm volatile("dsb" ::: "memory");
            udelay(10000);
        }
    } else {
        uart_print("usb: port1 no device on EHCI (CCS=0)\r\n");
    }

    if (!port1_claimed) found += usb_ohci_claim_port(1);

    /* Порт 2: донглу нужно время появиться на шине после подачи PPS —
       ждём CCS до 5 с (эталон pico-retro). */
    for (int trial = 0; trial < 500; trial++) {
        if (usb_ohci_port_connected2()) break;
        udelay(10000);
    }
    found += usb_ohci_claim_port(2);

    return found;
}