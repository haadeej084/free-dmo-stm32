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
| **OP57** (default) | `make` | 672 dots @ 300 dpi (56.9 mm) | `build/OP57/opendmo-OP57.bin` |
| OP104 | `make MODEL=OP104` | 1248 dots @ 300 dpi (105.7 mm) | `build/OP104/opendmo-OP104.bin` |

OP57 is the LabelWriter 550, the board with an STM32F072. OP104 is the 5XL's
4"/300 dpi geometry (the name refers to the 104 mm printable label width, not
the head); a genuine 5XL carries an STM32F407VET6, so OP104 is built and tested
for a future port but does not run on a 5XL board (DECISIONS D25).

## Building

```sh
make                 # OP57 (default) -> build/OP57/opendmo-OP57.bin + size
make MODEL=OP104     # 4" geometry    -> build/OP104/opendmo-OP104.bin
make clean MODEL=OP104
```

Equivalent without `make`:

```sh
./build.sh           # OP57 (default) -> the LabelWriter 550
./build.sh OP57      # 57 mm variant
./build.sh all       # both
```

The build uses `--specs=nano.specs` (newlib-nano) and `-Os`; the memory budget is
**64 KB** flash / 16 KB SRAM — the linker script declares 64 K deliberately, so
that an image built here also fits an STM32F072C8 if that is what the board
actually carries (the one legible 550 marking is ambiguous, see below).
`make size` shows the usage.

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
st-flash write build/OP57/opendmo-OP57.bin 0x08000000   # the 550 image
# NOTE: these commands used to name the OP104 image. Nothing can run it:
# `make flash` never produces it, and a genuine 5XL carries an STM32F407,
# not the F072 this image is linked for.
```

Alternative with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f0x.cfg \
  -c "program build/OP57/opendmo-OP57.bin 0x08000000 verify reset exit"
```

> **RDP Level 2 on stock printers:** SWD and the system bootloader are off.
> Lowering RDP mass-erases flash. Flash this image only onto an F072 that is
> already programmable. The firmware does **not** write option bytes (it will
> not set RDP1 or RDP2).
>
> Linker script: `linker/stm32f072xb.ld` (declares 64 KB flash / 16 KB RAM: the
> image fits both the F072C8 and the F072CB, and the one legible 550 marking
> does not say which is fitted).

## Updating over USB after the first flash (DFU)

The first image has to go in over SWD. After that, no probe is needed:

```sh
python tools/opsend.py dfu                                   # printer reboots as 0483:df11
dfu-util -a 0 -s 0x08000000:leave -D build/OP57/opendmo-OP57.bin
```

`GS D 0x09 'D' 'F' 'U'` sets a flag in `.noinit` RAM and resets; the reset
handler sees the flag before anything else runs and jumps to ST's boot loader
at `0x1FFFC800` (AN2606). The boot loader clocks USB from HSI48 + CRS, so it
works without the crystal. `:leave` jumps back to the new image when done.
On Windows, `dfu-util` needs the WinUSB driver for `0483:df11` (Zadig), or use
STM32CubeProgrammer. At RDP Level 1 the boot loader refuses to read or write
flash until a mass erase; at RDP 0 (a chip you flashed yourself) it just works.

## Testing enumeration (no head connected)

After flashing, with only USB connected:

```sh
lsusb | grep 0922:0028                     # OP57 / 550 (default)
lsusb | grep 0922:002a                     # OP104 test build
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
python tools/opsend.py --model OP104 feed 30    # 4" test build instead of the default OP57
```

On Windows a WinUSB binding may be needed (e.g. via Zadig) before libusb can claim
the device.

## Tests

`make test` runs the host checks (no board):

```sh
make test
```

- `test/test_protocol.c` — the real parser with mocked hardware, both models
  (179 checks). This is the regression test: it links and runs
  `src/printer/protocol.c`. Needs a host `cc`/`gcc` on PATH.
- `test/test_usb.c` — the real USB stack (`usb_core.c`, `usb_desc.c`,
  `usb_printer.c`) against a register-level model of the STM32F0 USB
  peripheral, with a scripted host: enumeration, descriptors, bulk transfers,
  printer-class requests, HALT/STALL handling (127 checks per model).
- `test/test_e2e.c` — the USB stack, printer class and protocol parser together
  against the same peripheral model: a 120-line job pushed as 64-byte bulk
  packets with the main loop running only on NAK (the ring buffer fills and
  pauses the endpoint), status/SKU/version replies through bulk IN, SOFT_RESET
  mid-raster, and a refused firmware update (29 checks per model).
- `test/test_protocol_wire.py` — a hand transcription of the reply generators,
  checked against the live capture and the decompiled driver structs. It does
  **not** execute the C; keep it in sync when `protocol.c` changes.
- `test/test_thermal.c` — `thermal.c` + `head.c`: the NTC curve, the dwell law
  and the energy ceiling, built twice (the second build arms `HEAD_SAG_FULL_US`;
  39 and 41 checks per model).
- `test/test_motor.c` — `motor.c`: phase ORDER, break-before-make, exactly one
  step per line, idle release (12 checks per model).
- `test/test_system.c` — the real `sys_pin_toggle()` and its hot-pin guard
  (18 checks per model).
- `test/test_store.c` — `store.c` against a register-level I2C EEPROM model
  (`test/i2c_eeprom_model.h`): ACK/NAK per byte, the stale-NACKF case, bus
  recovery, and the checksummed record (27 scenarios per model).

Two further checks, both also run in CI:

```sh
make stack [MODEL=OP104]                  # worst-case stack from GCC's call graph vs 2048 B
make renode [MODEL=OP104] RENODE=/path/to/renode
```

`make renode` runs the built image in the Renode emulator (1.17, portable
tarball): `test/renode/smoke.py` checks boot, fault-free main loop, SysTick and
the LED patterns; `test/renode/head_shift.py` checks the exact bit stream the
head receives and prints the per-line shift cost; `test/renode/eeprom.py` runs
the config store against emulated 16 KB and 256 B EEPROMs; `test/renode/dfu.py`
checks the USB DFU request and the hand-over to the boot loader;
`test/renode/fault.py` forces HardFault, NMI, an unused vector and a fault
before `SystemInit()` and checks the safe state (VH off, strobes idle).

Also directly:

```sh
cc -Wall -Wextra -std=c11 [-DMODEL_OP104] \
  -Isrc -o test_protocol test/test_protocol.c src/printer/protocol.c && ./test_protocol
```

The PC-side patcher has its own suite (Windows, .NET SDK) — it builds a synthetic
assembly and runs the real patcher against it, so no `DYMO.LabelAPI.dll` is
needed:

```sh
cd pc-patch/test && dotnet run -c Release
```
