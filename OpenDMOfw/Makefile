# OpenDMOfw - firmware build
# Toolchain: arm-none-eabi-gcc (bare-metal, no vendor-SDK needed).
# Output: build/<MODEL>/opendmo-<MODEL>.{elf,bin} (flash via SWD).

# MODEL: OP104 (default, 4"/300dpi) or OP57 (57 mm). Build the small one with:
#   make MODEL=OP57
MODEL    ?= OP104
TARGET   := opendmo-$(MODEL)
BUILD    := build/$(MODEL)

CROSS    := arm-none-eabi-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

# Cortex-M0, no FPU, Thumb.
MCUFLAGS := -mcpu=cortex-m0 -mthumb -mfloat-abi=soft

# Source files.
SRC := \
  src/startup.c \
  src/system.c \
  src/usb/usb_core.c \
  src/usb/usb_desc.c \
  src/usb/usb_printer.c \
  src/printer/protocol.c \
  src/printer/head.c \
  src/printer/motor.c \
  src/printer/thermal.c \
  src/config/store.c \
  src/main.c

INC := -Isrc

OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC))

# -Os: this is a 128KB-flash / 16KB-RAM target, code size matters.
CFLAGS := $(MCUFLAGS) -Os -g3 -std=c11 -ffreestanding \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter \
  -fno-common -DMODEL_$(MODEL) $(INC)

LDSCRIPT := linker/stm32f072x8.ld
LDFLAGS  := $(MCUFLAGS) -T$(LDSCRIPT) -nostartfiles \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs

.PHONY: all clean size flash

all: $(BUILD)/$(TARGET).bin size

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

# Flash via st-flash (open-source, ST-Link). See BUILD.md for alternatives.
flash: $(BUILD)/$(TARGET).bin
	st-flash write $< 0x08000000

clean:
	rm -rf $(BUILD)
