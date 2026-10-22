#include "h3_dma.h"
#include "h3.h"
#include "h3_ccu.h"
#include "h3_mmu.h"
#include "sys.h"

/* sunxi-DMA (H3): циклическая передача RAM -> I2S0_TX_FIFO.
 * Карта регистров — по даташиту H3 4.11.4; порядок запуска и раскладка LLI —
 * по Linux-драйверу sun6i-dma.c.
 *
 * Выбор канала: у H3 12 физических DMA-каналов. U-Boot (SPL + чтение SD)
 * занимает каналы 0..2. Нельзя использовать канал, который держит загрузчик,
 * поэтому сначала пробуем верхний диапазон (5..11), затем 0..4, и в момент
 * запуска проверяем (smart-start), что выбранный канал действительно принял
 * наш LLI.
 *
 * Атомарность (требование проекта): трогаем ТОЛЬКО биты своего канала в
 * глобальных регистрах IRQ_EN/IRQ_PEND/STA; никогда не пишем 0xFFFFFFFF
 * глобально — это сотрёт чужие каналы U-Boot (память, USB, MMC).
 */

/* Предпочтительный канал (из нижнего диапазона 0..4; верхний 5..11 занимает
   SPL — ищем с него, но 4 берём как надёжный после проверки smart-start). */
#define AUDIO_DMA_CH    4
#define DMA_NCHAN       12

/* Канал n = H3_DMA_BASE + 0x100 + n*0x40 (H3_DMA_CHL0_BASE = 0x01C02100). */
#define DMA_IRQ_EN_REG(n)   (*(volatile uint32_t *)(H3_DMA_BASE + ( (n)/8 ) * 0x04))
#define DMA_IRQ_STAT_REG(n) (*(volatile uint32_t *)(H3_DMA_BASE + 0x10 + ( (n)/8 ) * 0x04))
#define DMA_STA             (*(volatile uint32_t *)(H3_DMA_BASE + 0x30))

/* Биты канала в IRQ-регистре: HALF=bit0, PKG=bit1, QUEUE=bit2,
   сдвинуты на (chan%8)*4. */
#define DMA_IRQ_CHAN_OFF(n)  (((n) % 8) * 4)
#define DMA_CH(_n, reg)      (*(volatile uint32_t *)(H3_DMA_BASE + 0x100 + (_n) * 0x40 + (reg)))

/* Занят ли канал шину прямо сейчас (busy-бит в DMA_STA)? */
static inline int dma_ch_busy(int n) { return (int)((DMA_STA >> n) & 1u); }

#define DMA_IRQ_HALF   (1u << 0)
#define DMA_IRQ_PKG    (1u << 1)
#define DMA_IRQ_QUEUE  (1u << 2)

/* DRQ-порты (даташит 4.11.2.2, таблица 4-1): SDRAM=1, I2S/PCM0_TX=3. */
#define DRQ_SDRAM       1u
#define DRQ_I2S0_TX     3u

/* CFG-биты (даташит 4.11.4.11; H3: SRC_BURST bits[7:6], DST_BURST bits[23:22]). */
#define CFG_SRC_DRQ(d)     ((d) << 0)
#define CFG_SRC_MODE_IO    (1u << 5)
#define CFG_SRC_MODE_LIN   0u
#define CFG_SRC_BURST(b)   ((b) << 6)   /* 0=1, 1=4, 2=8, 3=16 */
#define CFG_SRC_WIDTH(w)   ((w) << 9)   /* 2=32-bit */
#define CFG_DST_DRQ(d)     ((d) << 16)
#define CFG_DST_MODE_IO    (1u << 21)   /* IO mode (FIFO) */
#define CFG_DST_MODE_LIN   0u
#define CFG_DST_BURST(b)   ((b) << 22)
#define CFG_DST_WIDTH(w)   ((w) << 25)

#define NORMAL_WAIT        8u
#define BURST_8            2u

struct dma_lli {
    uint32_t cfg;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
    uint32_t para;
    uint32_t p_next;
    uint32_t pad[2];   /* выравнивание до 32 байт (кэш-линия Cortex-A7) */
};

/* LLI читает сам DMAC — обязан лежать в uncached RAM (.coherent). */
static struct dma_lli g_lli[H3_DMA_AUDIO_PERIODS_MAX]
    __attribute__((section(".coherent"), aligned(64)));

