# OpenDMOfw - firmware build
# Toolchain: arm-none-eabi-gcc (bare-metal, no vendor-SDK needed).
# Output: build/OP104/opendmo-OP104.* (default) or build/OP57/opendmo-OP57.*.

# Default build = OP104 (5XL, 4"/300 dpi), plain `make`. Build the 57 mm
# (550) variant with:
#   make MODEL=OP57
ifeq ($(MODEL),OP57)
TARGET   := opendmo-OP57
BUILD    := build/OP57
MODELDEF := -DMODEL_OP57
else
TARGET   := opendmo-OP104
BUILD    := build/OP104
MODELDEF :=
endif

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

# Build identifier reported by GS D 0x05, so a fieldwork report can name the
# exact image it was measured on. Outside a git checkout this becomes "dev".
BUILD_ID ?= $(shell git describe --always --dirty --abbrev=8 2>/dev/null || echo dev)

# -Os: this is a 128KB-flash / 16KB-RAM target, code size matters.
CFLAGS := $(MCUFLAGS) -Os -g3 -std=c11 -ffreestanding \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter \
  -fno-common $(MODELDEF) $(INC) -DOPENDMO_BUILD='"$(BUILD_ID)"'

LDSCRIPT := linker/stm32f072xb.ld
LDFLAGS  := $(MCUFLAGS) -T$(LDSCRIPT) -nostartfiles \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs

.PHONY: all clean size flash test

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

# Host tests: wire-layout transcription + native parser (both models).
# python3 is required; a host C compiler (cc/gcc) runs test_protocol.c when present.
test:
	python3 test/test_protocol_wire.py
	@if command -v cc >/dev/null 2>&1; then \
	  cc -Wall -Wextra -std=c11 -Isrc -o /tmp/opendmo-test-op104 test/test_protocol.c src/printer/protocol.c && /tmp/opendmo-test-op104; \
	  cc -Wall -Wextra -std=c11 -DMODEL_OP57 -Isrc -o /tmp/opendmo-test-op57 test/test_protocol.c src/printer/protocol.c && /tmp/opendmo-test-op57; \
	elif command -v gcc >/dev/null 2>&1; then \
	  gcc -Wall -Wextra -std=c11 -Isrc -o /tmp/opendmo-test-op104 test/test_protocol.c src/printer/protocol.c && /tmp/opendmo-test-op104; \
	  gcc -Wall -Wextra -std=c11 -DMODEL_OP57 -Isrc -o /tmp/opendmo-test-op57 test/test_protocol.c src/printer/protocol.c && /tmp/opendmo-test-op57; \
	else \
	  echo "skip native test_protocol.c (no host C compiler)"; \
	fi

clean:
	rm -rf $(BUILD)
