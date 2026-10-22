#include <stdint.h>
#include <string.h>

#include "h3.h"
#include "h3_ccu.h"
#include "display_timing.h"
#include "dw_hdmi.h"
#include "uart.h"
#include "sys.h"

/* HDMI подсистема: PHY (0x01EF0000) + ядро DW-HDMI (0x01EE0000).
   Источник: ref/pico-retro-new-gen h3_hdmi.c (Skrabec/van Vught),
   значения PHY взяты как есть из BSP Allwinner (нет документации). */

#define BIT(nr) (1UL << (nr))

/* ---- PHY дефайны ---- */
#define CCU_PLL_VIDEO_CTRL_EN        (1u << 31)
#define CCU_PLL_VIDEO_CTRL_LOCK      (1u << 28)
#define CCU_PLL_VIDEO_INTEGER_MODE   (1u << 24)
#define CCU_PLL_VIDEO_N_SHIFT        8u
#define CCU_PLL_VIDEO_N_MASK         (0x7f << CCU_PLL_VIDEO_N_SHIFT)
#define CCU_PLL_VIDEO_M_SHIFT        0u
#define CCU_PLL_VIDEO_M_MASK         (0xf << CCU_PLL_VIDEO_M_SHIFT)

static uint32_t hdmi_get_divider(uint32_t clock)
{
    /* Доступны только 4 делителя PHY из-за отсутствия документации. */
    if (clock <= 27000000)  return 11;
    if (clock <= 74250000)  return 4;
    if (clock <= 148500000) return 2;
    return 1;
}

static void hdmi_phy_init(void)
{
    uint32_t tmo;
    uint32_t tmp;

    H3_HDMI_PHY_CTRL = 0;

    H3_HDMI_PHY_CTRL |= BIT(0);
    udelay(5);

    H3_HDMI_PHY_CTRL |= BIT(16);
    H3_HDMI_PHY_CTRL |= BIT(1);
    udelay(10);

    H3_HDMI_PHY_CTRL |= BIT(2);
    udelay(5);

    H3_HDMI_PHY_CTRL |= BIT(3);
    udelay(40);

    H3_HDMI_PHY_CTRL |= BIT(19);
    udelay(100);

    H3_HDMI_PHY_CTRL |= BIT(18);

    H3_HDMI_PHY_CTRL |= (7u << 4);

    tmo = micros() + 2000;
    while ((H3_HDMI_PHY_STATUS & 0x80) == 0) {
        if (micros() > tmo) {
            uart_print("Warning: HDMI PHY init timeout!\n");
            break;
        }
    }

    H3_HDMI_PHY_CTRL |= (0xF << 8);
    H3_HDMI_PHY_CTRL |= BIT(7);

    H3_HDMI_PHY_PLL = 0x39dc5040;
    H3_HDMI_PHY_CLK = 0x80084343;
    udelay(10000);

    H3_HDMI_PHY_UNK3 = 1;
    H3_HDMI_PHY_PLL |= BIT(25);
    udelay(100000);

    tmp = (H3_HDMI_PHY_STATUS & 0x1f800) >> 11;

    H3_HDMI_PHY_PLL |= (BIT(31) | BIT(30));
    H3_HDMI_PHY_PLL |= tmp;

    H3_HDMI_PHY_CTRL = 0x01FF0F7F;
    H3_HDMI_PHY_UNK1 = 0x80639000;
    H3_HDMI_PHY_UNK2 = 0x0F81C405;

    /* Включить чтение регистров контроллера. */
    H3_HDMI_PHY_READ_EN = 0x54524545;
    /* Расшифровать смещения регистров. */
    H3_HDMI_PHY_UNSCRAMBL = 0x42494E47;
}

static int hdmi_get_plug_in_status(void)
{
    return !!(H3_HDMI_PHY_STATUS & (1u << 19));
}

static void hdmi_wait_for_hpd(void)
{
    uint32_t start;

    start = micros();
    do {
        if (hdmi_get_plug_in_status())
            return;
        udelay(100);
    } while ((micros() - start) < 300);

    /* 7" HDMI-панели часто не дают HPD — принудительно продолжаем
       (аналог hdmi_force_hotplug=1 в Raspberry Pi config.txt). */
    (void)start;
}