static int g_ch = AUDIO_DMA_CH;
static int      g_periods;      /* периодов в кольце */
static uint32_t g_buf_phys;     /* физический адрес буфера */
static uint32_t g_buf_bytes;    /* размер кольца в байтах */
static uint32_t g_last_pos;     /* последняя достоверная позиция, байты */

/* Погасить осиротевшие DMA-каналы (наследие SPL/U-Boot).
 *
 * После `go` код загрузчика уже не исполняется, НО включённый DMA-канал —
 * автономный аппаратный агент: он продолжает возить данные в DRAM, пока его
 * не остановят. В JTAG-дампах виден канал 4 с EN=1, DESC/CUR_SRC="eGON" —
 * остаток от чтения eGON-образа. Такой «зомби» может писать в зону нашего
 * образа и бороться с нашим LLI/буфером -> «то рычание, то тишина».
 *
 * Гасим его ТОЛЬКО канальными регистрами (EN/PAU) и точечным W1C двух бит.
 * НЕ делаем CCU soft-reset DMA (BUS_SOFT_RESET0 bit6) и не трогаем GIC — это
 * рискованно и не нужно: у USB свой HC-DMA (не общий DMAC), HDMI управляется
 * регистрами DE2/TCON, а SD мёртв после `go`.
 *
 * I9 (осознанное решение): перебор идёт по ВСЕМ 12 каналам, а не по «безопасному»
 * диапазону. Зомби встречался именно на канале 4 (PROJECT-MEMORY-ARCHIVE), а у
 * загрузчика могут остаться каналы и в младших номерах — сужение диапазона
 * оставило бы часть агентов живыми и вернуло бы «то рычание, то тишину».
 * Сужать имеет смысл только когда в проекте появится второй потребитель общего
 * DMAC — тогда по битовой маске владельцев, а не по номеру канала.
 */
static void dma_reap_stray(void)
{
    for (int n = 0; n < DMA_NCHAN; n++) {
        uint32_t en = DMA_CH(n, 0x00);
        if (!en && !dma_ch_busy(n))
            continue;                       /* канал и так свободен */

        DMA_CH(n, 0x00) = 0;                /* EN = 0 -> прервать передачу */
        DMA_CH(n, 0x04) = 0;                /* PAU = 0 (снять паузу) */
        __asm volatile("dmb sy" ::: "memory");

        /* Дождаться фактической остановки (busy=0), с таймаутом. */
        for (volatile int t = 0; t < 1000000 && dma_ch_busy(n); t++)
            ;

        /* W1C: снять только биты этого канала (HALF|PKG). */
        uint32_t off = DMA_IRQ_CHAN_OFF(n);
        DMA_IRQ_STAT_REG(n) |= ((DMA_IRQ_HALF | DMA_IRQ_PKG) << off);
        __asm volatile("dmb sy" ::: "memory");
    }
}

/* Выбрать свободный физический канал для звука.
 *
 * В режиме U-Boot/SD SPL может занять канал 4 (или другой младший) и держать
 * там дескриптор "eGON", перезаписывая наш DESC_ADDR. Поэтому сначала пробуем
 * верхний диапазон (5..11), затем нижний (0..4). dma_reap_stray() уже погасил
 * зомби перед этим вызовом.
 */
static int dma_pick_channel(void)
{
    int n;
    /* 1-й проход: верхние каналы (5..11) — в стороне от SPL-диапазона. */
    for (n = 5; n < DMA_NCHAN; n++)
        if (!DMA_CH(n, 0x00) && !dma_ch_busy(n))
            return n;
    /* 2-й проход: остальные (0..4). */
    for (n = 0; n < 5; n++)
        if (!DMA_CH(n, 0x00) && !dma_ch_busy(n))
            return n;
    return -1;
}

