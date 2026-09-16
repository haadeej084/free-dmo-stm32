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
SOF — USB-conformant without a crystal. `SystemInit()` sets `CRS_CFGR.SYNCSRC`
to USB SOF explicitly (not only the reset default).

**The board does have a crystal.** A close-up of a Rev K mainboard shows an
HC-49 can marked **`AXC12.00-115`** at **Y1**, immediately beside the 48-pin MCU,
with its load capacitors — a **12 MHz** HSE. 12 MHz × PLL4 = exactly 48 MHz, a
better USB clock than a trimmed RC, so `system.c` now carries that path too:
build with `-DOPENDMO_CLOCK_HSE12=1`.

It stays **opt-in, not the default**, for one reason: bring-up step A happens on
a bare F072 that may have no crystal at all, and waiting on `HSERDY` there would
hang before USB ever comes up. HSI48 needs nothing from the board.

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

The limit is **not a guess**. The LabelWriter 450 Series Technical Reference
(p.7) states the genuine rule verbatim:

> "In order to protect the print head from excessive heat, the control
> electronics halt printing if the print head temperature exceeds **70 °C**.
> Printing resumes when the print head cools to **56 °C**."

So `thermal.c` implements a **latched limit with hysteresis** at exactly those
two temperatures, not a single threshold. The same page describes the dwell
model — "the control electronics measure the print voltage and the head
temperature before each print cycle, and then calculate the required print
[energy]" — which is what `thermal_dwell_scale()` does for the temperature half.
The voltage half needs a divider on the 24 V rail that is not on our pin map yet.

Only the **divider topology** (R_p, pull-up vs pull-down) is still unknown, and
FIELDWORK measurement 3 turns that into reading two numbers off a table.

Two paths, two policies — deliberately:

- **Print path** (`protocol.c::emit_line`): waits up to ~1 s for the head to
  come back under the limit, then prints anyway. The dwell is already thermally
  reduced and capped, so this is bounded energy, and dropping raster lines from
  a customer's label would be worse than a slightly light line.
- **`GS D 0x01` diagnostic**: all dots on at maximum dwell — the hottest thing
  the firmware can do. Here it does **not** fall through: it waits, and if the
  head is still over the limit it stops and reports how many lines actually
  fired. A bring-up self-test must never be the thing that cooks the head.

## D8 — Hardware bring-up items (NOT verified without a board)

Reasoned but not tested on silicium — verify before production:
1. **PMA access** is 1:1 (STM32F0x2, 1024 B). Confirm with a single EP0 echo.
2. **EPnR STAT/CTR** (`usb_core.c`) use TinyUSB's keep-mask + XOR-STAT (STAT/DTOG
   are toggle bits; CTR is rc_w0). Confirm enumeration with a USB analyzer.
3. **Pinmap** (`pins.h`) — see PINMAP.md, which now carries the complete 48-pin
   physical pad→GPIO map (Table 13, whose `LQFP48/UFQFPN48` column covers both
   packages). GPIO alternate-functions verified
   against the F072 datasheet (DocID025004 **Rev 2**, Table 14): I2C1 is **AF2**
   and exists only on PB6/PB7 or PB8/PB9 — we use **PB8/PB9**. USB DM/DP = PA11/PA12
   AF2. SWD = PA13/PA14 (pads 34/37). LED/button are PA2/PA3 (PC6/PC7 are not
   bonded on LQFP48). VH enable is PA8 (assumed). The board-level pin *routing*
   is still an assumption (no board dump) — measure each pin on hardware.
4. **I2C `TIMINGR`** (`store.c`) is a start value for ~100 kHz @ 48 MHz.
5. **Dwell/density and motor timing** calibrate against print quality + temperature.
   `GS D 0x01`/`0x02` (see D15) confirm the head/motor mechanism fires before you
   tune the absolute values.
6. **`COUNT_RX` encoding** `0x8400` = BLSIZE=1/NUM_BLOCK=1 = 64 B.

## D9 — Multi-model from one codebase (5XL default, 550 option)

