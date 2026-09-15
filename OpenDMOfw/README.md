# OpenDMO-FW

> ### ⚠️ Prototype / concept firmware — needs hardware fieldwork
>
> This is a **prototype / concept**, not a finished product. The **software layer is
> complete and verified** (both models build clean; every software-testable path is
> tested), but the firmware **has not yet been run on a real board**. Before it prints,
> someone with a genuine LabelWriter 550/5XL must do the **hardware fieldwork** in
> [`FIELDWORK.md`](FIELDWORK.md) — confirm the GPIO routing, motor steps/line,
> thermistor divider, and STB polarity on the physical board.
>
> **The author's specialism is embedded software, not hardware.** The firmware is
> built to be robust and self-diagnosing, so a short session with a multimeter — not
> much work — closes the remaining gap. If you have a board and the tools,
> [`FIELDWORK.md`](FIELDWORK.md) lists exactly what to measure — reach out at
> **opendymofw@secret.fyi**.

Firmware that runs **in place on a genuine Dymo LabelWriter 550 / 5XL mainboard**
(STM32F072, flashed over SWD — no new hardware) and makes the printer **print on any
roll**, by defeating the three layers of Dymo's roll DRM. It is USB-only (the network
"LabelWriter Print Server" coprocessor is out of scope). One codebase builds both models:

| Model | Build | USB PID | Print head | Dots / line |
|-------|-------|---------|-----------|-------------|
| LabelWriter **5XL** (101 mm) | `make` (default) | `0x002A` | 1248 dots (156 B) | 300 dpi |
| LabelWriter **550** (57 mm)   | `make MODEL=OP57` | `0x0028` | 672 dots (84 B)   | 300 dpi |

Both present themselves as the real device — VID `0x0922`, per-model PID, `DYMO` /
`LabelWriter 5XL|550` strings, and an IEEE-1284 device ID that makes Windows derive the
exact driver-model match ID Dymo's own driver package expects. A unique serial number is
taken from the MCU's 96-bit UID.

## What the Dymo DRM actually is

Dymo does not sell cheap rolls; it sells the *lock*. Printing is gated on a genuine Dymo
roll, enforced at **three independent layers**. Remove any one and the printer still
refuses — so a working bypass must defeat all three:

1. **Printer side (MCU + NFC tag).** Each roll carries an embedded NFC tag with the
   consumable record — SKU, geometry, remaining label count. The printer's own firmware
   reads the tag and reports a *roll state* to the host. No tag, or a tag whose SKU it
   doesn't recognise, yields an error or a **counterfeit** bay status
   (`MainBayStatus = 10`), and the engine will not print. The tech reference is explicit:
   *"the label length is determined by the SKU data found on the NFC Tag."*

2. **PC side (DYMO Connect / `DYMO.LabelAPI.dll`).** Even a printer that reports "roll
   present" is not enough. The host software validates the reported SKU against an
   embedded catalog, checks `eRollValidity`, and gates printing on a *valid* roll. An
   unknown/empty SKU fails this check on the PC.

3. **Counter / counterfeit.** The tag's label count decrements with each label; when it
   runs out — or if the tag is a known fake — the roll is rejected. This is what makes
   third-party "refill" rolls stop working.

Practically: the DRM is an **anti-consumer lockout** that forces you to buy Dymo rolls
(and, on some models, to let Dymo's software decide whether a roll is "real"). It is not
a security boundary protecting data — it is **anti-consumer DRM packed as "consumer
convenience"**: useless automatic label recognition.

## How OpenDMO-FW defeats it

| Layer | What OpenDMO-FW does |
|-------|---------------------|
| Printer side | The firmware **clones the genuine Dymo USB identity** and speaks the exact published wire protocol, then reports a **valid, configurable roll state directly** — SKU + count from on-board EEPROM, no NFC tag read. `MainBayStatus` is always reported OK (`8`), never counterfeit. |
| PC side | The companion tool **`pc-patch/`** patches `DYMO.LabelAPI.dll` (IL injection + catalog/exclusion fixes) so an empty or unknown SKU still resolves to a *valid* roll in DYMO Connect. |
| Counter | SKU + label count live in the on-board EEPROM, always reported valid, and **decrement per printed label** like a real roll — so the host sees normal wear instead of an exhausted or fake tag. |

The result: plug in any physical roll (genuine Dymo, third-party, or blank die-cut
stock), and the printer accepts it, shows a plausible SKU/count in DYMO Connect, and
prints end-to-end.

## Layout

```
src/                  firmware (C, hand-rolled USB FS device stack)
  model.h             per-model geometry + USB identity (OP104=5XL default, OP57=550)
  printer/protocol.c  genuine Dymo wire-protocol parser (see PROTOCOL.md)
  printer/paper.h     paper table from the real driver GPDs (feed pitch + ESC U mm)
  usb/usb_desc.c      USB descriptors: VID/PID/strings/IEEE-1284 device ID
  config/store.c      I2C EEPROM config store (SKU + count), with compiled defaults
tools/opsend.py       driver-less host sender (libusb) that speaks the real protocol
pc-patch/             PC-side DYMO.LabelAPI.dll patcher (.NET tray app, dymo.ico icon)
test/test_protocol.c  host unit test of the parser (mocked hardware)
```

## Build

Two equivalent drivers produce `build/<MODEL>/opendmo-<MODEL>.{elf,bin,map}`:

```sh
make                 # OP104 / 5XL (default)
make MODEL=OP57      # OP57 / 550
```

On a host without `make`, use the equivalent shell driver:

```sh
./build.sh           # OP104 (default)
./build.sh OP57      # OP57
./build.sh all       # both
```

See `BUILD.md` for the exact flags, and `PINMAP.md` for the assumed pin map (verify
against a board before flashing).

## Verification criteria

- DYMO Connect enumerates the device as a genuine LabelWriter 550/5XL.
- It shows a **valid roll** with the configured SKU and count (no "empty"/"JOKER").
- A label prints end-to-end from that software — correct size, content, feed.
- Changing the configured SKU/count (backdoor `GS C` or EEPROM) changes what the host
  displays, without reflashing.
- The `GS D` diagnostic backdoor fires the head / steps the motor / reads the EEPROM and
  GPIOs with no vendor driver, for on-board bring-up.
- Any physical roll in the path prints; there is no tag or authentication step.
- Sustained printing (100+ labels) with no thermal runaway or protocol drift.

## Ground rule

Every wire value in the firmware is sourced from a public document — the official
*LabelWriter 550 Series Printers Technical Reference Manual* (`LW550_TECHREF.txt`), the
genuine driver GPDs, and the decompiled stock host — and cited in-code. Anything not
sourceable is listed as an assumption in `DECISIONS.md` ("verify on hardware"), never a
silent guess.

## Fieldwork / contributing

The firmware is complete; the only remaining work is **hardware verification +
calibration** on a real board (GPIO routing, motor µsteps, thermistor divider, STB
polarity). If you have a LabelWriter 550/5XL and a multimeter, [`FIELDWORK.md`](FIELDWORK.md)
lists exactly what to measure — then **report your findings by email to
`opendymofw@secret.fyi`** (no code required).