int h3_dma_audio_init(uint32_t buf_phys, uint32_t buf_bytes, int periods)
{
    /* Кольцо из periods равных дескрипторов; период кратен 4 байтам. */
    if (periods < 1 || periods > H3_DMA_AUDIO_PERIODS_MAX) return -1;
    if (buf_bytes == 0 || (buf_bytes % (uint32_t)periods) != 0) return -1;
    if ((buf_bytes / (uint32_t)periods) % 4u) return -1;

    g_periods   = periods;
    g_buf_phys  = buf_phys;
    g_buf_bytes = buf_bytes;
    g_last_pos  = 0;

    /* Тактировать DMAC. Если DMA-блок в reset (RST0 bit6=0) — снять reset;
       это НЕ глобальный soft-reset, а только сброс состояния. В FEL SPL
       оставляет DMA в reset, поэтому его регистры (SEC/AUTO_GATE/каналы) не
       отвечают на запись и DMA молчит. В SD-режиме U-Boot уже деассертит
       RST0 bit6, тогда не трогаем. */
    H3_CCU_GATING0 |= H3_CCU_GATE_DMA;
    __asm volatile("dsb" ::: "memory");
    if (!(H3_CCU_RST0 & H3_CCU_RST_DMA)) {
        H3_CCU_RST0 |= H3_CCU_RST_DMA;   /* снять reset DMA */
        __asm volatile("dsb" ::: "memory");
        udelay(10);
    }

    /* DMA_SECURE_REG: пометить все каналы non-secure (после включения такта). */
    *(volatile uint32_t *)(H3_DMA_BASE + 0x20) = 0xFFFFFFFFu;
    __asm volatile("dsb" ::: "memory");

    /* Auto-gating (Linux sun6i: SUNXI_H3_DMA_GATE 0x28 = 0x4). */
    *(volatile uint32_t *)(H3_DMA_BASE + 0x28) = 0x4u;
    __asm volatile("dsb" ::: "memory");

    /* 1) Погасить зомби-каналы загрузчика. */
    dma_reap_stray();

    /* 2) Выбрать свободный канал. */
    int n = dma_pick_channel();
    if (n < 0) return -1;
    g_ch = n;

    /* Сбросить только биты этого канала в IRQ_EN/STAT (W1C по маске канала). */
    uint32_t off = DMA_IRQ_CHAN_OFF(n);
    uint32_t mask = (DMA_IRQ_HALF | DMA_IRQ_PKG | DMA_IRQ_QUEUE) << off;
    DMA_IRQ_EN_REG(n)   &= ~mask;
    DMA_IRQ_STAT_REG(n) |= mask;
    __asm volatile("dmb sy" ::: "memory");

    /* Остановить канал перед настройкой. */
    DMA_CH(n, 0x00) = 0;            /* EN = 0 */
    DMA_CH(n, 0x04) = 0;            /* PAU = 0 */
    __asm volatile("dmb sy" ::: "memory");

    /* Конфиг LLI: RAM(linear,32b,burst8) -> I2S0_TX FIFO(IO,32b,burst8).
       burst 8 совпадает с maxburst=8 в драйвере Linux sun4i-i2s. */
    uint32_t cfg = CFG_SRC_DRQ(DRQ_SDRAM) | CFG_SRC_MODE_LIN
                 | CFG_SRC_BURST(BURST_8) | CFG_SRC_WIDTH(2)
                 | CFG_DST_DRQ(DRQ_I2S0_TX) | CFG_DST_MODE_IO
                 | CFG_DST_BURST(BURST_8) | CFG_DST_WIDTH(2);

    uint32_t period_bytes = buf_bytes / (uint32_t)periods;
    for (int i = 0; i < periods; i++) {
        struct dma_lli *l = &g_lli[i];
        l->cfg    = cfg;
        l->src    = buf_phys + (uint32_t)i * period_bytes;
        l->dst    = H3_I2S0_TX_FIFO;
        l->len    = period_bytes;
        l->para   = NORMAL_WAIT;
        l->p_next = (uint32_t)(uintptr_t)&g_lli[(i + 1) % periods];  /* циклически */
    }
    __asm volatile("dmb sy" ::: "memory");
    __asm volatile("dsb" ::: "memory");

    /* Почистить LLI из D-cache в физическую память: DMAC читает дескрипторы по
       физическому адресу, минуя кэш CPU. Без этого он читает старые данные
       (JTAG показал CFG=0xea000016 и CUR="eGON" по адресу 0x40100000). */
    dcc_clean_range((uint32_t)(uintptr_t)&g_lli[0],
                    (uint32_t)(uintptr_t)&g_lli[periods] - (uint32_t)(uintptr_t)&g_lli[0]);

    return 0;
}

