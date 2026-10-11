#include "secure.h"
#include "h3.h"

/* SMTA: Secure Memory Touch Arbiter (datasheet H3 V1.2 4.18).
   Base 0x01C23400. Port x:
     +0x04/x*0xC + 0x00 = STA (RO, защита)
     +0x04/x*0xC + 0x04 = SET (WO, 1 -> non-secure)
     +0x04/x*0xC + 0x08 = CLR (WO, 1 -> secure)
   Карта портов (Table 4-6):
     DECPORT1: bit1=DMA,  bit3=Crypto, bit4=USB_OTG, bit0=NAND
     DECPORT2: bit3=CCU,  bit2=System Control, bit6=RTC, bit4=DE
 */
#define SMTA_BASE       0x01C23400u
#define SMTA_DEC1_SET   (*(volatile uint32_t *)(SMTA_BASE + 0x14))
#define SMTA_DEC2_SET   (*(volatile uint32_t *)(SMTA_BASE + 0x20))
#define SMTA_DEC1_STA   (*(volatile uint32_t *)(SMTA_BASE + 0x10))
#define SMTA_DEC2_STA   (*(volatile uint32_t *)(SMTA_BASE + 0x1C))

#define SMTA_BIT_DMA    (1u << 1)   /* DECPORT1: DMA */
#define SMTA_BIT_CCU    (1u << 3)   /* DECPORT2: CCU */

/* DMA_SECURE_REG (datasheet 4.11.4.5): каналы DMA0-11, 0=Secure,1=Non-secure.
   В Non-Secure-мире Secure-каналы недоступны для управления. */
#define DMA_SECURE_REG  (*(volatile uint32_t *)(H3_DMA_BASE + 0x20))

void secure_unlock(void)
{
    /* SMTA: DMA через DECPORT1, CCU через DECPORT2 -> non-secure.
       (DMA_SECURE_REG - каналы - ставим ПОСЛЕ gate+reset DMA, см.
        h3_dma_audio_init: запись в DMA до включения его такта может
        не примениться.) */
    SMTA_DEC1_SET = SMTA_BIT_DMA;
    SMTA_DEC2_SET = SMTA_BIT_CCU;

    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}