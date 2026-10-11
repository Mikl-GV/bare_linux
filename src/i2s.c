#include "i2s.h"
#include "h3.h"
#include "h3_ccu.h"
#include "h3_dma.h"
#include "ports.h"
#include "sys.h"
#include "h3_mmu.h"

/* I2S0 -> MAX98357A (Allwinner H3, sun8i).
 * Реализовано с нуля по даташиту H3 (раздел 8.6) и Linux-драйверу
 * sun8i-h3-i2s.c.
 *
 * Регистры I2S/PCM0 (база 0x01C22000) по даташиту 8.6.6:
 *   CTL 0x00, FMT0 0x04, FMT1 0x08, ISTA 0x0C, RXFIFO 0x10,
 *   FCTL 0x14, FSTA 0x18, INT 0x1C, TXFIFO 0x20, CLKD 0x24,
 *   TXCNT 0x28, RXCNT 0x2C, CHCFG 0x30, TXnCHSEL 0x34+4n,
 *   TXnCHMAP 0x44+4n.
 *
 * Формат (проверен на стенде, HARDWARE-REF): 32-битное слово на слот FIFO,
 * отсчёт в СТАРШИХ 16 битах; пара [L<<16][R<<16]. TXIM=0 (данные MSB),
 * см. 8.6.7.6.
 *
 * Порядок запуска/останова — по Linux sun4i_i2s_start/stop_playback:
 *   flush TX FIFO -> clear TXCNT -> TX_EN -> TX_DRQ.
 */

/* ---------- Регистры I2S0 ---------- */
#define I2S_CTL     (*(volatile uint32_t *)(H3_I2S0_BASE + 0x00))
#define I2S_FMT0    (*(volatile uint32_t *)(H3_I2S0_BASE + 0x04))
#define I2S_FMT1    (*(volatile uint32_t *)(H3_I2S0_BASE + 0x08))
#define I2S_ISTA    (*(volatile uint32_t *)(H3_I2S0_BASE + 0x0C))
#define I2S_FCTL    (*(volatile uint32_t *)(H3_I2S0_BASE + 0x14))
#define I2S_FSTA    (*(volatile uint32_t *)(H3_I2S0_BASE + 0x18))
#define I2S_INT     (*(volatile uint32_t *)(H3_I2S0_BASE + 0x1C))
#define I2S_TXFIFO  (*(volatile uint32_t *)(H3_I2S0_BASE + 0x20))
#define I2S_CLKD    (*(volatile uint32_t *)(H3_I2S0_BASE + 0x24))
#define I2S_TXCNT   (*(volatile uint32_t *)(H3_I2S0_BASE + 0x28))
#define I2S_CHCFG   (*(volatile uint32_t *)(H3_I2S0_BASE + 0x30))
#define I2S_TXCHSEL (*(volatile uint32_t *)(H3_I2S0_BASE + 0x34))
#define I2S_TXCHMAP (*(volatile uint32_t *)(H3_I2S0_BASE + 0x44))

/* ---------- Биты I2S_CTL (даташит 8.6.7.1) ---------- */
#define I2S_CTL_BCLK_OUT  (1u << 18)   /* BCLK — ведущий (выход) */
#define I2S_CTL_LRCK_OUT  (1u << 17)   /* LRCK — ведущий (выход) */
#define I2S_CTL_SDO0_EN   (1u << 8)    /* включить выход SDO0 */
#define I2S_CTL_MODE(n)   ((n) << 4)   /* 0=PCM,1=Left(LJ),2=Right */
#define I2S_CTL_TX_EN     (1u << 2)
#define I2S_CTL_RX_EN     (1u << 1)
#define I2S_CTL_GL_EN     (1u << 0)