/* Позиция чтения внутри кольца (байты). Регистры канала: LLI/DESC 0x08,
   CUR_SRC 0x10 (RO) — HARDWARE-REF.md:51. Значение вне диапазона буфера
   (канал остановлен или ещё не стартовал) игнорируем и отдаём последнее
   достоверное: во время работы CUR_SRC «живой» и лежит внутри [buf, buf+len). */
uint32_t h3_dma_audio_pos_bytes(void)
{
    int n = g_ch;
    if (n < 0 || g_buf_bytes == 0) return g_last_pos;

    uint32_t cur = DMA_CH(n, 0x10);
    if (cur < g_buf_phys || cur >= g_buf_phys + g_buf_bytes)
        return g_last_pos;

    g_last_pos = cur - g_buf_phys;
    return g_last_pos;
}

void h3_dma_audio_start(void)
{
    /* Smart-start: пробуем каналы по очереди, пока один реально не примет наш
       LLI. SPL/U-Boot может держать недетерминированный канал (DMA_STA=busy,
       DESC=мусор), поэтому перед записью DESC ОБЯЗАТЕЛЬНО дожидаемся остановки
       канала (busy=0), иначе запись DESC проглатывается, а EN=1 перезапускает
       канал со СТАРЫМ (чужим) дескриптором. */

    uint32_t want = (uint32_t)(uintptr_t)&g_lli[0];
    int tried = 0;

    while (tried++ < DMA_NCHAN) {
        int n = g_ch;
        if (n < 0) return;

        /* Остановить канал и дождаться фактической остановки (busy=0). */
        DMA_CH(n, 0x00) = 0;                 /* EN = 0 */
        DMA_CH(n, 0x04) = 0;                 /* PAU = 0 */
        __asm volatile("dmb sy" ::: "memory");
        for (volatile int t = 0; t < 100000 && dma_ch_busy(n); t++)
            ;
        __asm volatile("dmb sy" ::: "memory");

        /* Сбросить только биты нашего канала (W1C по маске). */
        uint32_t off = DMA_IRQ_CHAN_OFF(n);
        uint32_t mask = (DMA_IRQ_HALF | DMA_IRQ_PKG | DMA_IRQ_QUEUE) << off;
        DMA_IRQ_STAT_REG(n) |= mask;
        __asm volatile("dmb sy" ::: "memory");

        /* Записать DESC_ADDR, затем EN=1 (Linux sun6i_dma_start_desc). */
        DMA_CH(n, 0x08) = want;
        __asm volatile("dmb sy" ::: "memory");
        DMA_CH(n, 0x00) = 1;
        __asm volatile("dmb sy" ::: "memory");

        /* Проверка: DESC принят и канал стартовал (busy=1). */
        if (DMA_CH(n, 0x08) == want && dma_ch_busy(n))
            return;   /* успех */

        /* Канал перехвачен чужим агентом — остановить и попробовать следующий. */
        DMA_CH(n, 0x00) = 0;
        DMA_CH(n, 0x04) = 0;
        DMA_IRQ_STAT_REG(n) |= mask;
        __asm volatile("dmb sy" ::: "memory");

        int next = -1;
        for (int k = 0; k < DMA_NCHAN; k++) {
            int c = (k + n + 1) % DMA_NCHAN;
            if (!DMA_CH(c, 0x00) && !dma_ch_busy(c)) { next = c; break; }
        }
        if (next < 0) return;
        g_ch = next;
    }
}

void h3_dma_audio_stop(void)
{
    int n = g_ch;
    if (n < 0) return;
    DMA_CH(n, 0x00) = 0;             /* EN = 0 */
    DMA_CH(n, 0x04) = 0;             /* PAU = 0 */
    /* Сбросить только биты нашего канала. */
    uint32_t off = DMA_IRQ_CHAN_OFF(n);
    uint32_t mask = (DMA_IRQ_HALF | DMA_IRQ_PKG | DMA_IRQ_QUEUE) << off;
    DMA_IRQ_STAT_REG(n) |= mask;
    __asm volatile("dmb sy" ::: "memory");
}