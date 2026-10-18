#ifndef H3_DE2_H
#define H3_DE2_H

#include <stdint.h>
#include "h3.h"

/* Дефайны DE2 MUX0 (0x01100000) — дословно из ref/pico-retro-new-gen h3_de2.h
   (структуры регистров U-Boot sunxi_de2.c). Никаких "упрощений" — смещения
   берутся из структур, а не вручную, чтобы не разъехались. */

#define H3_DE2_MUX0_BASE        (H3_DE_BASE + 0x100000)   /* 0x01100000 */

#define H3_DE2_MUX0_GLB_BASE    (H3_DE2_MUX0_BASE + 0x00000)
#define H3_DE2_MUX0_BLD_BASE    (H3_DE2_MUX0_BASE + 0x01000)
#define H3_DE2_MUX0_CHAN_BASE   (H3_DE2_MUX0_BASE + 0x02000)
#define H3_DE2_MUX_CHAN_SZ      0x1000
#define H3_DE2_MUX0_DCSC_BASE   (H3_DE2_MUX0_BASE + 0xB0000)
#define H3_DE2_MUX0_VSU_BASE    (H3_DE2_MUX0_BASE + 0x20000)
#define H3_DE2_MUX0_GSU1_BASE   (H3_DE2_MUX0_BASE + 0x30000)
#define H3_DE2_MUX0_GSU2_BASE   (H3_DE2_MUX0_BASE + 0x40000)
#define H3_DE2_MUX0_GSU3_BASE   (H3_DE2_MUX0_BASE + 0x50000)
#define H3_DE2_MUX0_FCE_BASE    (H3_DE2_MUX0_BASE + 0xA0000)
#define H3_DE2_MUX0_BWS_BASE    (H3_DE2_MUX0_BASE + 0xA2000)
#define H3_DE2_MUX0_LTI_BASE    (H3_DE2_MUX0_BASE + 0xA4000)
#define H3_DE2_MUX0_PEAK_BASE   (H3_DE2_MUX0_BASE + 0xA6000)
#define H3_DE2_MUX0_ASE_BASE    (H3_DE2_MUX0_BASE + 0xA8000)
#define H3_DE2_MUX0_FCC_BASE    (H3_DE2_MUX0_BASE + 0xAA000)

#ifndef __I
#define __I volatile const
#endif
#ifndef __IO
#define __IO volatile
#endif

typedef struct T_H3_DE2 {
	__IO uint32_t GATE;	///< 0x00
	__IO uint32_t BUS;	///< 0x04
	__IO uint32_t RST;	///< 0x08
	__IO uint32_t DIV;	///< 0x0C
	__IO uint32_t SEL;	///< 0x10
} H3_DE2_TypeDef;

typedef struct T_H3_DE2_GLB {
	__IO uint32_t CTL;		///< 0x00
	__IO uint32_t STATUS;
	__IO uint32_t DBUFFER;
	__IO uint32_t SIZE;
} H3_DE2_GLB_TypeDef;

typedef struct T_H3_DE2_BLD {
	__IO uint32_t FCOLOR_CTL;	/* 0x00 */
	struct {
		__IO uint32_t FCOLOR;	/* 0x04 + i*0x10 */
		__IO uint32_t INSIZE;
		__IO uint32_t OFFSET;
		__IO uint32_t RES;
	} ATTR[4];			/* 0x04..0x43 */
	__IO uint32_t RES1[15];		/* 0x44..0x7B */
	__IO uint32_t ROUTE;		/* 0x7C */
	__IO uint32_t PREMULTIPLY;	/* 0x80 */
	__IO uint32_t BKCOLOR;		/* 0x84 */
	__IO uint32_t OUTPUT_SIZE;	/* 0x88 */
	__IO uint32_t BLD_MODE[4];	/* 0x8C..0x9B */
	__IO uint32_t RES2[4];		/* 0x9C..0xAB */
	__IO uint32_t CK_CTL;		/* 0xAC */
	__IO uint32_t CK_CFG;		/* 0xB0 */
	__IO uint32_t RES3[2];		/* 0xB4..0xBB */
	__IO uint32_t CK_MAX[4];	/* 0xBC..0xCB */
	__IO uint32_t RES4[4];		/* 0xCC..0xDB */
	__IO uint32_t CK_MIN[4];	/* 0xDC..0xEB */
	__IO uint32_t RES5[3];		/* 0xEC..0xF7 */
	__IO uint32_t OUT_CTL;		/* 0xF8 */
} H3_DE2_BLD_TypeDef;

typedef struct T_H3_DE2_VI {
	struct {
		__IO uint32_t ATTR;
		__IO uint32_t SIZE;
		__IO uint32_t COORD;
		__IO uint32_t PITCH[3];
		__IO uint32_t TOP_LADDR[3];
		__IO uint32_t BOT_LADDR[3];
	} CFG[4];
	__IO uint32_t FCOLOR[4];
	__IO uint32_t TOP_HADDR[3];
	__IO uint32_t BOT_HADDR[3];
	__IO uint32_t OVL_SIZE[2];
	__IO uint32_t HORI[2];
	__IO uint32_t VERT[2];
} H3_DE2_VI_TypeDef;

typedef struct T_H3_DE2_UI {
	struct {
		__IO uint32_t ATTR;
		__IO uint32_t SIZE;
		__IO uint32_t COORD;
		__IO uint32_t PITCH;
		__IO uint32_t TOP_LADDR;
		__IO uint32_t BOT_LADDR;
		__IO uint32_t FCOLOR;
		__IO uint32_t RES;
	} CFG[4];
	__IO uint32_t TOP_HADDR;
	__IO uint32_t BOT_HADDR;
	__IO uint32_t OVL_SIZE;
} H3_DE2_UI_TypeDef;

typedef struct T_H3_DE2_CSC {
	__IO uint32_t CTL;
	__I  uint32_t RES[3];
	__IO uint32_t COEF11;
	__IO uint32_t COEF12;
	__IO uint32_t COEF13;
	__IO uint32_t COEF14;
	__IO uint32_t COEF21;
	__IO uint32_t COEF22;
	__IO uint32_t COEF23;
	__IO uint32_t COEF24;
	__IO uint32_t COEF31;
	__IO uint32_t COEF32;
	__IO uint32_t COEF33;
	__IO uint32_t COEF34;
} H3_DE2_CSC_TypeDef;

#define H3_DE2					((H3_DE2_TypeDef *) H3_DE_BASE)

#define H3_DE2_MUX0_GLB			((H3_DE2_GLB_TypeDef *) H3_DE2_MUX0_GLB_BASE)
#define H3_DE2_MUX0_BLD			((H3_DE2_BLD_TypeDef *) H3_DE2_MUX0_BLD_BASE)
#define H3_DE2_MUX0_UI			((H3_DE2_UI_TypeDef *) (H3_DE2_MUX0_CHAN_BASE + (1 * H3_DE2_MUX_CHAN_SZ)))
#define H3_DE2_MUX0_CSC			((H3_DE2_CSC_TypeDef *) H3_DE2_MUX0_DCSC_BASE)

#define H3_DE2_WH(w, h)				(((h - 1) << 16) | (w - 1))
#define H3_DE2_MUX_GLB_CTL_EN		(1U << 0)

#define H3_DE2_UI_CFG_ATTR_FMT(f)	((f & 0xf) << 8)
#define H3_DE2_UI_CFG_ATTR_EN		(1U << 0)
#define H3_DE2_UI_FORMAT_XRGB_8888	4U
#define H3_DE2_UI_FORMAT_RGB_565		10U

#endif /* H3_DE2_H */