static void hdmi_phy_set(uint32_t clock)
{
    uint32_t div = hdmi_get_divider(clock);
    uint32_t tmp;

    switch (div) {
    case 1:
        H3_HDMI_PHY_PLL = 0x30dc5fc0;
        H3_HDMI_PHY_CLK = 0x800863C0;
        udelay(1000 * 10);
        H3_HDMI_PHY_UNK3 = 0x00000001;
        H3_HDMI_PHY_PLL |= BIT(25);
        udelay(1000 * 200);
        tmp = (H3_HDMI_PHY_STATUS & 0x1f800) >> 11;
        H3_HDMI_PHY_PLL |= (BIT(31) | BIT(30));
        H3_HDMI_PHY_PLL |= (tmp < 0x3d) ? (tmp + 2) : 0x3f;
        udelay(1000 * 100);
        H3_HDMI_PHY_CTRL = 0x01FFFF7F;
        H3_HDMI_PHY_UNK1 = 0x8063b000;
        H3_HDMI_PHY_UNK2 = 0x0F8246B5;
        break;
    case 2:
        H3_HDMI_PHY_PLL = 0x39dc5040;
        H3_HDMI_PHY_CLK = 0x80084381;
        udelay(1000 * 10);
        H3_HDMI_PHY_UNK3 = 0x00000001;
        H3_HDMI_PHY_PLL |= BIT(25);
        udelay(1000 * 100);
        tmp = (H3_HDMI_PHY_STATUS & 0x1f800) >> 11;
        H3_HDMI_PHY_PLL |= (BIT(31) | BIT(30));
        H3_HDMI_PHY_PLL |= tmp;
        H3_HDMI_PHY_CTRL = 0x01FFFF7F;
        H3_HDMI_PHY_UNK1 = 0x8063a800;
        H3_HDMI_PHY_UNK2 = 0x0F81C485;
        break;
    case 4:
        H3_HDMI_PHY_PLL = 0x39dc5040;
        H3_HDMI_PHY_CLK = 0x80084343;
        udelay(1000 * 10);
        H3_HDMI_PHY_UNK3 = 0x00000001;
        H3_HDMI_PHY_PLL |= BIT(25);
        udelay(1000 * 100);
        tmp = (H3_HDMI_PHY_STATUS & 0x1f800) >> 11;
        H3_HDMI_PHY_PLL |= (BIT(31) | BIT(30));
        H3_HDMI_PHY_PLL |= tmp;
        H3_HDMI_PHY_CTRL = 0x01FFFF7F;
        H3_HDMI_PHY_UNK1 = 0x8063b000;
        H3_HDMI_PHY_UNK2 = 0x0F81C405;
        break;
    case 11:
        H3_HDMI_PHY_PLL = 0x39dc5040;
        H3_HDMI_PHY_CLK = 0x8008430a;
        udelay(1000 * 10);
        H3_HDMI_PHY_UNK3 = 0x00000001;
        H3_HDMI_PHY_PLL |= BIT(25);
        udelay(1000 * 100);
        tmp = (H3_HDMI_PHY_STATUS & 0x1f800) >> 11;
        H3_HDMI_PHY_PLL |= (BIT(31) | BIT(30));
        H3_HDMI_PHY_PLL |= tmp;
        H3_HDMI_PHY_CTRL = 0x01FFFF7F;
        H3_HDMI_PHY_UNK1 = 0x8063b000;
        H3_HDMI_PHY_UNK2 = 0x0F81C405;
        break;
    }
}

static void clock_set_pll_video_factors(uint32_t m, uint32_t n)
{
    /* VIDEO rate = 24000000 * n / m */
    H3_CCU_PLL_VIDEO_CTRL = H3_PLL_VIDEO_EN | H3_PLL_VIDEO_INTEGER
                            | H3_PLL_VIDEO_N(n) | H3_PLL_VIDEO_M(m);

    uint32_t timeout = 10000000;
    while (!(H3_CCU_PLL_VIDEO_CTRL & H3_PLL_VIDEO_LOCK)) {
        if (--timeout == 0) {
            uart_print("WARN: PLL_VIDEO not locked\n");
            break;
        }
    }
}

static void hdmi_pll_set(uint32_t clk_khz)
{
    uint32_t n, m, div, diff;
    uint32_t best_n = 0, best_m = 0, best_diff = 0x0FFFFFFF;

    div = hdmi_get_divider(clk_khz * 1000);

    for (m = 1; m <= 16; m++) {
        n = (m * div * clk_khz) / 24000;
        if ((n >= 1) && (n <= 128)) {
            diff = clk_khz - (24000 * n) / m / div;
            if (diff < best_diff) {
                best_diff = diff;
                best_m = m;
                best_n = n;
            }
        }
    }

    clock_set_pll_video_factors(best_m, best_n);
}

