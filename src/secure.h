#ifndef SECURE_H
#define SECURE_H

/* Снятие TrustZone-защиты на Allwinner H3.
 *
 * Источник - datasheet H3 V1.2 4.18 (SMTA) и 4.11.4.5 (DMA_SECURE_REG).
 * (Ранее в KNOWLEDGE.md адреса защиты были взяты из внешнего референса под
 *  именем "TZPC" с адресами 0x01C23410..20 без проверки. По даташиту это
 *  SMTA - Secure Memory Touch Arbiter, и регистры там SET/CLR, а не
 *  "записать 0 в DEC".)
 *
 * SMTA (0x01C23400), порт x имеет три регистра (datasheet 4.18.3):
 *   STA_SET (write-1-to-set non-secure), CLR (write-1-to-set secure), STA(RO).
 *   DECPORT1: bit1=DMA, bit4=USB_OTG
 *   DECPORT2: bit3=CCU
 *
 * DMA канальная защита (DMA base + 0x20): бит канала 0=Secure,1=Non-secure.
 * Если ядро U-Boot перевело CPU в Non-Secure, Secure-DMA-каналы недоступны.
 */

void secure_unlock(void);

#endif /* SECURE_H */