/* ---------- Биты I2S_FMT0 (даташит 8.6.7.2, sun8i) ---------- */
#define I2S_FMT0_LRCKPER(v)  (((v) - 1) << 8)   /* BCLK на канал, N+1 */
#define I2S_FMT0_SR(v)       ((v) << 4)         /* разрешение отсчёта: 3=16bit */
#define I2S_FMT0_SW(v)       ((v) << 0)         /* ширина слота: 7=32bit */

/* ---------- Биты I2S_FMT1 (sun8i) ---------- */
#define I2S_FMT1_SEXT(v)     ((v) << 4)         /* 0: нули в младших битах (LSB pad) */

/* ---------- Биты I2S_FCTL (даташит 8.6.7.6) ---------- */
#define I2S_FCTL_FLUSH_TX    (1u << 25)
#define I2S_FCTL_FLUSH_RX    (1u << 24)
#define I2S_FCTL_TXTL(v)     ((v) << 12)        /* уровень срабатывания TX empty */
#define I2S_FCTL_TXIM        (1u << 2)          /* 0=MSB,1=LSB */

/* ---------- Биты I2S_INT (даташит 8.6.7.8) ---------- */
#define I2S_INT_TX_DRQ       (1u << 7)          /* разрешить DRQ при пустом TX FIFO */

/* ---------- Биты I2S_CLKD (даташит 8.6.7.10) ---------- */
#define I2S_CLKD_MCLKO_EN    (1u << 8)
#define I2S_CLKD_BCLK(v)     ((v) << 4)         /* делитель BCLK: 5=/8 */
#define I2S_CLKD_MCLK(v)     ((v) << 0)         /* делитель MCLK: 2=/2 */

/* ---------- Биты I2S_CHCFG (sun8i) ---------- */
#define I2S_CHCFG_TXSLOT(v)  ((v) - 1)          /* TX-слоты (1..2) */
#define I2S_CHCFG_RXSLOT(v)  (((v) - 1) << 4)

/* ---------- Биты I2S_TXCHSEL (sun8i) ---------- */
#define I2S_TXCHSEL_OFF(v)   ((v) << 12)        /* смещение TX относительно LRCK */
#define I2S_TXCHSEL_CHEN(v)  ((((1u << (v)) - 1)) << 4)  /* включение каналов */
#define I2S_TXCHSEL_CHSEL(v) (((v) - 1))        /* каналы */

#define SD_PIN 10
#define SR     48000u

/* DMA-буфер: 64 КБ = 16384 слова = 8192 пары; 2 полубуфера по 4096 пар. */
#define DMA_BUF_BYTES   (64 * 1024)
#define DMA_BUF_WORDS   (DMA_BUF_BYTES / 4)
#define DMA_TOTAL_PAIRS (DMA_BUF_WORDS / 2)
#define DMA_HALF_PAIRS  (DMA_TOTAL_PAIRS / 2)

extern unsigned char _dma_buf_start[];
extern unsigned char _dma_buf_end[];
static volatile uint32_t *g_dma = (volatile uint32_t *)(uintptr_t)_dma_buf_start;

static int g_ready = 0;
static int g_run   = 0;