Width-parametrised via `src/model.h` + a build define. **Default = 5XL**
(**1248 dots**, 156 B/line, 105.7 mm at 300 dpi); **550** is `make MODEL=OP57`
(**672 dots**, 84 B/line, 56.9 mm). Head widths come from the tech reference and
the driver GPDs' `MaxPrintableWidth`; the mm figures are derived from the dot
count at 300 dpi, not quoted independently. Only head
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
  compiled + run natively; **99 checks / 46 scenarios, both models**. Note that
  `test/test_protocol_wire.py` is a *hand transcription* of the reply generators
  and checks that transcription against the capture and the driver structs — it
  does not execute `protocol.c`. `test_protocol.c` is the executable regression
  test on the real parser.
- **Host sender** `tools/opsend.py` — speaks the genuine D.mo protocol via libusb
  (test pattern or PNG→raster), byte-matched to the decompiled driver.

## D11 — Genuine wire protocol

`protocol.c` implements the **real D.mo host protocol**, sourced from the official
LabelWriter 550 Series Technical Reference Manual and the decompiled stock driver
(`send_valid_job.py` byte-matches it and printed on a real 550). Key points:

- **Command set** per tech ref p.11–20: `ESC s/L/h/i/T/n/D/G/E/Q/A/C/e/U/V/$/o/q/@/W`,
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

## D12 — Assumptions (what is genuinely still unknown)

A research pass over DYMO's own published manuals (D21) moved several entries
out of this list. What is left:

- **ESC U CRC:** the 63-byte record carries a CRC16-CCITT over bytes 8–62,
  stored LE at bytes 4–5. The tech ref's own row is self-contradictory
  ("Byte 7…Byte 4 / b15…b0 / CRC" — four bytes, sixteen bits), so both the
  position and the polynomial/init are assumptions. **One captured `ESC U` reply
  from any genuine printer with a real roll settles all of it** — no teardown,
  just a USB cable (FIELDWORK section 1).
- **ESC V version *values*:** the *format* is now fully sourced (p.20: `FWAP`,
  major, minor, `MMYY`, four chars each) and implemented. Only the numbers we
  put in those fields are ours. Same one-command capture settles them.
- **Die-cut gap / tear offset:** `LABEL_GAP_DOTS` (20) and `TEAR_EXTRA_DOTS` (15)
  are fixed dot counts, not read from the roll. Tune against a real roll.
- **Density-to-dwell mapping:** `ESC C` duty is 0–200 % (sourced, p.16) and the
  status echoes it; how that percentage maps to microseconds of strobe depends
  on the head's energy curve and the rail voltage. Calibrate.
- **Paper table:** `paper.h` codes are keyed on the `ESC L` values in the driver
  GPDs. The driver sends `0` for die-cut, so the table is a fallback (feed pitch
  + ESC U mm) rather than the primary path. Raster geometry always comes from
  `ESC D`.

Resolved since the previous revision, with sources, so nobody re-measures them:
thermal limits (70 °C / 56 °C, LW450 p.7), the ESC V field structure (p.20),
`ESC o` argument width (p.20), USB self-powered + 4 mA (LW450 `lsusb -v`),
VH = 24 V (550 TRM p.9 adapter table), MainBayStatus and PrintHeadVoltage
enumerations (p.14–15), and the per-line time budget (rated labels/min).

## D13 — Never overwritable / never brickable

The firmware has **no flash-write path** (it cannot reflash itself) and an IWDG
watchdog, so a bad config/loop resets instead of hanging forever. It does **not**
program option bytes: RDP is left as the programmer set it. Do **not** raise the
chip to RDP level 2 from this image (one-way door, brick risk on the next
reflash). Stock 550/5XL parts already ship at RDP2 — this firmware cannot be
installed over SWD until that is lowered (mass-erase).

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
- **4" / 1248-dot (5XL class):** ROHM **TE3004-TP1W00A** class — the official ROHM
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
- **Board part IDs** (from a rev E board photo, not bundled): **STM32F072CB**
  (48-pin, 128 K / 16 K; LQFP48 on that revision), **24C02A** EEPROM (256 B,
  1-byte), **SLRC610** NFC front-end — confirming the two-EEPROM model above.

