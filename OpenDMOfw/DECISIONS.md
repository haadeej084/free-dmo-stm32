# DECISIONS — OpenDMOfw

Design choices where the spec was silent or an explicit trade-off was needed.
Per point: choice + reason + how to reverse it. **Ground rule:** every wire value
is sourced from a public document (the tech reference, the driver GPDs, the
decompiled host) and cited in-code; anything not sourceable is listed under
**Assumptions** as "verify on hardware", never a silent guess.

## D1 — Scope: clone the genuine D.mo device

OpenDMOfw **deliberately clones** the genuine LabelWriter 550/5XL: real USB
identity, real wire protocol, real roll-state semantics. The goal is for stock
D.MO Connect to enumerate and drive the device unchanged, so any physical roll
prints. (To make the *stock host* accept the printer, the host must see a genuine
D.mo on the wire.)

## D2 — USB identity: genuine D.mo VID 0x0922 + per-model PID

`src/model.h` / `usb_desc.c` present VID `0x0922`, PID `0x002A` (5XL) /
`0x0028` (550), `DYMO` / `LabelWriter 5XL|550` strings, and an IEEE-1284 device
ID whose `MFG`+`MDL` makes Windows derive the exact driver-model match ID D.mo's
own driver package expects. The serial number is 12 decimal digits from the MCU
UID (unique per chip). To target a different identity, edit `model.h`.

## D3 — Clock: HSI48 + CRS instead of an external crystal

The F072 has an internal 48 MHz oscillator (HSI48) trimmed by CRS against USB
SOF — USB-conformant without a crystal. If the board has an HSE crystal and you
want it, replace `SystemInit()` in `system.c` with an HSE→PLL config at 48 MHz.

## D4 — USB stack: hand-rolled instead of TinyUSB

A minimal own device stack for the F0 USB peripheral fits the memory budget and
keeps the repo dependency-free. TinyUSB would work but adds an external tree and
an integration layer. Downside of an own stack: less proven — see bring-up note D8.

## D5 — Head as a serial shift-register type

The head is a ROHM KF3002-family module with built-in shift registers, latch
and heat drivers — confirmed by the public sibling datasheet (KF3002-GL50A) and
the replacement-head listings that name D.mo's own part (see D16). Host
interface: `CLK` + `DI1`/`DI2` (one data line per half) + `LAT` (Low = THROUGH)
+ `STB1`/`STB2` (heat strobe per half); no MISO. `head.c` bit-bangs these as
plain GPIOs (no SPI peripheral). If the board turns out to use a different head
family, `head.c` + `pins.h` must be rewritten; the rest of the firmware is
unchanged.

## D6 — Print processing in the main loop, not the IRQ

The USB IRQ only fills a ring buffer (`protocol_feed`); `protocol_task()` in the
main loop drives the head and motor. Strobe/step times are long (hundreds of µs–ms)
and must not block the USB IRQ. Flow control: bulk OUT is reopened only when a full
64-byte packet fits again, so no data is lost.

## D7 — Thermal safety always on

Regardless of reference behaviour the firmware limits the head: `thermal_ok()`
must return 1 before each line, and the dwell scales with temperature under a hard
per-line ceiling (`head.c`). Conservative defaults; calibrate on hardware.

## D8 — Hardware bring-up items (NOT verified without a board)

Reasoned but not tested on silicium — verify before production:
1. **PMA access** is 1:1 (STM32F0x2, 1024 B). Confirm with a single EP0 echo.
2. **EPnR toggle / rc_w0 macros** (`usb_core.c`) follow the ST pattern; check
   enumeration with a USB analyzer.
3. **Pinmap** (`pins.h`) — see PINMAP.md, which now carries the complete F072CBT6
   LQFP48 physical pad→GPIO map (Table 13). GPIO alternate-functions verified
   against the F072 datasheet (DocID025004 **Rev 2**, Table 14): I2C1 is **AF2**
   and exists only on PB6/PB7 or PB8/PB9 — we use **PB8/PB9**. The head signals are
   is involved there. SWD = PA13/PA14 (pads 34/37). The board-level pin *routing*
   is still an assumption (no board dump) — measure each pin on hardware.