/* Таблица синуса на 256 точек (int16). */
static const int16_t sin_tab[256] = {
     0,804,1607,2410,3211,4011,4807,5601,6392,7179,7961,8739,9511,10278,11038,11792,
     12539,13278,14009,14732,15446,16150,16845,17530,18204,18867,19519,20159,20787,21402,22004,22594,
     23169,23731,24278,24811,25329,25831,26318,26789,27244,27683,28105,28510,28898,29268,29621,29955,
     30272,30571,30851,31113,31356,31580,31785,31970,32137,32284,32412,32520,32609,32678,32728,32757,
     32767,32757,32728,32678,32609,32520,32412,32284,32137,31970,31785,31580,31356,31113,30851,30571,
     30272,29955,29621,29268,28898,28510,28105,27683,27244,26789,26318,25831,25329,24811,24278,23731,
     23169,22594,22004,21402,20787,20159,19519,18867,18204,17530,16845,16150,15446,14732,14009,13278,
     12539,11792,11038,10278,9511,8739,7961,7179,6392,5601,4807,4011,3211,2410,1607,804,0,
    -804,-1607,-2410,-3211,-4011,-4807,-5601,-6392,-7179,-7961,-8739,-9511,-10278,-11038,-11792,-12539,
    -13278,-14009,-14732,-15446,-16150,-16845,-17530,-18204,-18867,-19519,-20159,-20787,-21402,-22004,-22594,
    -23169,-23731,-24278,-24811,-25329,-25831,-26318,-26789,-27244,-27683,-28105,-28510,-28898,-29268,-29621,
    -29955,-30571,-30851,-31113,-31356,-31580,-31785,-31970,-32137,-32284,-32412,-32520,-32609,-32678,-32728,
    -32757,-32767,-32757,-32728,-32678,-32609,-32520,-32412,-32284,-32137,-31970,-31785,-31580,-31356,-31113,
    -30851,-30571,-30272,-29955,-29621,-29268,-28898,-28510,-28105,-27683,-27244,-26789,-26318,-25831,-25329,
    -24811,-24278,-23731,-23169,-22594,-22004,-21402,-20787,-20159,-19519,-18867,-18204,-17530,-16845,-16150,
    -15446,-14732,-14009,-13278,-12539,-11792,-11038,-10278,-9511,-8739,-7961,-7179,-6392,-5601,-4807,-4011,
    -3211,-2410,-1607,-804
};
static inline int32_t sin_t(uint32_t phase) { return (int32_t)sin_tab[(phase >> 24) & 0xFF]; }

static int pll_audio_enable(void)
{
    /* Включить AudioPLL 24.576 МГц (N=14 -> 24.576M), ждать LOCK. */
    if ((H3_CCU_PLL_AUDIO_CTRL & (1u << 31)) && (H3_CCU_PLL_AUDIO_CTRL & (1u << 28)))
        return 0;
    H3_CCU_PLL_AUDIO_PAT = 0xC000AC02u;
    H3_CCU_PLL_AUDIO_CTRL = (1u << 31) | (1u << 24) | (13u << 8) | 13u;  /* N=14 */
    udelay(3000);
    return (H3_CCU_PLL_AUDIO_CTRL & (1u << 28)) ? 0 : -1;
}

void i2s_mute(int m)
{
    set_pin_data(PORTA, SD_PIN, m ? 0 : 1);
}

