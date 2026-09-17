# OpenDMOfw - firmware build
# Toolchain: arm-none-eabi-gcc (bare-metal, no vendor-SDK needed).
# Output: build/<MODEL>/opendmo-<MODEL>.{elf,bin,map}

# Default build = OP57: the LabelWriter 550, the model whose mainboard carries
# the STM32F072 this firmware is written for. OP104 (1248-dot / 4" head) is the
# 5XL geometry; genuine 5XL and 550 Turbo boards use an STM32F407VET6 instead
# (DECISIONS D25), so that image has no board to run on until a port exists.
#   make                 # OP57, LabelWriter 550
#   make MODEL=OP104     # 4" head geometry
MODEL    ?= OP57
ifneq ($(filter $(MODEL),OP57 OP104),$(MODEL))
$(error MODEL must be OP57 or OP104)
endif
TARGET   := opendmo-$(MODEL)
BUILD    := build/$(MODEL)
MODELDEF := -DMODEL_$(MODEL)

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

# The warning-free rule was a convention no job checked: CI ran `make` with
# -Wall -Wextra and no -Werror, so a new warning went green. Both models build
# clean with it, so enforcing it is free. Override with `make WERROR=` while
# exploring.
WERROR ?= -Werror

# -Os: this is a 64KB-flash / 16KB-RAM target, code size matters.
CFLAGS := $(MCUFLAGS) -Os -g3 -std=c11 -ffreestanding $(WERROR) \
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
# against a register model (both models each). The thermal suite is built TWICE:
# once as shipped and once with HEAD_SAG_FULL_US forced to the value the genuine
# LW450 firmware implies, so the energy ceiling is proved to hold at a non-zero
# sag rather than only at the zero it currently ships with. python3 is required; the C tests
# run when a host compiler (cc or gcc) is present. `set -e` so a failure in ANY
# of them fails the target, not only the last one.
USB_TEST_SRC = test/test_usb.c src/usb/usb_core.c src/usb/usb_desc.c src/usb/usb_printer.c
E2E_TEST_SRC = test/test_e2e.c src/usb/usb_core.c src/usb/usb_desc.c src/usb/usb_printer.c src/printer/protocol.c
THERMAL_TEST_SRC = test/test_thermal.c src/printer/thermal.c src/printer/head.c
# motor.c had no host harness at all. Its behaviour is a SEQUENCE of GPIO
# writes, so the harness records their order and asserts on it - a
# final-state test cannot see a coil that is shorted only between two writes.
MOTOR_TEST_SRC = test/test_motor.c src/printer/motor.c
# system.c was compiled by no host suite and reached by no Renode assertion.
# A mutation run measured 3 of 33 real mutants killed, and the concrete cost
# was that the hot-pin guard on GS D 0x07 - the one stopping a host command
# from driving the 24 V heat gate - could be DELETED with every suite green,
# because test_protocol.c re-implements sys_pin_toggle() instead of calling it.
SYSTEM_TEST_SRC = test/test_system.c src/system.c

test:
	python3 test/test_protocol_wire.py
	@set -e; \
	HOSTCC=$$(command -v cc || command -v gcc || true); \
	if [ -z "$$HOSTCC" ]; then echo "skip native C tests (no host C compiler)"; exit 0; fi; \
	for m in OP57 OP104; do \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 -DMODEL_$$m -Isrc -o /tmp/opendmo-test-$$m \
	    test/test_protocol.c src/printer/protocol.c; \
	  /tmp/opendmo-test-$$m; \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 \
	    -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc -o /tmp/opendmo-usbtest-$$m $(USB_TEST_SRC); \
	  /tmp/opendmo-usbtest-$$m; \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 \
	    -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc -o /tmp/opendmo-e2etest-$$m $(E2E_TEST_SRC); \
	  /tmp/opendmo-e2etest-$$m; \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc \
	    -o /tmp/opendmo-thermaltest-$$m $(THERMAL_TEST_SRC) -lm; \
	  /tmp/opendmo-thermaltest-$$m; \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc \
	    -DHEAD_SAG_FULL_US=47 -o /tmp/opendmo-sagtest-$$m $(THERMAL_TEST_SRC) -lm; \
	  /tmp/opendmo-sagtest-$$m; \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc -Itest \
	    -o /tmp/opendmo-motortest-$$m $(MOTOR_TEST_SRC); \
	  /tmp/opendmo-motortest-$$m; \
	  $$HOSTCC -Wall -Wextra -Werror -std=c11 -DOPENDMO_HOST_TEST -DMODEL_$$m -Isrc -Itest \
	    -o /tmp/opendmo-systest-$$m $(SYSTEM_TEST_SRC); \
	  /tmp/opendmo-systest-$$m; \
	done

# Worst-case stack: call graph + frame sizes from GCC, checked against the
# linker script's _Min_Stack_Size (0x800). See tools/stack_depth.py.
STACK_LIMIT := 2048
stack:
	@rm -rf $(BUILD)/stack && mkdir -p $(BUILD)/stack
	@for f in $(SRC); do \
	  $(CC) $(CFLAGS) -fcallgraph-info=su -c $$f -o $(BUILD)/stack/$$(basename $$f .c).o || exit 1; \
	done
	python3 tools/stack_depth.py --limit $(STACK_LIMIT) $(BUILD)/stack/*.ci

# The real image in the Renode emulator: boot/LED/SysTick smoke test, the head
# shift bit stream + cost, the config EEPROM on both known parts, and the USB
# DFU hand-over. Needs Renode: RENODE=/path/to/renode.
RENODE ?= renode
renode: $(BUILD)/$(TARGET).elf
	RENODE=$(RENODE) python3 test/renode/smoke.py $(MODEL)
	RENODE=$(RENODE) python3 test/renode/head_shift.py $(MODEL)
	RENODE=$(RENODE) python3 test/renode/eeprom.py $(MODEL)
	RENODE=$(RENODE) python3 test/renode/dfu.py $(MODEL)
# fault.py costs about 15 s per model: four Renode launches, each with a
# 1 s emulated boot. It is the only suite that asserts what the firmware
# does to the print head when the CPU stops being trustworthy.
	RENODE=$(RENODE) python3 test/renode/fault.py $(MODEL)

clean:
	rm -rf $(BUILD)
