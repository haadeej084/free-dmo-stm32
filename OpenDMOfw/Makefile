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
  -fno-common $(MODELDEF) $(INC) -DOPENDMO_BUILD='"$(BUILD_ID)"' $(CFLAGS_EXTRA)

LDSCRIPT := linker/stm32f072xb.ld
LDFLAGS  := $(MCUFLAGS) -T$(LDSCRIPT) -nostartfiles \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs

.PHONY: all clean size flash test stack renode

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

# Host tests: wire-layout transcription, the native parser and the USB stack
# against a register model (both models each). python3 is required; the C tests
# run when a host compiler (cc or gcc) is present. `set -e` so a failure in ANY
# of them fails the target, not only the last one.
USB_TEST_SRC = test/test_usb.c src/usb/usb_core.c src/usb/usb_desc.c src/usb/usb_printer.c

test:
	python3 test/test_protocol_wire.py
	@set -e; 	HOSTCC=$$(command -v cc || command -v gcc || true); 	if [ -z "$$HOSTCC" ]; then echo "skip native C tests (no host C compiler)"; exit 0; fi; 	for m in OP104 OP57; do 	  $$HOSTCC -Wall -Wextra -std=c11 -DMODEL_$$m -Isrc -o /tmp/opendmo-test-$$m 	    test/test_protocol.c src/printer/protocol.c; 	  /tmp/opendmo-test-$$m; 	  $$HOSTCC -Wall -Wextra -Wno-uninitialized -Wno-maybe-uninitialized -std=c11 	    -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc -o /tmp/opendmo-usbtest-$$m $(USB_TEST_SRC); 	  /tmp/opendmo-usbtest-$$m; 	done

# Worst-case stack: call graph + frame sizes from GCC, checked against the
# linker script's _Min_Stack_Size (0x800). See tools/stack_depth.py.
STACK_LIMIT := 2048
stack:
	@rm -rf $(BUILD)/stack && mkdir -p $(BUILD)/stack
	@for f in $(SRC); do 	  $(CC) $(CFLAGS) -fcallgraph-info=su -c $$f -o $(BUILD)/stack/$$(basename $$f .c).o || exit 1; 	done
	python3 tools/stack_depth.py --limit $(STACK_LIMIT) $(BUILD)/stack/*.ci

# The real image in the Renode emulator (boot/LED/SysTick smoke test and the
# head shift bit-stream + cost check). Needs Renode: RENODE=/path/to/renode.
RENODE ?= renode
renode: $(BUILD)/$(TARGET).elf
	RENODE=$(RENODE) python3 test/renode/smoke.py $(if $(filter OP57,$(MODEL)),OP57,OP104)
	RENODE=$(RENODE) python3 test/renode/head_shift.py $(if $(filter OP57,$(MODEL)),OP57,OP104)

clean:
	rm -rf $(BUILD)