int i2s_init(void)
{
    if (pll_audio_enable() != 0) return -1;

    /* Такт I2S0: gating2 bit12, reset bit12, модульный такт.
       Источник модульного такта — AudioPLL 24.576M (mux=0, bits17:16). */
    H3_CCU_GATING2 |= (1u << 12);
    /* Reset: сбросить в активное состояние -> снять. */
    H3_CCU_RST3 &= ~(1u << 12);
    __asm volatile("dsb" ::: "memory");
    udelay(10);
    H3_CCU_RST3 |= (1u << 12);
    __asm volatile("dsb" ::: "memory");
    udelay(10);

    H3_CCU_I2SPCM0_CLK = (H3_CCU_I2SPCM0_CLK & ~H3_I2SPCM0_CLKSRC_MASK)
                       | H3_I2SPCM0_MUX_PLLAUDIO | H3_I2SPCM0_GATE;
    udelay(2000);

    /* Пины: PA18=SYNC, PA19=CLK, PA20=DOUT (F2 в CFG2); PA10=SD (выход, hi=работа). */
    uint32_t c2 = *(volatile uint32_t *)(PORTA + 0x08);
    c2 &= ~((0xFu << 8) | (0xFu << 12) | (0xFu << 16));
    c2 |=  (2u << 8) | (2u << 12) | (2u << 16);
    *(volatile uint32_t *)(PORTA + 0x08) = c2;

    set_pin_mode(PORTA, SD_PIN, 1);
    set_pin_data(PORTA, SD_PIN, 0);   /* начать с выключенным усилителем */

    /* ---------- Инициализация I2S (даташит 8.6.5.1) ---------- */
    /* Выключить модуль (GEN/TX/RX = 0). */
    I2S_CTL = 0;
    __asm volatile("dsb" ::: "memory");

    /* Сбросить FIFO (TX/RX) и счётчики. */
    I2S_FCTL |= (I2S_FCTL_FLUSH_TX | I2S_FCTL_FLUSH_RX);
    __asm volatile("dsb" ::: "memory");
    udelay(10);
    I2S_TXCNT = 0;
    __asm volatile("dsb" ::: "memory");

    /* 48 кГц стерео 16 бит: LRCK_PERIOD=32 BCLK/канал, SR=16, SW=32.
       BCLK=PLL/8=3.072M, MCLK=PLL/2=12.288M. */
    I2S_FMT0 = I2S_FMT0_LRCKPER(32) | I2S_FMT0_SR(3) | I2S_FMT0_SW(7);
    I2S_FMT1 = I2S_FMT1_SEXT(0);                 /* нули в младших битах (LSB pad) */

    I2S_CLKD  = I2S_CLKD_MCLKO_EN | I2S_CLKD_BCLK(5) | I2S_CLKD_MCLK(2);

    I2S_TXCHMAP = 0x76543210;
    I2S_TXCHSEL = I2S_TXCHSEL_OFF(1) | I2S_TXCHSEL_CHEN(2) | I2S_TXCHSEL_CHSEL(2);
    I2S_CHCFG   = I2S_CHCFG_TXSLOT(2) | I2S_CHCFG_RXSLOT(2);

    /* TX FIFO: TXTL=64 (0x40<<12), TXIM=0, RXOM=0.
       TXIM=0 значит "данные в СТАРШИХ битах" (MSB) — критично для формата
       [L<<16][R<<16]; при TXIM=1 FIFO взяло бы младшие биты (0)
       и выдало бы тишину. */
    I2S_FCTL = ((I2S_FCTL & ~(0x7Fu << 12)) | (0x40u << 12))
             & ~(I2S_FCTL_TXIM | 0x3u);

    /* Включить передачу: master BCLK/LRCK, Left-Justified (I2S = Left+offset),
       SDO0, TX_EN, GL_EN. */
    I2S_CTL = I2S_CTL_BCLK_OUT | I2S_CTL_LRCK_OUT | I2S_CTL_MODE(1)
            | I2S_CTL_SDO0_EN | I2S_CTL_TX_EN | I2S_CTL_GL_EN;
    __asm volatile("dsb" ::: "memory");
    udelay(1000);

    /* Пометить DMA-секции как uncached: буфер и LLI-дескрипторы (.coherent).
       КРИТИЧНО: LLI читает сам DMAC, поэтому он обязан лежать в uncached-памяти,
       иначе DMAC читает устаревшие D-cache-линии (кэш обходит DMA).
       mmu_mark_uncached() обрабатывает и section-записи (FEL), и L2 large pages
       (U-Boot), помечая шагом по 16 КБ — покрываем весь диапазон. */
    extern unsigned char _coherent_start[];
    extern unsigned char _coherent_end[];
    for (uint32_t a = (uint32_t)(uintptr_t)_dma_buf_start;
         a < (uint32_t)(uintptr_t)_dma_buf_end; a += 0x4000u)
        mmu_mark_uncached(a);
    for (uint32_t a = (uint32_t)(uintptr_t)_coherent_start;
         a < (uint32_t)(uintptr_t)_coherent_end; a += 0x4000u)
        mmu_mark_uncached(a);
    /* Почистить D-cache для секций, где лежат буфер и LLI. */
    {
        uint32_t b = (uint32_t)(uintptr_t)_dma_buf_start & ~0x1Fu;
        uint32_t e = (uint32_t)(uintptr_t)_dma_buf_end;
        for (uint32_t a = b; a < e; a += 32)
            __asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a));
        uint32_t cb = (uint32_t)(uintptr_t)_coherent_start & ~0x1Fu;
        uint32_t ce = ((uint32_t)(uintptr_t)_coherent_end + 31) & ~0x1Fu;
        for (uint32_t a = cb; a < ce; a += 32)
            __asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a));
        __asm volatile("dsb" ::: "memory");
    }

    if (h3_dma_audio_init((uint32_t)(uintptr_t)_dma_buf_start, DMA_BUF_BYTES) != 0)
        return -1;

    g_ready = 1;
    return 0;
}

