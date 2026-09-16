#!/usr/bin/env bash
# OpenDMOfw - build driver.
#
# This box has no `make`, so this script replicates the Makefile target-for-target
# (same flags, same object layout) and drives arm-none-eabi-gcc directly.
#
#   ./build.sh            -> OP104 (5XL, 1248 dots / 300 dpi) [default]
#   ./build.sh OP57       -> OP57  (550,   672 dots / 300 dpi)
#   ./build.sh all        -> both
#
# Note: unlike the Makefile this passes -DMODEL_<MODEL> for every model,
# including -DMODEL_OP104. model.h only tests MODEL_OP57, so both drivers
# produce the same OP104 image.
#
# Output: build/<MODEL>/opendmo-<MODEL>.{elf,bin,map}
set -euo pipefail
cd "$(dirname "$0")"

# Prefer a toolchain already on PATH; fall back to the common Windows install.
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  TOOL="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
  export PATH="$TOOL:$PATH"
fi

MCUFLAGS="-mcpu=cortex-m0 -mthumb -mfloat-abi=soft"
# Build identifier reported by GS D 0x05 (see Makefile); "dev" outside git.
BUILD_ID="$(git describe --always --dirty --abbrev=8 2>/dev/null || echo dev)"
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
  local LDFLAGS="$MCUFLAGS -Tlinker/stm32f072xb.ld -nostartfiles \
    -Wl,--gc-sections -Wl,-Map=$BUILD/$TARGET.map --specs=nano.specs"

  echo "=== building $MODEL ($TARGET) ==="
  mkdir -p "$BUILD"
  local OBJS="" f o
  for f in $SRC; do
    o="$BUILD/${f%.c}.o"
    mkdir -p "$(dirname "$o")"
    arm-none-eabi-gcc $CFLAGS -DOPENDMO_BUILD="\"$BUILD_ID\"" -c "$f" -o "$o"
    OBJS="$OBJS $o"
  done
  arm-none-eabi-gcc $LDFLAGS $OBJS -o "$BUILD/$TARGET.elf"
  arm-none-eabi-objcopy -O binary "$BUILD/$TARGET.elf" "$BUILD/$TARGET.bin"
  arm-none-eabi-size "$BUILD/$TARGET.elf"
}

MODEL_ARG="${1:-OP104}"
case "$MODEL_ARG" in
  all)   build_model OP104; build_model OP57 ;;
  OP104) build_model OP104 ;;
  OP57)  build_model OP57 ;;
  *)     echo "unknown model '$MODEL_ARG' (use OP104, OP57 or all)" >&2; exit 2 ;;
esac
echo "done."