4. **I2C `TIMINGR`** (`store.c`) is a start value for ~100 kHz @ 48 MHz.
5. **Dwell/density and motor timing** calibrate against print quality + temperature.
   `GS D 0x01`/`0x02` (see D15) confirm the head/motor mechanism fires before you
   tune the absolute values.
6. **`COUNT_RX` encoding** `0x8400` = BLSIZE=1/NUM_BLOCK=1 = 64 B.

## D9 — Multi-model from one codebase (5XL default, 550 option)

Width-parametrised via `src/model.h` + a build define. **Default = 5XL**
(101 mm head, **1248 dots**, 156 B/line); **550** is `make MODEL=OP57` (57 mm
head, **672 dots**, 84 B/line). Head widths come from the tech reference and the
driver GPDs' `MaxPrintableWidth`. Only head
geometry, strobe-segment count, and USB identity (PID/product/MDL) differ; USB
core, protocol, motor, thermics, and config are shared.

## D10 — Software robustness (A1–A7 + host sender)

- **A1** EEPROM page-wise writes (`store.c`) — fixes the page-wrap bug.
- **A2** Resumable, byte-driven raster FSM (`protocol.c`) — a job survives ring
  underflow without corruption.
- **A3** USB endpoint-HALT: `GET_STATUS`(endpoint) + `SET/CLEAR_FEATURE`
  (ENDPOINT_HALT) with DTOG reset (`usb_core.c`).
- **A4** Head strobe applies to **N segments** via a pin array (`head.c`, `pins.h`).
- **A5** Print-density command (genuine `ESC C`/`ESC e`).
- **A6** IWDG watchdog (per-line kick, bounded cool-down wait), LED fault patterns
  (overheat / paper-out), and a **unique serial from the MCU UID**.
- **A7** Host unit test of the parser (`test/test_protocol.c`, mocked hardware),
  compiled + run natively; **37 checks / 23 scenarios, both models**.
- **Host sender** `tools/opsend.py` — speaks the genuine D.mo protocol via libusb
  (test pattern or PNG→raster), byte-matched to the decompiled driver.

## D11 — Genuine wire protocol

`protocol.c` implements the **real D.mo host protocol**, sourced from the official
LabelWriter 550 Series Technical Reference Manual and the decompiled stock driver
(`send_valid_job.py` byte-matches it and printed on a real 550). Key points:

