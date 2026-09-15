# BUILD & FLASH — OpenDMOfw

## Requirements

- **arm-none-eabi-gcc** toolchain (e.g. Arm GNU Toolchain, or `gcc-arm-none-eabi`).
  On Windows: install the Arm GNU Toolchain and put `arm-none-eabi-gcc` in PATH.
  `make` via MSYS2/Git-Bash or WSL; on a host without `make`, use `./build.sh`
  (same flags, same object layout).
- To flash: **st-flash** (open-source, from `stlink-tools`) or OpenOCD, or
  STM32CubeProgrammer.

## Choosing the model

One codebase, two head widths (see `src/model.h`):

| Model | Command | Width | Output |
|-------|---------|-------|--------|
| **OP104** (default) | `make` | 104 mm / 4" @ 300 dpi, 1248 dots | `build/OP104/opendmo-OP104.bin` |
| OP57 | `make MODEL=OP57` | 57 mm, 672 dots | `build/OP57/opendmo-OP57.bin` |

OP104 matches the specs of the common 4"/300 dpi shipping-label class.

## Building

```sh
make                 # OP104 (default) -> build/OP104/opendmo-OP104.bin + size
make MODEL=OP57      # 57 mm variant   -> build/OP57/opendmo-OP57.bin
make clean MODEL=OP57
```

Equivalent without `make`:

```sh
./build.sh           # OP104 (default)
./build.sh OP57      # 57 mm variant
./build.sh all       # both
```

The build uses `--specs=nano.specs` (newlib-nano) and `-Os`; the memory budget is
128 KB flash / 16 KB SRAM. `make size` shows the usage.

> No vendor-SDK/CMSIS needed: `src/mcu.h` contains the register definitions.

## Flashing (SWD)

Connect an ST-Link (or compatible) to the SWD header.

```sh
make flash
# or explicitly:
st-flash write build/OP104/opendmo-OP104.bin 0x08000000
```

Alternative with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f0x.cfg \
  -c "program build/OP104/opendmo-OP104.bin 0x08000000 verify reset exit"
```

> **RDP Level 2 on stock printers:** SWD and the system bootloader are off.
> Lowering RDP mass-erases flash. Flash this image only onto an F072 that is
> already programmable. The firmware does **not** write option bytes (it will
> not set RDP1 or RDP2).
>
> Linker script: `linker/stm32f072xb.ld` (128 KB flash / 16 KB RAM — the **B**
> density of F072CBT6; `x8` would be the 64 KB part).

## Testing enumeration (no head connected)

After flashing, with only USB connected:

```sh
lsusb | grep 0922:002a                     # OP104 / 5XL (default)
lsusb | grep 0922:0028                     # OP57 / 550
# device ID (Linux, usblp): the printer returns the IEEE-1284 string via GET_DEVICE_ID
```

The genuine D.mo identity is VID `0x0922`; per-model PID `0x002A` (5XL) /
`0x0028` (550). Windows derives the driver-model match ID from the IEEE-1284
`MFG`/`MDL` fields, so the stock D.mo driver package binds.

## Setting config / reading roll status

Send the bulk commands from PROTOCOL.md to the printer endpoint (e.g. via a small
`libusb`/`pyusb` script):

- `1D 43 len lo hi sku...` — **GS C**: set SKU + count (to EEPROM).
- `1B 41 <lock>` — **ESC A**: request status; read the 32-byte struct from bulk-IN.

## Changing config without reflashing

The roll state (SKU + count + density) lives in the I2C EEPROM. `GS C` writes new
values; they survive a power cycle. No EEPROM present, or empty? The compiled
defaults in `src/config/store.c` apply (per model: 5XL = SKU `S0904980`, count 220;
550 = SKU `30387`, count 100).

## Host-side printing without a driver (`tools/opsend.py`)

A small sender that speaks the genuine D.mo wire protocol via libusb — no vendor
driver needed.

```sh
pip install pyusb pillow      # pillow only for 'image'
python tools/opsend.py status
python tools/opsend.py testpattern              # alignment / width test
python tools/opsend.py image label.png          # PNG -> raster (scales to head width)
python tools/opsend.py density 12
python tools/opsend.py config --count 500 --sku 30256
python tools/opsend.py version
python tools/opsend.py diag 4                   # GS D diagnostic snapshot
python tools/opsend.py --model OP57 feed 30     # 57 mm instead of default OP104
```

On Windows a WinUSB binding may be needed (e.g. via Zadig) before libusb can claim
the device.

## Tests

`test/test_protocol.c` is a host unit test of the protocol parser (mocked
hardware), including the resumable-after-underflow requirement — now compiled and
**run natively** with the installed host GCC (WinLibs MinGW): **37 checks / 23
scenarios, both models**.

```sh
x86_64-w64-mingw32-gcc -Wall -Wextra -std=c11 [-DMODEL_OP57] \
  -Isrc -o test_protocol.exe test/test_protocol.c src/printer/protocol.c && ./test_protocol.exe
```

`test/test_protocol_wire.py` is a Python transcription harness that checks the
status struct, ESC U record, ESC V reply, and GS D layouts byte-for-byte — no board
needed.
