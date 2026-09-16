# OpenDMOfw

> ### ⚠️ Prototype / concept firmware — needs hardware fieldwork
>
> This is a **prototype / concept**, not a finished product. The **software layer is
> complete and verified** (both models build clean; every software-testable path is
> tested), but the firmware **has not yet been run on a real board**. Before it prints,
> someone with a genuine LabelWriter 550 must do the **hardware fieldwork** in
> [`FIELDWORK.md`](FIELDWORK.md) — which is down to **seven** items: GPIO routing,
> the motor drive train, the thermistor divider resistor, the top-of-form sensor,
> the VH enable pin, the half-2 dot order, and host acceptance.
>
> **You can help in five minutes without opening anything.** If you own a working
> LabelWriter and a genuine roll, three USB captures (`FIELDWORK.md` section 1b)
> retire three of the last open protocol assumptions.
>
> **The author's specialism is embedded software, not hardware.** The firmware is
> built to be robust and self-diagnosing, so a short session with a multimeter — not
> much work — closes the remaining gap. If you have a board and the tools,
> [`FIELDWORK.md`](FIELDWORK.md) lists exactly what to measure — reach out at
> **opendymofw@secret.fyi**.
>
> **Stock 550 MCUs are reported to ship at RDP Level 2** (see DECISIONS D13 for
> the evidence). SWD debug and the system bootloader
> are then disabled; `st-flash write` will not take until RDP has already been lowered
> (that mass-erases flash). This image is for an F072 you are already allowed to
> program (replacement chip, or a chip whose RDP was lowered). A factory printer
> still uses the Bluepill I2C tag-emulator path until then.

> **Which printers.** The image runs on the **LabelWriter 550**, whose mainboard
> carries an **STM32F072** (48-pin). The **5XL and 550 Turbo** use an
> **STM32F407VET6** with an Ethernet PHY instead — read on several boards
> (DECISIONS D25) — so this image does **not** run on them. The 4" build below
> keeps the 5XL geometry, identity and paper table ready for an F407 port.

Firmware that runs **in place on a genuine D.mo LabelWriter 550 mainboard**
(STM32F072, flashed over SWD once the chip is writable) and makes the printer **print on any
roll**, by defeating the three layers of D.mo's roll DRM. It is USB-only. One codebase
builds two head geometries:

