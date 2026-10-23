OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)

/* h3_bare.bin грузится U-Boot'ом (go 0x40000000) или FEL+SPL на 0x40000000. */
MEMORY
{
  RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 512M
}

SECTIONS
{
  . = 0x40000000;

  /* Векторы исключений — в начале образа (VBAR = 0x40000000). */
  .vectors : {
    KEEP(*(.vectors))
    . = ALIGN(0x100);
  } > RAM

  .text : {
    *(.text)
    *(.text.*)
    . = ALIGN(4);
  } > RAM

  .rodata : {
    *(.rodata)
    *(.rodata.*)
    . = ALIGN(4);
  } > RAM

  .data : {
    _data_start = .;
    *(.data)
    *(.data.*)
    . = ALIGN(4);
    _data_end = .;
  } > RAM

  .bss (NOLOAD) : {
    _bss_start = .;
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(4);
    _bss_end = .;
  } > RAM

  /* Данные DMA, uncached (LLI-дескрипторы). После .bss на 1MB-границе,
     чтобы mmu_mark_uncached() не трогал секцию кода (0x40000000). */
  .coherent (NOLOAD) : {
    . = ALIGN(0x100000);
    _coherent_start = .;
    *(.coherent)
    *(.coherent.*)
    . = ALIGN(0x80);
    _coherent_end = .;
  } > RAM

  /* Почта CPU0<->CPU1: разделяемая uncached-область (см. mailbox.h/c). */
  .mailbox (NOLOAD) : {
    . = ALIGN(0x100000);
    _mailbox_start = .;
    *(.mailbox)
    *(.mailbox.*)
    . = ALIGN(0x80);
    _mailbox_end = .;
  } > RAM

  /* DMA-буфер звука: 64 КБ, пары [L<<16|R<<16]. uncached, своя 1MB секция. */
  .dma_buf (NOLOAD) : {
    . = ALIGN(0x100000);
    _dma_buf_start = .;
    . = . + 65536;
    _dma_buf_end = .;
  } > RAM

  /* Framebuffer HDMI 1024x600 XRGB8888 = 2.4 МБ. Своя 1MB секция. */
  .fb_hdmi (NOLOAD) : {
    . = ALIGN(0x100000);
    _fb_hdmi_start = .;
    . = . + (1024 * 600 * 4);
    _fb_hdmi_end = .;
  } > RAM

  . = ALIGN(16);
  _end = .;

  __stack_top = ORIGIN(RAM) + LENGTH(RAM) - 8;
}