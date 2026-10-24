# Проект MultiTool bare-metal для Allwinner H3 (OPi Lite).
# HDMI 1024x600 + I2S(DMA) 3 частоты + PCF8574 кнопки (I2C bitbang).
# Загрузка: U-Boot с SD `go 0x40000000` или FEL+SPL. Отладка: JTAG.

SHELL      := sh
TOOLCHAIN  ?= /c/ARM/gcc-arm-none-eabi-15.2.1/bin
CC         := $(TOOLCHAIN)/arm-none-eabi-gcc
OBJCOPY    := $(TOOLCHAIN)/arm-none-eabi-objcopy
SIZE       := $(TOOLCHAIN)/arm-none-eabi-size

TARGET     := h3_bare
SRCDIR     := src
INCDIR     := include
OBJDIR     := build

SRC_C := main.c uart.c ports.c sys.c h3_mmu.c h3_dma.c i2s.c gic.c \
         h3_i2c_bb.c pcf8574.c display.c h3_de2.c h3_lcd.c h3_hdmi.c \
         dw_hdmi.c h3_display.c wifi_layer.c secure.c font8x8.c h3_sdc.c wpa_crypto.c wifi_net.c \
         h3_sdc0.c h3_smp.c fat.c retro_menu.c mem.c mailbox.c emulator.c usb_hc.c usb_msd.c usb_ohci.c usb_kbd.c
SRCS  := $(addprefix $(SRCDIR)/,$(SRC_C))
ASM   := boot.s fw_blob.S
OBJS  := $(OBJDIR)/boot.o $(OBJDIR)/fw_blob.o $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

CFLAGS := -mcpu=cortex-a7 -mfpu=neon -mfloat-abi=hard \
          -O2 -g -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
          -std=gnu99 -fno-stack-protector -fno-common \
          -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -MMD -MP \
          -I$(INCDIR) -Isrc
LDFLAGS := -T linker.ld -nostartfiles -nostdlib -Wl,--no-warn-rwx-segments -lgcc

# Автозависимости от заголовков (генерируются -MMD; см. %.d ниже).
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

# Порядок: объекты создаются до попытки включить их .d-файлы.
$(DEPS): ;

.PHONY: all clean bin bump

# Скрипт инкремента версии (вызывается один раз за сборку).
BUMP_SCRIPT := tools/scripts/bump_version.sh
VERSION_H   := include/version.h
MSYS_BIN    := /c/msys64/usr/bin

# Увеличить версию ДО компиляции. Вызов через рекурсивный make обязателен:
# при `make -j` цели bump и $(TARGET).elf иначе стартуют параллельно, и
# main.c успевает собраться со старой версией (в логе W.907 вместо W.908).
all: bump
	@$(MAKE) --no-print-directory $(TARGET).elf
bin: bump
	@$(MAKE) --no-print-directory $(TARGET).elf
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

bump:
	@PATH="$(MSYS_BIN):$$PATH" sh $(BUMP_SCRIPT) $(VERSION_H) >/dev/null

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/boot.o: boot.s | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/fw_blob.o: $(SRCDIR)/fw_blob.S fw_nic.bin | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# main.c должен перекомпилироваться при изменении версии.
$(OBJDIR)/main.o: $(SRCDIR)/main.c $(VERSION_H) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(OBJS) -T linker.ld -nostartfiles -nostdlib \
	  -Wl,--no-warn-rwx-segments -lgcc -o $@
	$(SIZE) $@

clean:
	rm -rf $(OBJDIR) $(TARGET).elf $(TARGET).bin