**`store.c` EEPROM detection.** A round-trip probe cannot distinguish 1-byte from
2-byte addressing (a write+read is self-consistent under either scheme), so detection
uses the **config magic field** (`ODM1`): it tries 2-byte at offset `0x100`, then
legacy 2-byte at `0`, then 1-byte at `0`. On first boot it persists with 2-byte and
**verifies the magic**; if that fails it retries 1-byte; if both fail, config stays
in RAM (WP high or no EEPROM). Scratch self-test is `GS D 0x03` only.

**Collateral write on a Rev E part.** On first boot the 2-byte attempt writes at
offset `0x0100`, which a 1-byte 24C02 reads as word address `0x01`; the 32-byte
block then wraps inside its 8-byte page and overwrites bytes `0x00`–`0x07`. That
is accepted: the 1-byte fallback stores the config at offset 0 anyway, so those
bytes are ours in either case. A non-destructive probe is not possible — a
write+read round-trip is self-consistent under both addressing schemes.

**EEPROM wear is not a concern.** The config block is rewritten once per printed
label at a fixed address. At the part's rated 1 M write cycles that is ~1 M
labels, or roughly 4500 full 220-label rolls — well past the life of the printer,
so no wear levelling or write batching is warranted. Persisting every label is
worth more: the remaining count survives an unplanned power cut mid-roll.

## D18 — Correctness fixes that changed observable behaviour

These replaced earlier behaviour that was wrong rather than merely undecided;
listed so a reader of an older build knows what moved.

- **Factory reset is `ESC $` (`1B 24`), with `1B 2A` as an alias.** The byte in
  the tech ref and in `tools/opsend.py` is `0x24`; the firmware previously only
  matched `'*'` (`0x2A`), so the host tool's `factory-reset` fell into the
  unknown-command branch and additionally swallowed the following byte. Both are
  now accepted. Regression test: `test_protocol.c` scenario 24.
- **Bulk-OUT is re-armed in `protocol_reset()`.** Throttling sets `s_rx_paused`
  and leaves the endpoint NAKing; the un-pause only fires when a byte is read.
  After a USB SOFT_RESET on a backed-up ring the ring went empty, so no byte was
  ever read again and the endpoint stayed shut with the watchdog happily kicked.
  `protocol_reset()` now clears the flag and reopens the endpoint.
- **Bulk-IN waits for the previous reply.** There is one PMA TX buffer per
  endpoint, so two replies generated from one 64-byte packet (`ESC A ESC U`)
  used to overwrite each other. `usb_ep_write()` now waits for STAT_TX to leave
  VALID, bounded at 50 ms, and returns 0 rather than corrupting a reply.
- **Over-wide rasters are consumed, not clipped.** Clamping bytes-per-line made
  the surplus bytes of a too-wide line parse as the next line, desynchronising
  the whole block. The wire width is now honoured in full and only the head
  buffer is clipped; an unrepresentable header drops the block and resyncs.
  Regression tests: scenarios 25 and 26.
- **Motor holding torque survives a job.** The main loop called `motor_enable(0)`
  every iteration, i.e. between the USB packets of a single label, dropping all
  four phases mid-print. `motor_idle_tick(300)` now releases the motor only
  after 300 ms without a step.
- **Geometry uses 25.4 mm/inch.** `dots_to_mm()` replaced a flat `*25/dpi`, which
  reported every ESC U dimension ~1.6 % short (S0904980 as 102×156 mm instead of
  its real 104×159 mm) — the values D.MO Connect uses to pick the label size.
  Regression test: scenario 28 plus the mm checks in `test_protocol_wire.py`.
- **`delay_us()` reads a hardware counter.** TIM3 free-runs at 1 MHz as the time
  base; the previous NOP loop drifted with compiler version and was stretched by
  any interrupt landing inside it — which on a head strobe is extra energy per
  dot line. TIM3 was already declared in `mcu.h` and wired into the vector table
  but used by nothing; now it is used, and the SPI1 register map (also unused) is
  gone rather than reading as if the head were wired to it.
- **`thermal_read_raw()` is a median of three.** This reading gates every printed
  line and the `GS D 0x01` self-test, so one ADC glitch must not drop a raster
  line or refuse a diagnostic.
- **`ESC L 0` clears a raw length override.** 0 means die-cut and is what the
  stock driver sends for every die-cut job; it previously left a preceding raw
  `ESC L <dots>` in force. Regression test: scenario 34.
