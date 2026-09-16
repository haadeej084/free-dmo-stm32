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

| Model | Command | Head | Output |
|-------|---------|------|--------|
| **OP104** (default) | `make` | 1248 dots @ 300 dpi (105.7 mm) | `build/OP104/opendmo-OP104.bin` |
| OP57 | `make MODEL=OP57` | 672 dots @ 300 dpi (56.9 mm) | `build/OP57/opendmo-OP57.bin` |

OP104 matches the specs of the common 4"/300 dpi shipping-label class; the name
refers to the 104 mm printable label width, not to the head width.

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

Both drivers stamp a build identifier from `git describe --always --dirty
--abbrev=8` into the image; `GS D 0x05` (`opsend.py diag 5`) reports it, so a
fieldwork report can name the exact image it was measured on. Override with
`make BUILD_ID=whatever`; outside a git checkout it becomes `dev`.

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
> density of the F072CB; `x8` would be the 64 KB part).

## Testing enumeration (no head connected)

After flashing, with only USB connected:

```sh
lsusb | grep 0922:002a                     # OP104 / 5XL (default)
lsusb | grep 0922:0028                     # OP57 / 550
# device ID (Linux, usblp): the printer returns the IEEE-1284 string via GET_DEVICE_ID
```

The genuine D.mo identity is VID `0x0922`; per-model PID `0x002A` (5XL) /
`0x0028` (550). Windows derives the hardware ID from the IEEE-1284
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
python tools/opsend.py diag 5                   # firmware build id
python tools/opsend.py --model OP57 feed 30     # 57 mm instead of default OP104
```

On Windows a WinUSB binding may be needed (e.g. via Zadig) before libusb can claim
the device.

## Tests

`make test` runs the host checks (no board):

```sh
make test
```

- `test/test_protocol.c` — the real parser with mocked hardware, both models
  (117 checks / 52 scenarios). This is the regression test: it links and runs
  `src/printer/protocol.c`. Needs a host `cc`/`gcc` on PATH.
- `test/test_protocol_wire.py` — a hand transcription of the reply generators,
  checked against the live capture and the decompiled driver structs. It does
  **not** execute the C; keep it in sync when `protocol.c` changes.

Also directly:

```sh
cc -Wall -Wextra -std=c11 [-DMODEL_OP57] \
  -Isrc -o test_protocol test/test_protocol.c src/printer/protocol.c && ./test_protocol
```

The PC-side patcher has its own suite (Windows, .NET SDK) — it builds a synthetic
assembly and runs the real patcher against it, so no `DYMO.LabelAPI.dll` is
needed:

```sh
cd pc-patch/test && dotnet run -c Release
```