| Model | Build | USB PID | Print head | Head width | Resolution | Board |
|-------|-------|---------|-----------|------------|------------|-------|
| LabelWriter **550**            | `make` (default) | `0x0028` | 672 dots (84 B)   | 56.9 mm  | 300 dpi | STM32F072 — runs |
| LabelWriter **5XL** (4" class) | `make MODEL=OP104` | `0x002A` | 1248 dots (156 B) | 105.7 mm | 300 dpi | STM32F407 on the real board — needs a port |

Only the **dot count** is a spec value — the tech reference states 672 and 1248
dots at 300 dpi — and the mm width follows from it. (DYMO's own prose calls the
5XL head "101 mm wide"; 1248 dots at 300 dpi is 105.7 mm, and the ROHM catalog
lists the matching head at 105.706 mm, so the 101 mm figure is a nominal media
width rather than the dot row. The firmware only ever uses the dot count.) The
build name `OP104` refers to the 104 mm printable width of the biggest 5XL roll.

Both present themselves as the real device — VID `0x0922`, per-model PID, `DYMO` /
`LabelWriter 5XL|550` strings, and an IEEE-1284 device ID that makes Windows derive the
exact hardware ID D.mo's own driver package expects. A unique serial number is
taken from the MCU's 96-bit UID.

## What the D.mo DRM actually is

D.mo does not sell cheap rolls; it sells the *lock*. Printing is gated on a genuine D.mo
roll, enforced at **three independent layers**. Remove any one and the printer still
refuses — so a working bypass must defeat all three:

1. **Printer side (MCU + NFC tag).** Each roll carries an embedded NFC tag with the
   consumable record — SKU, geometry, remaining label count. The printer's own firmware
   reads the tag and reports a *roll state* to the host. No tag, or a tag whose SKU it
   doesn't recognise, yields an error or a **counterfeit** bay status
   (`MainBayStatus = 10`, an enumerated value in the manual's own status table),
   and the engine will not print. The tech reference says it in as many words:
   *"The label length is determined by the SKU data found on the NFC Tag"* and
   *"Only authentic Dymo labels with a valid NFC Tag can be used for printing."*

2. **PC side (D.MO Connect / `DYMO.LabelAPI.dll`).** Even a printer that reports "roll
   present" is not enough. The host software validates the reported SKU against an
   embedded catalog, checks `eRollValidity`, and gates printing on a *valid* roll. An
   unknown/empty SKU fails this check on the PC.

3. **Counter / counterfeit.** The tag's label count decrements with each label; when it
   runs out — or if the tag is a known fake — the roll is rejected. This is what makes
   third-party "refill" rolls stop working.

Practically: the DRM is an **anti-consumer lockout** that forces you to buy D.mo rolls
(and to let D.mo's software decide whether a roll is "real"). It is not
a security boundary protecting data — it is **anti-consumer DRM packed as "consumer
convenience"**: useless automatic label recognition.

## How OpenDMOfw defeats it

| Layer | What OpenDMOfw does |
|-------|---------------------|
| Printer side | The firmware **clones the genuine D.mo USB identity** and speaks the exact published wire protocol, then reports a **valid, configurable roll state directly** — SKU + count from on-board EEPROM, no NFC tag read. `MainBayStatus` is always reported OK (`8`), never counterfeit. |
| PC side | The companion tool **`pc-patch/`** patches `DYMO.LabelAPI.dll` (IL injection + catalog/exclusion fixes) so an empty or unknown SKU still resolves to a *valid* roll in D.MO Connect. |
| Counter | SKU + label count live in the on-board EEPROM, always reported valid, and **decrement per printed label** like a real roll — so the host sees normal wear instead of an exhausted or fake tag. |

The result: plug in any physical roll (genuine D.mo, third-party, or blank die-cut
stock), and the printer accepts it, shows a plausible SKU/count in D.MO Connect, and
prints end-to-end.

## Layout

```
src/                  firmware (C, hand-rolled USB FS device stack)
  model.h             per-model geometry + USB identity (OP57=550 default, OP104=4" head)
  printer/protocol.c  genuine D.mo wire-protocol parser (see PROTOCOL.md)
  printer/paper.h     paper table from the real driver GPDs (feed pitch + ESC U mm)
  usb/usb_desc.c      USB descriptors: VID/PID/strings/IEEE-1284 device ID
  config/store.c      I2C EEPROM config store (SKU + count), with compiled defaults
tools/opsend.py       driver-less host sender (libusb) that speaks the real protocol
pc-patch/             PC-side DYMO.LabelAPI.dll patcher (.NET tray app, dmo.ico icon)
test/test_protocol.c  host unit test of the parser (mocked hardware)
test/test_usb.c       host unit test of the USB stack (register-level peripheral model)
test/test_e2e.c       USB stack + parser end to end (a full job through 64-byte packets)
test/renode/          the real image in the Renode emulator (boot, LED, head shift, EEPROM, DFU)
tools/stack_depth.py  worst-case stack from GCC call-graph info (make stack)
```

## Build

Two equivalent drivers produce `build/<MODEL>/opendmo-<MODEL>.{elf,bin,map}`:

```sh
make                 # OP57 / 550 (default)
make MODEL=OP104     # OP104 / 4" head geometry
```

On a host without `make`, use the equivalent shell driver:

```sh
./build.sh           # OP57 (default)
./build.sh OP104     # OP104
./build.sh all       # both
```

See `BUILD.md` for the exact flags, and `PINMAP.md` for the assumed pin map (verify
against a board before flashing).

## Verification criteria

- D.MO Connect enumerates the device as a genuine LabelWriter 550.
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
*LabelWriter 550 Series Printers Technical Reference Manual* (D.mo's published doc),
the genuine driver GPDs, and the decompiled stock host — and cited in-code. Anything not
sourceable is listed as an assumption in `DECISIONS.md` ("verify on hardware"), never a
silent guess.

## Fieldwork / contributing

The firmware is complete; the only remaining work is **hardware verification +
calibration** on a real board (GPIO routing, motor µsteps, thermistor divider, STB
polarity, VH enable, half-2 dot order).

[`FIELDWORK.md`](FIELDWORK.md) is written to be worked through with a multimeter
in hand: safety rules, the continuity table, a staged bring-up that never powers
the head before it has been measured, each measurement with its method, its
expected value and the exact constant to patch, a symptom→cause troubleshooting
map, and a **fill-in report template**. The continuity work alone is one to two
hours and is the single most valuable contribution.

**No code required** — fill in the template and mail it to
`opendymofw@secret.fyi`. A PR updating `src/pins.h` is a welcome bonus.

## License

Covered by the repository's [`LICENSE`](../LICENSE) (GPL-3.0). The PC-side
patcher in `pc-patch/` depends on [dnlib](https://github.com/0xd4d/dnlib) (MIT).