void h3_lcdc_init(void);
void h3_lcdc_tcon1_mode_set(const struct display_timing *mode);
void h3_lcdc_enable(void);

static void hdmi_lcdc_init(const struct display_timing *edid)
{
    uint32_t div = hdmi_get_divider(edid->pixelclock.typ);

    H3_CCU_RST1    |= H3_CCU_RST1_TCON0;
    H3_CCU_GATING1 |= H3_CCU_GATING1_TCON0;
    H3_CCU_TCON0_CLK = H3_TCON0_CLK_GATE | H3_TCON0_CLK_M(div);

    h3_lcdc_init();
    h3_lcdc_tcon1_mode_set(edid);
    h3_lcdc_enable();
}

static int hdmi_phy_cfg(struct dw_hdmi *hdmi, unsigned int mpixelclock)
{
    (void)hdmi;
    hdmi_pll_set(mpixelclock / 1000);
    hdmi_phy_set(mpixelclock);
    return 0;
}

static struct dw_hdmi _hdmi = {
    .ioaddr        = H3_HDMI_BASE,
    .i2c_clk_high  = 0xd8,
    .i2c_clk_low   = 0xfe,
    .reg_io_width  = 1,
    .phy_set       = hdmi_phy_cfg,
};

int h3_hdmi_enable(const struct display_timing *edid, uint32_t bpp)
{
    (void)bpp;

    /* RGB888 на вход и выход видео-пути — без этого хардверные дефолты
       уводят сигнал в YUV и белый становится грязно-жёлтым. */
    _hdmi.hdmi_data.enc_in_bus_format  = MEDIA_BUS_FMT_RGB888_1X24;
    _hdmi.hdmi_data.enc_out_bus_format = MEDIA_BUS_FMT_RGB888_1X24;

    int ret = dw_hdmi_enable(&_hdmi, edid);
    if (ret)
        return ret;

    /* Принудительно отключить встроенный CSC (bypass) в DW-HDMI H3:
       0x180 уходит в bypass, пиксели идут из DE2 в PHY 1:1 без RGB->YUV. */
    *(volatile uint32_t *)(H3_HDMI_BASE + 0x180) = 0;

    hdmi_lcdc_init(edid);

    if (edid->flags & DISPLAY_FLAGS_VSYNC_LOW)
        H3_HDMI_PHY_POL |= 0x200;
    if (edid->flags & DISPLAY_FLAGS_HSYNC_LOW)
        H3_HDMI_PHY_POL |= 0x100;

    H3_HDMI_PHY_CTRL |= (0xF << 12);

    /* Снова перешифровать адреса (иначе BSP-драйвер не заработает).
       Dummy read нужен, иначе последняя запись не пишется корректно. */
    (void)*(volatile uint8_t *)H3_HDMI_BASE;
    H3_HDMI_PHY_UNSCRAMBL = 0;

    return 0;
}

int h3_hdmi_probe(void)
{
    /* PLL_VIDEO 288 МГц (24 МГц * 24 / 2) для внутреннего опроса (probe). */
    clock_set_pll_video_factors(2, 24);   /* 24МГц*24/2 = 288МГц */

    H3_CCU_HDMI_CLK = (H3_CCU_HDMI_CLK & ~H3_HDMI_CLK_PLL_MASK) | H3_HDMI_CLK_PLL_VIDEO;

    H3_CCU_RST1    |= H3_CCU_RST1_HDMI;
    H3_CCU_RST1    |= H3_CCU_RST1_HDMI2;
    H3_CCU_GATING1 |= H3_CCU_GATING1_HDMI;

    H3_CCU_HDMI_SLOW_CLK |= H3_HDMI_SLOW_DDC_GATE;
    H3_CCU_HDMI_CLK      |= H3_HDMI_CLK_GATE;

    hdmi_phy_init();

    hdmi_wait_for_hpd();

    memset(&_hdmi.hdmi_data, 0, sizeof(_hdmi.hdmi_data));

    _hdmi.ioaddr        = H3_HDMI_BASE;
    _hdmi.i2c_clk_high  = 0xd8;
    _hdmi.i2c_clk_low   = 0xfe;
    _hdmi.reg_io_width  = 1;
    _hdmi.phy_set       = hdmi_phy_cfg;

    dw_hdmi_init(&_hdmi);

    return 0;
}