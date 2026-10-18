#include <stdint.h>
#include <string.h>

#include "h3.h"
#include "h3_ccu.h"
#include "h3_de2.h"
#include "display_timing.h"
#include "uart.h"

/* DE2 MUX0 + UI-канал. Дословно как ref/pico-retro-new-gen h3_de2.c
   (эталон U-Boot sunxi_de2.c, (C) 2017 Jernej Skrabec), через структуры
   регистров — без ручных оффсетов, чтобы не разъехались. */

#define BIT(nr) (1UL << (nr))

static void clock_set_pll_de(const uint32_t clk)
{
    /* VIDEO rate = 24000000 * n / m, целочисленный режим. */
    const uint32_t m = 2;   /* шаги по 12 МГц */

    H3_CCU_PLL_DE_CTRL = H3_PLL_DE_ENABLE | H3_PLL_DE_INTEGER_MODE |
                         H3_PLL_DE_N(clk / (24000000 / m)) | H3_PLL_DE_M(m);

    uint32_t timeout = 10000000;
    while (!(H3_CCU_PLL_DE_CTRL & H3_PLL_DE_LOCK)) {
        if (--timeout == 0) {
            uart_print("WARN: PLL_DE not locked\n");
            break;
        }
    }
}

void de2_composer_init(void)
{
    clock_set_pll_de(432000000);

    H3_CCU_GATING1 |= H3_CCU_GATING1_DE;
    H3_CCU_RST1    |= H3_CCU_RST1_DE;

    H3_CCU_DE_CLK = (H3_CCU_DE_CLK & ~H3_DE_CLK_SRC_MASK) | H3_DE_CLK_SRC_PLL_DE;
    H3_CCU_DE_CLK |= H3_DE_CLK_GATE;
}

void de2_mode_set(const struct display_timing *mode, const uint32_t bpp,
                  uint32_t address)
{
    const uint32_t size = H3_DE2_WH(mode->hactive.typ, mode->vactive.typ);
    uint32_t channel;
    uint32_t format;

    H3_DE2->RST  |= 1;
    H3_DE2->GATE |= BIT(0);
    H3_DE2->BUS  |= BIT(0);
    H3_DE2->SEL  &= ~(1u);

    H3_DE2_MUX0_GLB->CTL    = H3_DE2_MUX_GLB_CTL_EN;
    H3_DE2_MUX0_GLB->STATUS = 0;
    H3_DE2_MUX0_GLB->SIZE   = size;

    for (channel = 0; channel < 4; channel++) {
        void *ch = (void *)(H3_DE2_MUX0_CHAN_BASE + H3_DE2_MUX_CHAN_SZ * channel);
        memset(ch, 0, (channel == 0) ? sizeof(struct T_H3_DE2_VI)
                                      : sizeof(struct T_H3_DE2_UI));
    }

    /* Полностью обнулить BLD — иначе мусор ломает наложение UI-канала. */
    memset(H3_DE2_MUX0_BLD, 0, sizeof(struct T_H3_DE2_BLD));

    H3_DE2_MUX0_BLD->FCOLOR_CTL   = 0x00000101;
    H3_DE2_MUX0_BLD->ROUTE        = 1;
    H3_DE2_MUX0_BLD->PREMULTIPLY  = 0;
    H3_DE2_MUX0_BLD->BKCOLOR      = 0xff000000;
    H3_DE2_MUX0_BLD->BLD_MODE[0]  = 0x03010301;
    H3_DE2_MUX0_BLD->OUTPUT_SIZE  = size;
    H3_DE2_MUX0_BLD->OUT_CTL      = (mode->flags & DISPLAY_FLAGS_INTERLACED) ? 2 : 0;
    H3_DE2_MUX0_BLD->CK_CTL       = 0;
    H3_DE2_MUX0_BLD->ATTR[0].FCOLOR = 0xff000000;
    H3_DE2_MUX0_BLD->ATTR[0].INSIZE = size;

    /* Отключить все остальные блоки. */
    *((volatile uint32_t *) H3_DE2_MUX0_VSU_BASE)  = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_GSU1_BASE) = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_GSU2_BASE) = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_GSU3_BASE) = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_FCE_BASE)  = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_BWS_BASE)  = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_LTI_BASE)  = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_PEAK_BASE) = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_ASE_BASE)  = 0;
    *((volatile uint32_t *) H3_DE2_MUX0_FCC_BASE)  = 0;

    H3_DE2_MUX0_CSC->CTL = 0;

    switch (bpp) {
    case 16:
        format = H3_DE2_UI_CFG_ATTR_FMT(H3_DE2_UI_FORMAT_RGB_565);
        break;
    case 32:
    default:
        format = H3_DE2_UI_CFG_ATTR_FMT(H3_DE2_UI_FORMAT_XRGB_8888);
        break;
    }

    /* UI-канал chan1: наш фреймбуфер. */
    H3_DE2_MUX0_UI->CFG[0].ATTR     = (H3_DE2_UI_CFG_ATTR_EN | format);
    H3_DE2_MUX0_UI->CFG[0].SIZE     = size;
    H3_DE2_MUX0_UI->CFG[0].COORD    = 0;
    H3_DE2_MUX0_UI->CFG[0].PITCH    = (bpp / 8) * mode->hactive.typ;
    H3_DE2_MUX0_UI->CFG[0].TOP_LADDR = address;
    H3_DE2_MUX0_UI->OVL_SIZE        = size;

    H3_DE2_MUX0_GLB->DBUFFER = 1;
}