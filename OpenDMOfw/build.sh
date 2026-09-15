#!/usr/bin/env bash
# OpenDMOfw - build driver.
#
# This box has no `make`, so this script replicates the Makefile target-for-target
# (same flags, same object layout) and drives arm-none-eabi-gcc directly.
#
#   ./build.sh            -> OP104 (5XL, 104 mm / 300 dpi)  [default]
#   ./build.sh OP57       -> OP57  (550,  57 mm / 300 dpi)
#   ./build.sh all        -> both
#
# Output: build/<MODEL>/opendmo-<MODEL>.{elf,bin,map}
set -euo pipefail
cd "$(dirname "$0")"

TOOL="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
export PATH="$TOOL:$PATH"

MCUFLAGS="-mcpu=cortex-m0 -mthumb -mfloat-abi=soft"
SRC="src/startup.c src/system.c src/usb/usb_core.c src/usb/usb_desc.c \
      src/usb/usb_printer.c src/printer/protocol.c src/printer/head.c \
      src/printer/motor.c src/printer/thermal.c src/config/store.c src/main.c"

build_model() {
  local MODEL="$1"
  local TARGET="opendmo-$MODEL"
  local BUILD="build/$MODEL"
  local CFLAGS="$MCUFLAGS -Os -g3 -std=c11 -ffreestanding \
    -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter \
    -fno-common -DMODEL_$MODEL -Isrc"
  local LDFLAGS="$MCUFLAGS -Tlinker/stm32f072x8.ld -nostartfiles \
    -Wl,--gc-sections -Wl,-Map=$BUILD/$TARGET.map --specs=nano.specs"

  echo "=== building $MODEL ($TARGET) ==="
  mkdir -p "$BUILD"
  local OBJS="" f o
  for f in $SRC; do
    o="$BUILD/${f%.c}.o"
    mkdir -p "$(dirname "$o")"
    arm-none-eabi-gcc $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
  done
  arm-none-eabi-gcc $LDFLAGS $OBJS -o "$BUILD/$TARGET.elf"
  arm-none-eabi-objcopy -O binary "$BUILD/$TARGET.elf" "$BUILD/$TARGET.bin"
  arm-none-eabi-size "$BUILD/$TARGET.elf"
}

case "${1:-all}" in
  all) build_model OP104; build_model OP57 ;;
  *)   build_model "$1" ;;
esac
echo "done."