/* Синтез: каждый полубуфер содержит целое число периодов, фаза
   начинается с 0 — бесшовный стык LLI. */
static void synth(int freq, uint32_t amp)
{
    uint32_t nsamp = DMA_HALF_PAIRS;
    uint32_t best_p = 1;
    uint64_t best_err = ~0ull;
    for (uint32_t p = 1; p <= nsamp / 2; p++) {
        uint64_t f  = (uint64_t)p * SR / nsamp;
        uint64_t err = f > (uint64_t)freq ? f - (uint64_t)freq : (uint64_t)freq - f;
        if (err < best_err) { best_err = err; best_p = p; }
    }
    uint32_t step = (uint32_t)(((uint64_t)best_p << 32) / nsamp);
    for (uint32_t half = 0; half < 2; half++) {
        uint32_t phase = 0;
        for (uint32_t i = 0; i < nsamp; i++) {
            int32_t s = sin_t(phase);
            int16_t v = (int16_t)(((int64_t)s * (int64_t)amp) / 32767);
            uint32_t idx = (half * nsamp + i) * 2;
            g_dma[idx]     = (uint32_t)(uint16_t)v << 16;
            g_dma[idx + 1] = (uint32_t)(uint16_t)v << 16;
            phase += step;
        }
    }
    __asm volatile("dsb" ::: "memory");
}

void i2s_play_freq(int freq)
{
    if (!g_ready) return;
    if (g_run) i2s_stop();

    i2s_mute(0);                       /* включить усилитель */
    synth(freq, 32767 / 2);

    /* Вымыть буфер DMA из D-cache в физическую память: DMAC читает данные
       по физическому адресу, минуя кэш. H3 НЕ кэш-когерентен для DMA,
       поэтому без этого DMAC читает устаревшие данные из буфера. */
    dcc_clean_range((uint32_t)(uintptr_t)_dma_buf_start, DMA_BUF_BYTES);

    /* Порядок Linux sun4i_i2s_start_playback: сбросить TX FIFO, обнулить TXCNT,
       TX_EN, TX_DRQ. I2S уже включён (init), просто запускаем DMA. */
    I2S_CTL &= ~I2S_CTL_TX_EN;         /* TX выключен на время запуска */
    I2S_FCTL |= I2S_FCTL_FLUSH_TX;
    __asm volatile("dsb" ::: "memory");
    udelay(10);
    I2S_TXCNT = 0;
    I2S_CTL |= I2S_CTL_TX_EN;          /* TX включён */
    I2S_INT  |= I2S_INT_TX_DRQ;        /* DRQ включён */
    __asm volatile("dsb" ::: "memory");

    h3_dma_audio_start();
    g_run = 1;
}

void i2s_stop(void)
{
    if (!g_ready) return;
    if (g_run) {
        h3_dma_audio_stop();
        g_run = 0;
    }
    /* Тишина: выключить TX и усилитель. */
    I2S_CTL &= ~I2S_CTL_TX_EN;
    I2S_INT  &= ~I2S_INT_TX_DRQ;
    __asm volatile("dsb" ::: "memory");
    i2s_mute(1);
}