- **The printed height does not leak between jobs.** `s_raster_dots` is cleared
  on `ESC s` and `ESC Q`, so a feed issued before a job's first `ESC D` advances
  a full pitch. Regression test: scenario 35.

## D19 — Build identification (`GS D 0x05`)

`FIELDWORK.md` asks for measurements taken against a specific image, so the image
has to be able to name itself. The Makefile and `build.sh` stamp
`-DOPENDMO_BUILD` from `git describe --always --dirty --abbrev=8`; `GS D 0x05`
returns it as ASCII (capped at 48 chars, still one bulk packet), and
`opsend.py diag 5` prints it. Outside a git checkout the string is `dev`.

Deliberately a **new subcommand** rather than extra bytes in the `0x04`
snapshot: that layout is fixed, documented and asserted in both test suites.

## D20 — pc-patch: branch wiring is tested offline

`DYMO.LabelAPI.dll` cannot be redistributed, so `pc-patch/test/` builds a
synthetic assembly with the shapes the patcher anchors on and runs the real
patcher against it. The assertions are about **branch targets**, not merely that
patching completed — two label captures in `IlInject` used to read back the wrong
instruction, which silently turned the flag file into a no-op while producing a
DLL that loaded and ran perfectly. A patcher that "succeeds" and does nothing is
the failure mode worth designing the tests around.

## D21 — Research pass: what the published manuals already answer

Before asking anyone to open a printer, everything DYMO has published was read
end to end: the **LabelWriter 550 Series Technical Reference** (20 pp.) and the
**LabelWriter 450 Series Technical Reference** (27 pp., the same engine family
and considerably more detailed on the mechanics), plus the rated print speeds
and published `lsusb -v` output for a genuine LabelWriter 450.

That closed these, all now implemented and cited in-code:

| Was an assumption | Now sourced |
|---|---|
| Thermal limit and whether there is hysteresis | Halt at 70 °C, resume at 56 °C (LW450 p.7) |
| `ESC V` firmware string layout | `FWAP`/`FWBL` + major + minor + `MMYY`, 4 chars each (p.20) |
| `ESC o` argument width | One byte (p.20 table) |
| Bus- or self-powered, and how much VBUS current | Self-powered, 4 mA (LW450 `lsusb -v`) |
| VH rail voltage | 24 V, from the adapter table (550 p.9) |
| MainBayStatus / PrintHeadVoltage value ranges | Full enumerations (p.14–15) |
| Whether graphics mode changes the feed step | On the **450** yes (300×600); on the **550** no — it is 300×300 (550 p.8), so `ESC h`/`ESC i` correctly change nothing here |
| What the paper sensor actually senses | An infrared LED photocell reading the top-of-form hole between labels (550 p.7) |
| Per-line time budget | 0.92 ms (550) / 1.08 ms (5XL), from rated labels/min on a 1050-line address label |
| Head voltage behaviour | Suspend below 19.3 V, resume at 21 V (LW450 p.7) |
| What the button does | Short press = form feed; long hold = canned self-test patterns (550 p.8) |

And these came off a Rev K board photo rather than a datasheet: a **12 MHz
crystal** at Y1 beside the MCU (D3), **22 Ω series resistors** in banks on the
head-interface lines, and a 48-pin package at U1 whose solder joints sit flush
against the body rather than on gull-wing leads — pointing to **UFQFPN48**
(`STM32F072CBU6`) rather than the LQFP48 (`...CBT6`) the docs first assumed.
ST's datasheet Table 13 carries a single shared `LQFP48/UFQFPN48` pin-number
column, so the pad map is unaffected; what changes is that there are no leads
to probe, which is why the 22 Ω series resistors are now the recommended test
points (PINMAP, FIELDWORK 7c).

The DRM framing in `README.md` is likewise quotable rather than inferred — the
550 manual states "The label length is determined by the SKU data found on the
NFC Tag" and "Only authentic Dymo labels with a valid NFC Tag can be used for
printing" (p.7).

**What no amount of reading can give:** the F072 pin routing. The silkscreen
carries only reference designators, the FCC circuit diagram is confidential, and
the stock MCU is RDP2 so its flash cannot be read back. That is the irreducible
core of FIELDWORK.