- **Command set** per tech ref p.11–20: `ESC s/L/h/i/T/n/D/G/E/Q/A/C/e/U/V/*/o/@/W`,
  plus `ESC M` (media type, 8 bytes — always sent by the driver; an `S_SKIP` state
  consumes it so a non-zero payload can't desync the parser).
- **Byte order:** two-byte args (`ESC L/n/o`) are **u16 LE**; the `ESC D` header is
  BPP, Align, then Width=lines (u32 LE) and Height=dots (u32 LE); the driver sends
  `BPP=1, Align=0x80`. Status/JobID/Index/Count in the reply are LE.
- **`ESC @`** = "restart print engine" → implemented as a full **pipeline reset**
  (not an MCU reboot), so the host recovers without losing USB configuration.
- **`ESC o`** = set label count (`ESC o <count u16 LE>`): writes the remaining-label
  count and **persists it to EEPROM** (`store_save()`), so a host-set value survives a
  power cycle and is echoed in the status struct (bytes 27–28) and the ESC U record. The
  u16 width matches the driver's counter field. Normal printing decrements the count per
  label and wraps to `MODEL_DEFAULT_COUNT` at zero (fresh-roll behaviour).
- **`ESC L`** = max label length (dots); `0` = die-cut. For die-cut the feed pitch
  comes from the configured/default paper height + a fixed gap (there is no NFC tag
  to read the true pitch).

## D12 — Assumptions (verify on hardware)

- **ESC U CRC:** the 63-byte consumable record carries a CRC16-CCITT over bytes
  8–62 (the SKU + geometry), stored LE at bytes 4–5. The exact polynomial/init is an
  assumption — D.MO Connect appears to tolerate it, but confirm whether it validates
  the CRC.
- **ESC V version strings:** the 16-char hardware string (`LW5XL-REV.K` / `LW550-REV.K`)
  and firmware string (`FWAP01.02.2112`) are per-model assumptions — the tech ref p.20
  fixes the *format* (two 16-char strings + PID LE) but not the values. Kept consistent
  with each model's PID/MDL; the stock host treats them as informational.
- **Die-cut gap / tear offset:** `LABEL_GAP_DOTS` (20) and `TEAR_EXTRA_DOTS` (15)
  are fixed dot counts, not read from the roll (no tag). Tune against a real roll's
  inter-label spacing.
- **Density scale:** `ESC C` duty is treated as 0–200 % (status byte 9 echoes the
  same percentage; a live capture showed 100 = `0x64`). Confirm the exact mapping to
  head dwell if print darkness is off.
- **Paper table:** `paper.h` codes are keyed on the `ESC L` values in the driver
  GPDs; the driver sends `0` for die-cut, so the table is a fallback (feed pitch +
  ESC U mm) rather than the primary path. Raster geometry always comes from `ESC D`.

## D13 — Never overwritable / never brickable

The firmware has **no flash-write path** (it cannot reflash itself), and the board
is set to **RDP level 1** (read-out protected, but still erasable/programmable via
SWD) plus an IWDG watchdog. Together: the host can never overwrite the firmware,
and a bad config/loop resets cleanly instead of bricking — RDP level 2 would be a
one-way door that raises brick risk, so it is deliberately avoided.

## D14 — Roll state is pure config; paper sensor does not gate the host view

The genuine printer derives `MainBayStatus` partly from a physical paper sensor.
Tying our host-facing status to that sensor would let a miswired pin, wrong
threshold, or unusual roll geometry make the host report "no media" (byte10=2) and
refuse to print — defeating the bypass. So by default the firmware reports a valid
roll **regardless of the sensor** (`op_config_t.flags & OP_FLAG_PAPER_FORCE`, set in
the compiled defaults): `MainBayStatus` is always 8 (OK) and the USB port status
never flags paper-out. Clearing the flag re-enables tracking the real sensor for the
status byte. The front-panel LED still reads the sensor directly either way, so a
user gets physical feedback without it changing what the host sees. SKU + label count
are likewise pure EEPROM config (see D12), decremented per printed label.

## D15 — GS D diagnostic backdoor (self-test without a print job)

To verify the physical layer (head / motor / EEPROM / thermistor / GPIOs) on the
real board without a full print job and without a vendor driver, `protocol.c` adds
a `GS D` backdoor (`1D 44 <sub> [arg]`) — see PROTOCOL.md "GS D". Design choices:

- **Marker byte `'D'`:** every reply starts with `'D'`, so it can never be parsed as
  a 32-byte status struct (all zeros in those positions) or any other genuine reply.
  This keeps the backdoor unambiguous on the shared bulk-IN endpoint.
- **No collision with the genuine protocol:** the stock host only ever sends `ESC`
  (`0x1B`) sequences, never `GS` (`0x1D`), so `GS D` is unreachable from a real
  driver and cannot be accidentally triggered by normal printing.
- **Uniform reply shape:** `[0]'D' [1]sub` for every subcommand, so the host tool
  (`opsend.py diag`) decodes one way regardless of which test was run.
- **What it closes:** R2 (head physically fires — `0x01`), R3 (feed motor moves —
  `0x02`), R5/R6 (EEPROM I2C path — `0x03`; live model/thermistor/GPIO/config
  readout — `0x04`). It reports the *raw* values so the absolute calibrations
  (dwell µs, steps/line, pin polarity) can be tuned on the board.

To reverse: delete `diagnose()` and the `S_DIAG_SUB`/`S_DIAG_ARG` states plus the
`GS D` branch in `S_AFTER_GS`; nothing else depends on them.

## D16 — Thermal head identified (ROHM KF3002 family)

Sourced identification:

- **57 mm (550 class):** ROHM **SHEC 3C56-9638 / GK11C308 / KF3002-GK11C**, D.mo
  assembly **PRTA05412** — from replacement-head listings for the LabelWriter
  400/400 Turbo/450 Turbo, which share this 57 mm / 672-dot / 300 dpi head (both
  the 450-series and 550-series tech references state 672 dots @ 300 dpi; the 550
  series is a refresh of the 450 series).
- **101 mm (5XL class):** ROHM **TE3004-TP1W00A** class — the official ROHM
  catalog (SF2024_EN_Thermal_Printheads.pdf) lists exactly **1248 dots @ 300 dpi,
  105.706 mm**, matching the 5XL spec.
- **Family architecture** (public sibling datasheet **KF3002-GL50A**): built-in
  shift registers + latch + heat drivers; host signals CLK, DI1/DI2 (one per
  half), LAT (High=HOLD / Low=THROUGH), STB1/STB2 (heat strobe per half), VH
  (24 V family standard), VDD (3.13–5.25 V), TM (built-in NTC **30 kΩ, B=3950**);
  no MISO (DO1/DO2 are daisy-chain outs). Calibration curves: Fig.3 max energy,
  Fig.4 density vs mJ/dot.

`model.h` sets **both models to 2 strobe segments** (the 57 mm head is also
two-half: 2×336); `head.c` bit-bangs the two-half shift with a sequential per-half
strobe; `thermal.c` documents the sourced NTC spec.

**Still verify on hardware:** exact part marking (GK11C vs a newer revision;
TE3004-TP1W00A vs a custom variant); the thermistor divider R_p / direction
(one 25 °C reading pins it). **Confirmed:** STB is **active-low** (Low fires
the heat driver), DI1/DI2 driven **in parallel**, VH = **24 V**.

## D17 — Board-level facts

- **No public 550 schematic** (FCC RGDLW550 circuit diagram is confidential,
  "metadata only"). The F072 GPIO map for head/sensor/motor is **not published
  anywhere** — it must be measured on the board (continuity from the head flex to
  the LQFP). This is the single biggest remaining unknown.
- **VH = 24 V.** The wall brick is 24 V (550: 1.75 A; Turbo: 2.5 A; 5XL: 3.75 A);
  the head's heat supply is that rail via a P-MOS/load switch (no separate buck).
  Logic VDD = the 3V3 rail.
- **EEPROM:** Rev H/I/K = **BL24C128A** (Belling, 128 kbit, 64 B page, **2-byte
  internal addressing**), address **0x50**, WP pins shorted; Rev E = Atmel
  AT24C01D/02D (8 B page, 1-byte addressing). `store.c` detects the scheme at init
  and uses the matching page size — one firmware works on both revisions.
- **NFC front-end:** SLRC610 @ I2C **0x28** on the same bus — a different address,
  ignored (tag emulation is out of scope).
- **Feed motor driver:** not named in public teardowns; likely a small dual-H-bridge
  (TB6612/MP6500 class) or a discrete 4-transistor H-bridge on 24 V, driving the four
  phases directly (`MOTOR_DRIVE_4PHASE`; STEP/DIR kept as fallback). µsteps per dot
  line are not in the TRM — count them by scoping the phase pins during one ESC D line.
- **STB polarity = active-low** (Low = heat driver on), from the ROHM KF3002 timing
  chart; `head.c` fires low. DI1/DI2 are driven in parallel (two shift-register banks,
  one CLK).
- **NTC curve:** 30 kΩ @ 25 °C, B=3950; R(T) = 30000·exp(3950·(1/T − 1/298.15))
  (25 °C ≈ 30 kΩ, 45 °C ≈ 13.4 kΩ, 60 °C ≈ 7.8 kΩ). One 25 °C ADC reading pins the
  divider R_p.
- **Feed contract:** one raster line = 1/300 inch = **0.08467 mm**; µsteps/line =
  (N_steps/rev · microstep · gear) / (π · D_roller_mm · 11.811). The drive-train
  constants are unpublished — count phase pulses per ESC D line.
- **Board part IDs** (from a rev E board photo, not bundled): **STM32F072CBT6**
  (LQFP48, 128 K / 16 K), **24C02A** EEPROM (256 B, 1-byte), **SLRC610** NFC front-end
  — confirming the two-EEPROM model above.

**`store.c` EEPROM detection.** A round-trip probe cannot distinguish 1-byte from
2-byte addressing (a write+read is self-consistent under either scheme), so detection
uses the **config magic field as the external reference**: it reads the config under
each width and keeps the one whose magic matches; on first boot (no valid config) it
defaults to 2-byte (current production) and persists immediately, pinning the width
from boot 1. One firmware works on both EEPROM revisions.
