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
ID whose `MFG`+`MDL` makes Windows derive the exact hardware ID D.mo's
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

**Constraint on any future clock change:** keep the AHB/APB prescalers at /1.
RM0091 Rev 9 section 30 requires the APB clock to be at least 10 MHz while USB
runs ("to avoid data overrun/underrun problems"), `delay_us()` (the strobe
dwell) is derived from SYSCLK, and ES0223 2.9.4-2.9.6 need APB above twice the
IWDG clock.

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
1. **PMA access is 1:1, 1024 B at 0x40006000 — settled on paper**, no longer an
   open item: RM0091 Rev 9 gives "2 x 16 bits / word" for STM32F072 and maps
   exactly 1 KB of USB/CAN SRAM there (Table 1). Byte/halfword accesses only.
   An EP0 echo stays a useful smoke test.
2. **EPnR STAT/CTR** (`usb_core.c`) use TinyUSB's keep-mask + XOR-STAT (STAT/DTOG
   are toggle bits; CTR is rc_w0). Confirm enumeration with a USB analyzer.
3. **Pinmap** (`pins.h`) — see PINMAP.md, which now carries the complete 48-pin
   physical pad→GPIO map (Table 13, whose `LQFP48/UFQFPN48` column covers both
   packages). GPIO alternate-functions verified
   against the F072 datasheet (DocID025004 **Rev 2**, Table 14): I2C1 is **AF2**
   and exists only on PB6/PB7 or PB8/PB9 — we use **PB8/PB9**. USB DM/DP = PA11/PA12
   are *additional* functions (Table 13, enabled through the USB registers, not
   AFR); the AF2 write in `usb_init()` is ST's optional "user guidance" habit. SWD = PA13/PA14 (pads 34/37). LED/button are PA2/PA3 (PC6/PC7 are not
   bonded on LQFP48). VH enable is PA8 (assumed). The board-level pin *routing*
   is still an assumption (no board dump) — measure each pin on hardware.
4. **I2C `TIMINGR`** (`store.c`) is a start value for ~100 kHz @ 48 MHz.
5. **Dwell/density and motor timing** calibrate against print quality + temperature.
   `GS D 0x01`/`0x02` (see D15) confirm the head/motor mechanism fires before you
   tune the absolute values.
6. **`COUNT_RX` encoding** `0x8400` = BLSIZE=1/NUM_BLOCK=1 = 64 B.

## D9 — Multi-model from one codebase (550 default, 4" geometry option)

Width-parametrised via `src/model.h` + a build define. **Default = 550**
(**672 dots**, 84 B/line, 56.9 mm at 300 dpi); the **4" / 5XL geometry** is
`make MODEL=OP104` (**1248 dots**, 156 B/line, 105.7 mm). The default was the
5XL until D25 established that the 5XL board carries an STM32F407. Head widths come from the tech reference and
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
  compiled + run natively; **139 checks / 60 scenarios, both models**. Note that
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
- **Byte order:** `ESC n` is **u16 LE**, `ESC o` is a single byte, and **`ESC L`
  is u16 BIG-endian** (D22). The `ESC D` header is BPP, Align, then Width=lines
  (u32 LE) and Height=dots (u32 LE); the driver sends `BPP=1, Align=2`.
  Status/JobID/Index/Count in the reply are LE.
- **`ESC @`** = "restart print engine" → implemented as a full **pipeline reset**
  (not an MCU reboot), so the host recovers without losing USB configuration.
- **Factory reset is `ESC *` (`1B 2A`).** The stock port monitor's opcode table
  dispatches `0x2A` as `RestoreFactorySettings` and has no entry for `0x24`; the
  tech ref's `1B 24` is a hex typo next to the right mnemonic. Both spellings are
  accepted here, and `tools/opsend.py` now sends the genuine one.
- **`ESC o`** = set label count (`ESC o <count u8>`, one argument byte per tech ref
  p.20): writes the remaining-label count and **persists it to EEPROM**
  (`store_save()`), so a host-set value survives a power cycle and is echoed in the
  status struct (bytes 27–28) and the ESC U record. Counts above 255 go through
  `GS C`. Normal printing decrements the count per
  label and wraps to `MODEL_DEFAULT_COUNT` at zero (fresh-roll behaviour).
- **`ESC L`** = label length (dots, u16 big-endian). A value in `paper.h` selects
  that paper; `0` clears any override (die-cut, pitch from the default paper);
  `0x7F00` (custom size) and `0xFFFF` (continuous) take the pitch from the raster
  height just printed; any other plausible value is used as the pitch directly
  (the CUPS driver sends the raw page height). The feed adds a fixed gap (there is
  no NFC tag to read the true pitch).

## D12 — Assumptions (what is genuinely still unknown)

A research pass over DYMO's own published manuals (D21) moved several entries
out of this list. What is left:

- ~~**ESC U CRC**~~ — **RESOLVED** (D22). It is CRC-32/ISO-HDLC over bytes 0–59
  with 4–7 zeroed, stored LE at 4–7, reproduced on 37/37 genuine roll tags. The
  manual's contradictory row was right about the span and wrong about the width.
- **ESC V version *values*:** the *format* is now fully sourced (p.20: `FWAP`,
  major, minor, `MMYY`, four chars each) and implemented. Only the numbers we
  put in those fields are ours. Same one-command capture settles them.
- **Die-cut gap / tear offset:** `LABEL_GAP_DOTS` (20) and `TEAR_EXTRA_DOTS` (15)
  are fixed dot counts, not read from the roll. Tune against a real roll.
- **Density-to-dwell mapping:** `ESC C` duty is 0–200 % (sourced, p.16) and the
  status echoes it; how that percentage maps to microseconds of strobe depends
  on the head's energy curve and the rail voltage. Calibrate.
- **Paper table:** `paper.h` is keyed on the `ESC L` values in the driver GPDs,
  now complete for LW5XX.GPD (550) and lw4xl.gpd (5XL). The Windows driver never
  sends `0`; it always sends one of those values or a sentinel. Several papers
  share a value (it is a length, not an id), so the table resolves pitch and the
  ESC U geometry, while raster geometry always comes from `ESC D`.

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
reflash). Stock 550 parts are reported to ship at RDP2 (assumed likewise for
the 5XL) — this firmware cannot be installed over SWD until that is lowered
(mass-erase). The report is a UART/boot-button null result on one 550 plus a
forum diagnosis (EEVblog, "Dymo 550 Thermal Printer DRM Hacking", replies
#27–#28, 8 Mar 2022), consistent with AN2606 section 4.1 ("When readout
protection Level2 is activated, the MCU does not boot on system memory"): RDP2
explains the silence, the silence does not prove RDP2.

Footnote on RDP1: ES0223 Rev 6 section 2.1.4 ("RDP Level 1 issue", graded P on
revisions Z/B/Y,1) says a debugger "may access one data in the Flash memory
after power up" through a race with the protection logic; ST's only workaround
is RDP2. That reinforces D13 rather than changing it — RDP1 would not have
protected a converted printer, and RDP2 is the door this firmware refuses to
walk through. It is no route to a stock image either: one datum per power-up,
and public SWD bus-race dumpers such as `racerxdl/stm32f0-pico-dump` state they
"only work for Level 1". (Voltage glitching is a documented bench attack on
other STM32 parts; nothing here depends on it.)

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

- **57 mm (550 class):** head bar marking **3C56-9638** is sourced from three FCC
  internal-photo exhibits (RGDLW550 5092158 Fig 16; RGDLW550T 5092072 Fig 16;
  RGDLW550 5387314 Fig 15) — the same part on the 550 and 550 Turbo, 2021 and
  2022 builds. The equivalence to ROHM **GK11C308 / KF3002-GK11C**, D.mo assembly
  **PRTA05412**, is still sourced only from replacement-head listings for the
  400/450 generation.
- **4" / 1248-dot (5XL class):** ROHM **TE3004-TP1W00A** class — the official ROHM
  catalog (SF2024_EN_Thermal_Printheads.pdf) lists exactly **1248 dots @ 300 dpi,
  105.706 mm**, matching the 5XL spec.
- **Family architecture** (public sibling datasheet **KF3002-GL50A**): built-in
  shift registers + latch + heat drivers; host signals CLK, DI1/DI2 (one per
  half), LAT (High=HOLD / Low=THROUGH), STB1/STB2 (heat strobe per half), VH
  (24 V family standard), VDD (3.13–5.25 V), TM (built-in NTC **30 kΩ, B=3950**);
  no MISO (DO1/DO2 are daisy-chain outs). Calibration curves: Fig.3 max energy,
  Fig.4 density vs mJ/dot. Fig.2 timing chart: strobe-to-driver-output delay
  "Max.10us" per edge (SHEC G56 class: 3.5 us) — a shift of the heat pulse, not
  a stretch.

`model.h` sets **both models to 2 strobe segments** (the 57 mm head is also
two-half: 2×336); `head.c` bit-bangs the two-half shift with a sequential per-half
strobe; `thermal.c` documents the sourced NTC spec.

**Still verify on hardware:** the ROHM equivalence of the 550's 3C56-9638
marking, and the 5XL head marking (TE3004-TP1W00A vs a custom variant); the
register split per data input (`MODEL_DI1_DOTS` / `MODEL_DI2_DOTS`, assumed two
equal halves — `head.c` handles unequal ones too, and the GD31A sibling really
is unequal: 384/256 dots with four strobes of 256/128/128/128); the thermistor
divider R_p / direction (one 25 °C reading pins it); the shortest strobe that
still heats (sweep dwell down from 50 us on the bench); **and the strobe
polarity — see below.** **Confirmed:** VH = **24 V**. **Assumed:** DI1/DI2
clocked in parallel on one CLK, and `MODEL_STB_ACTIVE_LEVEL` = 0.

**Withdrawn: "STB is active-low, confirmed".** Earlier revisions of this file
said that, sourced to the KF3002 timing chart. Re-reading the chart withdraws
it: in KF3002-GL50A and -GD31A Fig.2 the STROBE trace idles LOW and pulses
HIGH, DRIVER OUT idles high and pulses low, and within the same figure /LATCH
carries a drawn overbar while STROBE does not. Other family members
(KF3002-GM50A, KF3004-GM50A, KD3004-DC72A) spell the pin "/STB1" in text, so
ROHM do mark it when a variant is active-low — meaning polarity is per-variant,
and the GK11C has no public datasheet. The firmware keeps driving it active-low
(`MODEL_STB_ACTIVE_LEVEL` in `model.h`, one place to flip), but it is an
assumption now, and FIELDWORK has a current-limited check for it before the
first 24 V test. Getting it wrong means the head fires continuously the moment
VH comes up, which is exactly what `OP_FLAG_VH_INHIBIT` exists to survive.

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
- **Feed motor driver:** not named in public teardowns; a 24 V-capable driver —
  a constant-current chopper (MP6500 class, 4.5–35 V with current limiting) or a
  discrete 4-transistor H-bridge — on 24 V, driving the four
  phases directly (`MOTOR_DRIVE_4PHASE`; STEP/DIR kept as fallback). µsteps per dot
  line are not in the TRM — count them by scoping the phase pins during one ESC D line.
  The familiar dual bridges are all under 24 V and ruled out if the motor sits on
  VH: TB6612 15 V, DRV8846 18 V, DRV8834 10.8 V, A3906 9 V.
- **STB polarity = active-low** (Low = heat driver on), from the ROHM KF3002 timing
  chart; `head.c` fires low. DI1/DI2 are assumed driven in parallel (two
  shift-register banks, one CLK) — the per-input dot counts live in `model.h`.
- **NFC link:** the SLRC610 sits on a separate RFID board, joined by a 6-pin
  1.25 mm JST-GH cable (free-dmo README). Besides SCL/SDA it carries a power-down
  line (printer → reader) and an interrupt line (reader → printer) (free-dmo
  `Inc/main.h`); the other two conductors are presumed 3V3/GND. Two unidentified
  F072 GPIOs therefore carry NFC PWDN/IRQ — do not mistake them for motor phases.
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

## D22 — Second research pass: the genuine roll tags were in our own repository

A ten-line research sweep over public sources produced 65 findings that survived
adversarial verification. The largest single result was not on the web at all:
**the root of this repository already embeds 37 dumps of genuine DYMO 550-series
roll tags** (`Src/main.c`, the Bluepill tag emulator this project forked), and
the ESC U consumable record sits at tag offset 12 in every one of them.

Checking our generated record against all 37 — arithmetic run locally, not taken
on trust — settled the project's biggest open assumption and corrected five
things the manual states differently from what real tags carry:

| Field | Was | Is, on 37/37 genuine tags |
|---|---|---|
| Bytes 4–7 | CRC16-CCITT at 4–5, 6–7 "reserved" | **CRC-32/ISO-HDLC over bytes 0–59 with 4–7 zeroed, LE** |
| Byte 3 | SKU character count | Constant `0x3C` = the 60-byte payload length |
| All geometry | whole millimetres | **tenths of a millimetre** (30256 = 1016 × 587 = exactly 4″ × 2.3125″) |
| Bytes 52–53 | pitch × count, in mm | total media length in **2 mm units** (30270: 45720 × 2 mm = 300 ft exactly) |
| Byte 56 | `0x00` per the manual | `0x01` |
| Bytes 44–47, 60–62 | `2,0,2,0` and a fake production date | zero |
| Byte 22 (material) | `0x03` "paper" per the manual | `0x03` appears on none of the 37; we send `0x04`, S0904980's own value |

**`ESC L` is big-endian.** Independently confirmed three ways: DYMO's own
open-source CUPS driver writes `(v>>8)` then `v&0xff` and pins it with a unit
test; Microsoft's GPD documentation makes `<1B>L<0867>` emit bytes `08 67` in
that order; and read big-endian, 12 of the 14 non-sentinel entries in our own
LW5XX.GPD-derived paper table are exactly `height_dots + 300`, where byte-swapped
they are noise. Our little-endian parse made every `ESC L` fall into the raw-length
branch, so every `ESC G` afterwards hit the 4000-dot feed clamp — roughly 34 cm of
blank stock per label.

**`ESC c/d/e/g` are a genuine zero-argument density family** (Light 75 %, Medium
87.5 %, Normal 100 %, Dark 112.5 %; LW450 tech ref p.19, emitted by the CUPS
driver per job). `ESC d` had been repurposed as our feed backdoor, so a host
sending `ESC d` followed by `ESC L` would have had the `0x1B` swallowed as a feed
count. The feed now lives on `GS D 0x02` and the documented `ESC f 1 n`.

Silicon errata (ES0223) that actually applied:

- **2.4.3 — ADEN cannot be set immediately after calibration.** Our
  `thermal_init()` set ADEN a handful of core cycles after ADCAL cleared, inside
  the erratum's four-ADC-clock window, on an unbounded poll that ran *after*
  the watchdog was already started. The failure mode was a silent boot loop.
  Now: a delay, a bounded retry loop that re-asserts ADEN, and watchdog kicks.
- **2.4.1 — no two calibrations without an intervening disable.** `thermal_init()`
  is now idempotent, so the rule is structural rather than a convention.
- **2.11.12 — I2C stall after the first byte.** We are safe only by arithmetic
  (APB:I2C-kernel ratio 6, outside the forbidden 1.5–3 band). Recorded as a
  constraint so nobody lowers SYSCLK for power and silently corrupts EEPROM writes.

And from RM0091: **DTOG must be initialised when a non-control endpoint is
enabled** — we were leaving the data toggle wherever the previous session left
it across a re-configuration, while the host restarts from DATA0.

Corrections to our own USB identity, from a published descriptor dump of a
genuine 0922:0028: `iProduct` carries the vendor prefix, "DYMO LabelWriter 550".

The feed motor is a **LEILI 35BY412-339** two-phase bipolar PM stepper (~35 mm
can, ~6.5 Ω/phase), which independently validates `MOTOR_DRIVE_4PHASE` — a
4-lead bipolar motor is exactly two H-bridges driven IN1–IN4.

## D23 — Third pass: wire edge cases, USB endpoint layout, paper tables

Behaviour changes, each with a regression test where the parser is involved:

- **`ESC L` sentinels.** `7F 00` (custom size) and `FF FF` (continuous) were read
  as 32 512 / 65 535-dot lengths. They now take the pitch from the raster height.
  The Windows driver never sends `0`; the CUPS driver sends the raw page height.
- **ESC runs.** DYMO's CUPS driver opens every document with a run of bare `ESC`
  bytes (156 in the code, 100 in its unit test). The parser ate them in groups of
  three and could lose the following command; a run now collapses to one ESC.
- **`ESC y` / `ESC z`** (400-series resolution) are zero-argument and no longer
  swallow the next byte.
- **Endpoints `0x82` IN / `0x02` OUT**, IN listed first — the layout of a published
  `lsusb -v` of a genuine 0922:0028. EP number 2 is also the EPnR index.
- **SOFT_RESET** now also drops a queued bulk-IN reply and clears an IN stall
  (Printer Class 1.1 §4.2.3), without touching DTOG.
- **EP0 after a STALL** is no longer re-armed to VALID on the RX side, so an
  unsupported control-OUT request stays stalled instead of ACKing its data.
- **IEEE-1284 ID** uses the LabelWriter 450 family key layout
  (`MFG;CMD;MDL;CLASS;DESCRIPTION`); the unsourced `CID` is gone. Binding is
  unaffected: `DYMO_LW5xx.inf` matches on MFG+MDL only.
- **Paper tables** complete for LW5XX.GPD and lw4xl.gpd (4x10 = `0xB80B`, A6 =
  `0xB009`; the old `0x7F00` row was the custom-size sentinel, not a paper).
- **Head data inputs** are sized by `MODEL_DI1_DOTS` / `MODEL_DI2_DOTS` (assumed
  equal halves); `head.c` also handles an unequal split.
- **`opsend.py`** sent `ESC L` without its `ESC` byte in `cmd_label_length`
  (stored as a raw control character) — now explicit.

Documentation-only: ES0223 2.15.2 (resume/ESOF, unreachable without remote
wakeup), 2.9.x (IWDG, unreachable), 2.1.4 (RDP1 race, reinforces D13), 2.2.1
(I2C analog filter vs AF); RM0091 PMA addressing and the 10 MHz APB floor; the
PA11/PA12 additional-function note; strobe propagation delay; the NFC board's
PWDN/IRQ lines; the 3C56-9638 head marking from FCC photos; motor-driver voltage
classes; the RDP2 evidence and its limits.

## D24 — Verification without a board, and what the research pass found

### New checks (all run in CI)

- **`test/test_usb.c`** runs the real `usb_core.c`, `usb_desc.c` and
  `usb_printer.c` against a software model of the STM32F0 USB peripheral —
  EPnR toggle / rc_w0 / read-only bits and the transaction behaviour of RM0091
  30.5–30.6 — and plays the host: bus reset, descriptors (compared byte for byte
  with a genuine 0922:0028 `lsusb -v`), SET_ADDRESS timing, configuration and
  DATA0 reset, bulk OUT with flow control, bulk IN with the busy guard,
  GET_DEVICE_ID / GET_PORT_STATUS / SOFT_RESET (both recipients), endpoint
  HALT, STALL recovery, ZLP, suspend. 91 checks per model. Six deliberately
  injected bugs (no DTOG reset, SOFT_RESET without flush, address applied too
  early, STAT written as read/write, no ZLP, EP0 re-armed after STALL) are each
  caught. The only source change was routing EPnR writes through one macro,
  `USB_EPR_WRITE`, which is a plain register write in the firmware.
- **`test/renode/smoke.py`** boots the actual `.elf` in Renode 1.17 (STM32F072
  platform) and checks it reaches the main loop without a fault, that SysTick
  counts real milliseconds, and that the LED follows `main.c` for cold head,
  head over 70 °C and paper out. Renode does not model HSI48RDY or give the
  ADC a settable value, so those two reads are hooked; USB is not modelled
  (hence `test_usb.c`).
- **`test/renode/head_shift.py`** calls `head_print_line()` in the image,
  records every GPIOA write, and rebuilds what the head latches on each rising
  CLK: 624 / 336 clocks, DI1 and DI2 streams exact, data never changing on the
  edge that samples it, missing bytes white.
- **`make stack`** (`tools/stack_depth.py`): worst case from GCC's call graph is
  920 bytes (5XL) / 776 bytes (550) including the deepest interrupt and the
  exception frame, against the 2048-byte stack.
- **Static analysis** (GCC `-fanalyzer`, `-Wconversion` and friends, cppcheck
  2.21): no real defect. The analyzer's out-of-bounds paths through
  `pma_write` combine a 1-byte reply with a stale length from a different
  control transfer, which the state machine cannot produce. Three loops that
  read a string byte before checking the bound were reordered anyway.
- **`make test`** now uses `set -e`: before, a failing 5XL parser test was
  masked when the 550 run that followed it passed.

### A real finding: the 5XL line did not fit the time budget

Renode's instruction count for the shift of one full line was 20 546 on the
5XL build. At 1.2–2.0 clock cycles per instruction that is 0.51–0.86 ms, and
with 0.54 ms of strobe at density 8 the line exceeded the 1.08 ms per line
that DYMO's rated 53 labels/min implies. CLK, DI1 and DI2 are all on GPIOA, so
`head.c` now writes "both data bits + CLK low" and "CLK high" as two BSRR
words from a four-entry table built once per line: 11 898 instructions
(0.30–0.50 ms) for the 5XL, 6 462 for the 550, same bit stream, verified by
`head_shift.py`. `HEAD_SHIFT_SAME_PORT` in `pins.h` guards the assumption and
`head_init()` refuses to drive the head if a remap breaks it.

### Research results

- **DYMO Connect never reads `ESC U`.** Decompiling `DYMO.LabelAPI.dll`,
  `DYMO.PrinterCommands` and the rest of the installed assemblies: the only
  printer commands built are `ESC A`, `ESC Q`, `ESC V`, `ESC W`, and no 0xCAB6
  magic or record CRC exists anywhere. Roll state comes from the 32-byte
  status: bay status (byte 10: 8 OK, 10 counterfeit → "unknown label" dialog,
  NoMedia → empty), the 12-byte SKU, and the label count. The label size
  comes from the SKU catalog, filtered by region; an SKU the catalog does not
  accept for the install's region shows as empty, not counterfeit. So our
  `ESC U` fidelity matters only to other hosts, and host acceptance depends on
  status byte 10 = 8, a catalogued SKU and a count — which is what `pc-patch`
  already addresses.
- **Device ID.** Two independent reports of the genuine LabelWriter 450 string
  (apple/cups#5821, michaelrsweet/pappl#396):
  `MFG:DYMO;CMD: ;MDL:LabelWriter 450;CLASS:PRINTER;DESCRIPTION:DYMO LabelWriter 450;SERN:01010112345600;`,
  with `SERN` equal to the USB serial in the CUPS URI. Our string now ends in
  `SERN:<USB serial>;` the same way. No genuine 550/5XL string was found; DYMO's
  INF confirms MFG `DYMO` and MDL `LabelWriter 550` / `LabelWriter 5XL`
  (untruncated at 19 characters), and the 550 Turbo as `LabelWriter 550 Turbo`.
- **ESC V values:** no genuine capture exists publicly. DYMO Connect contains
  `FWAP`/`FWBL` and a `{0}.{1}` format next to them, so it most likely shows
  "major.minor" (inference).
- **FCC internal photos** (RGDLW550 5092158 / 5387314, RGDLW550T 5092072,
  RGDLW5XL 5092116), read at their 1072×804 resolution:
  - head bar `3C56-9638` on the 550 and the 550 Turbo; the 5XL bar is not
    visible in its exhibit;
  - feed motor `LEILI 35BY412-339 6.5Ω` on the 550 and, partially legible,
    the same on the 5XL;
  - NFC daughterboard `LW NFC BOARD REV E 200805` with a 32-pin QFN and a
    crystal, on a **6-wire** cable (consistent with I2C + PWDN + IRQ + power);
  - button board `LW550 Button RevB`, main boards `LW550_Rev E 20200812` and a
    2021 revision;
  - the 550 Turbo and 5XL main boards, which carry an RJ45 LAN jack, show one
    large ST QFP of about 14 mm next to it — identified in D25 as an
    STM32F407VET6.
  - Not legible anywhere: motor-driver, EEPROM, load-switch and regulator
    markings, the paper-sensor type, and any trace to an MCU pin.
- **Motor:** LEILI 35BY412 = 7.5°/step (48 steps/rev); low-resistance bipolar
  variants run on 24 V; no gearbox in the -339 part. No source gives the gear
  train or roller diameter. One full step per line fits the rated speed
  (~1360 rpm); two does not. `MOTOR_STEPS_PER_LINE` stays 1, now as an
  estimate with a reason rather than a placeholder.

## D25 — The 5XL is a different board; the 550 is the target

### MCU identification (markings read from full-resolution community photos)

- **LabelWriter 5XL: STM32F407VET6** on three boards — "LW5XL DATE 20221017
  Rev: I" (`STM32F407 VET6 … PHL 7B 306`) and "LW5XL DATE 20200624 RevD"
  (free-dmo/free-dmo-stm32 issue #50), and a 2022 5XL (issue #2). The RevD
  underside carries a **KSZ8081** RMII Ethernet PHY (EEVblog thread "Dymo 550
  Thermal Printer DRM Hacking", page 200).
- **LabelWriter 550 Turbo: STM32F407VET6** — "Revision I" (EEVblog page 175,
  two boards), board silkscreen "LW550T DATE 20221017 Rev: I".
- **LabelWriter 550: STM32F072, 48-pin UFQFPN** — "LW550B_Rev:E 20200812"
  reads `STM32F 072C?U6`; the fifth character (8 or B) is not legible
  (EEVblog page 25). The owner's own Rev K photos show the same 48-pin QFN
  beside the 12 MHz crystal. A Rev K owner reports two changes from Rev H: a
  different U2 stepper driver and a 3-wire label-sensor flex.
- No board shows a separate network coprocessor; the "DYMO BGA" in an older
  PINMAP table is not supported by any photo and has been removed.

### Consequences

- **The image targets the 550.** `make` now builds OP57 by default. OP104
  stays: its 1248-dot geometry, 5XL USB identity, paper table and all the
  protocol code are exactly what an STM32F407 port needs, and it keeps being
  built and tested so that nothing rots. It does not run on a genuine 5XL.
- **Linked for 64 KB.** The image is ~11.7 KB; declaring 64 KB makes it fit an
  F072C8 as well as a CB, and the linker would fail the build first.
- **An F407 port is a separate job**: different core (Cortex-M4), USB OTG FS
  (DWC2) instead of the F0 device peripheral, I2C v1, different ADC/GPIO/RCC,
  and a completely unmeasured pin map on a board with Ethernet in the way.

### Also in this pass

- **`ESC W` length (bug).** The decompiled `ControlCommand` builds the header
  as `len = payload + 6 − 2`, i.e. the length counts the four bytes after
  `ESC W`. The parser skipped `len` bytes after those four, swallowing four
  bytes of the next command. It now skips `len − 4`. Test 51.
- **`ESC R` (firmware update) is refused cleanly.** DYMO Connect's updater
  (`SecureFwUpdateCommand`, object `0xF100`) sends `ESC R 00 01 00 F1`, a
  128-byte signed header, and waits for `ESC r <status>`; `ReflashCommand`
  (`ESC R 04 03 00 00`) reboots a genuine printer into its bootloader. We
  ignore the reflash, consume the 128 bytes and answer `ESC r 01`, so an
  update attempt aborts with an error instead of streaming an image through
  the command parser. Test 52.
- **No stock firmware is obtainable.** DYMO Connect fetches
  `Software/dymoconnect/updates/Updates.xml` from
  `dymoreleasecontent.blob.core.windows.net/dymo-release/` (fallback
  `printdymolabel.azurewebsites.net`); its `<Firmware>` entries (HardwareVersion,
  FirmwareVersion, ProductID, DownloadURL, MD5) are empty today, the blob
  container is not listable, the image is only held in memory, and the
  128-byte "secure header" handshake implies a signed image checked on the
  printer. Stock 550 parts are RDP2. So the pin map cannot come from a dump.
- **Config EEPROM in Renode** (`test/renode/eeprom.py`): the real image
  against a 16 KB 2-byte / 64-byte-page part and a 256 B 1-byte / 8-byte-page
  part — defaults persisted at 0x100 resp. 0 with the right addressing, the
  low 256 bytes of the big part untouched, stored and legacy-offset records
  loaded, bad density sanitised, a write-protected part left alone with the
  firmware running on RAM defaults. One scenario is deliberately absent and
  documented: Renode's EEPROM commits bytes before the STOP condition, which
  a real 24Cxx does not, so a pre-stored record on the small part cannot be
  tested faithfully with it.

## D26 — Reflash over USB, and an end-to-end host test

- **USB DFU entry.** `GS D 0x09 'D' 'F' 'U'` (three confirmation bytes, refused
  during a job) drops the heat rail, replies, stores `0xDF00B007` in a `.noinit`
  RAM word and requests a system reset. `Reset_Handler` checks that word before
  anything else — before `.data`/`.bss`, the clock and above all the
  independent watchdog, which once started could not be stopped and would pull
  the part out of the boot loader after ~4 s — clears it, remaps system memory
  (SYSCFG MEM_MODE = 01) and jumps to ST's boot loader at `0x1FFFC800` with its
  own MSP (AN2606, STM32F071xx/072xx). The boot loader enumerates as
  `0483:df11` and clocks USB from HSI48 + CRS, so it needs nothing from the
  board. Only the first image needs SWD; `opsend.py dfu` + `dfu-util` does the
  rest, which makes the fieldwork's rebuild-and-retry loop practical. Checked in
  `test/test_protocol.c` (scenario 53) and `test/renode/dfu.py` (flag + AIRCR on
  request; jump with the boot loader's MSP and a cleared flag on the next boot).
- **`test/test_e2e.c`.** The USB stack and the parser had been tested
  separately; this joins them on the peripheral model. It found no defect, and
  a deliberately broken flow control (never pausing the endpoint when the ring
  is full) fails 11 of its 26 checks, so it does watch the seam it was written
  for.

## D27 — First audit cycle: safety, host acceptance and framing

Findings from a multi-lens audit of the whole stack, each one independently
verified before it was acted on.

**Safety (the heat rail).**
- `ESC *` / `ESC $` cleared `OP_FLAG_VH_INHIBIT`: `factory_reset()` assigned
  `flags` instead of merging, so a single host command disarmed the interlock
  that `store.h` promises no command sequence can defeat. The assignment is now
  monotone — it keeps the bit if it is set and never sets it by itself.
- `GS D 0x07` (toggle a pin) could drive `PIN_HEAD_VH` and the fitted strobes.
  The gate is active-low, so "drive it low for a millisecond" is exactly how the
  24 V rail is switched on, and on a strobe it is an unmetered heat pulse
  outside the thermal gate. Those pins are now refused (`pin_is_head_hot()` in
  `pins.h`); the spare strobes stay toggleable, since finding them is the point.
  The same function also restored `MODER` while leaving the pin driving low —
  it now restores the output level too.
- The interlock itself had never been executed by a test: both C suites replace
  `head.c` with a mock that reimplements the check. `test/renode/head_shift.py`
  now runs the real image twice — rail clear and rail inhibited — and asserts
  that `PIN_HEAD_VH` is driven only after the latch, exactly once, and never at
  all while the interlock is armed. Deleting the check in `head.c` fails it.

**Host acceptance.**
- `ESC Q` left the job id in the status struct. DYMO's own published language
  monitor takes the print lock only when the engine is idle **and** the job id is
  zero (`LabelWriterLanguageMonitorV2.cpp`, `CheckLock()`), so the genuine driver
  could never have printed a second job. `ESC Q` and every reset path now clear
  it.
- `ESC Z` (`CompressedPrintData`) was unknown to the parser, which ate one byte
  and then ran the compressed body through the command state machine. It is now
  consumed exactly, using the 17-byte header layout recovered from the port
  monitor.
- An interrupted firmware-update handshake left `s_refuse_update` armed across a
  reset, so the next job's `ESC M` skip emitted a stray `ESC r 01` into the
  reply stream.

**Robustness.**
- `protocol_reset()` runs in USB interrupt context and zeroed the ring indices
  under a running parser, racing `ring_getc()`'s read-modify-write of the tail:
  the parser could resume against a stale tail and chew through ~2000 bytes of
  exactly the data the reset exists to discard. The interrupt now only records a
  discard mark (`s_head`, which only it writes) and makes the head safe; the
  main loop applies the reset between bytes. Data that arrives after the reset
  is preserved, which a plain "clear everything" would have dropped.
- `motor_step_lines()` never kicked the watchdog. A maximum feed is 4000 lines
  at 800 us = 3.2 s against an IWDG timeout of 3.2 s at the datasheet's fastest
  LSI (4.0 s typical) — and 4.8 s with `MOTOR_DRIVE_STEPDIR`. It now kicks per
  dot line, like every other long loop in the tree.
- Endpoint-recipient standard requests took any `wIndex`. `EPR[]` has eight
  entries and an endpoint that was never given an address answers for endpoint 0
  (RM0091 30.6.2), so `SET_FEATURE(HALT, 0x81)` could wedge the control pipe.
  Only `0x02` and `0x82` are honoured now; the control pipe follows USB 2.0
  9.4.5.
- `thermal_scan_adc()` reported the thermistor ten times: `adc_sample()` always
  re-selected `ADC_HEAD_TEMP_CH`, overriding the per-channel selection. The
  channel is now a parameter, and a timed-out conversion is stopped and drained
  so that later channel selections do not land in the window RM0091 13.5 forbids.

**Provenance.** The engine's own opcode table was recovered from the stock port
monitor's dispatch tables and is now an appendix to PROTOCOL.md. It also settles
`ESC *` vs `ESC $` (see D18) and names five commands nobody has documented
argument layouts for.

## D28 — What the same-hardware hunt settled (and what it withdrew)

The 550's own firmware is unobtainable, but its parts are not unique. Reading
the public material for the KF3002 head family, a published thermal-mechanism
technical reference of the same architecture, and two shipping open-source head
drivers produced the following. Every number below is tagged: **SOURCED** (a
document about our part), **ANALOGUE** (a different part; only the form
carries), **ASSUMED** (our choice).

- **`HEAD_BASE_DWELL_US` 270 us keeps its value and loses its justification.**
  It used to be "about the knee of the density curve"; three KF3002 siblings'
  density curves disagree by a factor 1.9 at identical electrical spec, so the
  knee is not a family property. 270 us is now pinned to the family's rated
  operating point instead: GL50A TON 0.28 ms, GD31A 0.308 ms, GM50A 0.263 ms,
  all at Rave 1250 Ω / VH 24 V. ANALOGUE.
- **The 2000 us dwell clamp became a 410 us energy ceiling** (`HEAD_MAX_DWELL_US`).
  2000 us was not derived from anything and allowed roughly four times ROHM's
  flat maximum energy per dot; the diagnostic `GS D 0x01` was asking for more
  than the flat maximum on a cold head, and a 200 % density request would also
  have taken 1350 us of strobe against a 920 us per-line budget. 410 us is
  ROHM's maximum-energy envelope at the 550's rated line time. Nothing the
  genuine driver can ask for is clipped: its darkest preset is 112.5 %, 378 us.
- **The temperature law is linear in temperature, and ours was linear in ADC
  code.** Published references give `E = E25 − Tc·(T−25)`, at −1.16 %/K for one
  characterised paper, and a shipping 24 V product's strobe table is linear in
  T at −1.38 %/K with the same relative slope at every line rate. Our endpoints
  (−1.11 %/K) were already within 5 % of that, but tapering in raw code sagged
  ~5 % low in the middle. `thermal.c` now uses a 32-entry table generated by
  `tools/gen_thermal_table.py` from declared inputs. SOURCED that such a law is
  used at all (LW450 TRM p.7); ANALOGUE for its form; ASSUMED endpoints.
- **The head thermistor is double-sourced.** A published mechanism reference
  tabulates the same class of NTC at 25 °C = 30.00 kΩ, 55 °C = 8.92 kΩ,
  70 °C = 5.27 kΩ; `tools/gen_thermal_table.py --check` computes 5.280 kΩ from
  R25 = 30 k, B = 3950 — 0.2 % apart.
- **The 300 ns latch-to-strobe setup is now explicit.** The KF3002 chart's
  unlabelled timings were decoded against a sibling's labelled version of the
  same figure: CLK width ≥ 30 ns, DI setup 30 ns, DI hold 10 ns, LAT setup
  200 ns, LAT low 100 ns, LAT hold 50 ns, LAT-rise → STB ≥ 300 ns. Everything
  except the last is met by construction; the last was only met by accident of
  code generation, so `head.c` now spends 20 NOPs there.
- **Strobe splitting is a supply constraint, not a head requirement.** Both
  published siblings rate "maximum number of dots energized simultaneously" at
  the full dot count. What limits us is DYMO's own 42 W brick against a line of
  KF3002-class dots. `head.c` now also skips a strobe whose half has no dots —
  free, and it yields the per-line dot count any future current-aware scheme
  needs.
- **Second-segment sag is real but not transferable.** A shipping mechanism
  adds a fixed 10 us to its second heat group "to compensate for the voltage
  drop during the second group's heating". Our rail is an unregulated brick, so
  the effect is at least as large — but 10 us was 4 % of *their* pulse on
  *their* supply. `HEAD_SEGMENT_SAG_US` exists and is 0: the mechanism is
  recorded, the number is not guessed.
- **Explicitly rejected:** porting that reference's affine voltage correction
  (`V = 1.368·Vp − 2.800`). It is fitted over 4.75–9.5 V, our rail is 24 V, and
  the same source publishes a different pair for the same head with different
  paper. Do not re-propose without a measurement.
- **Still open, and now named:** DYMO's LW450 TRM p.7 states a head-voltage
  rule we do not implement — "if the voltage drops below 19.3 volts at the
  print head, printing is suspended until the power supply recovers to 21
  volts". That is the same latch-with-hysteresis shape as the 70/56 °C gate and
  wants a VH sense divider that is not in the pin map. Recorded in PINMAP and
  FIELDWORK rather than stubbed in code.

Testing that came with it: `test/test_thermal.c` links the real `thermal.c` and
`head.c` against an ADC model and checks the dwell curve against the published
law (it fails on the old linear-in-code taper), the 70/56 °C latch, the
median-of-three's rejection of a single outlier in both directions, the 20 ms
cache, and that no density/temperature combination can exceed the energy
ceiling.

## D29 — A sibling firmware image exists, and DYMO's own driver is the better oracle

### The image

"No DYMO firmware image is published" was true of the **550** and false of the
family. The LabelWriter 450 / 450 Turbo updater (`LW450Updater.exe`, still
served by DYMO and byte-identical to a 2015 archive copy) carries the printer
firmware as a plain const array: 20 480 bytes, unencrypted, an ARM Cortex-M0
image for an **NXP LPC11Uxx** at load address `0x2000` (an 8 KB boot loader sits
below it). It disassembles cleanly.

What it shows, and what it does **not** settle for us:

* the 450 drives the head's shift register from **hardware SPI** (SSP1 at
  PCLK/4, 8-bit frames), not bit-banged GPIO — a datapoint for our own
  two-write BSRR shift, not an argument against it;
* compressed rasters on the 450 are a simple run length: a count byte `n < 0x80`
  emits `n + 1` zero bits, `n >= 0x80` emits `n − 0x7F` one bits, bit-banged
  with the SPI function temporarily disabled;
* its `ESC V` reply is ten ASCII bytes chosen from four constants
  (`1750110f0J`, `1750111f0J`, `1750283f0J`, `1750284f0J`) selected by two
  bytes in a config page above the application — a different format from the
  550's 34-byte `FWAP`/`FWBL` record, so it does not answer our `ESC V`;
* its `ESC A` reply is a **single byte** with only bits 0, 1, 5 and 7 used;
* it uses **no I2C at all**: no NFC frontend, no roll tag. The 450 predates the
  550's roll DRM, which is consistent with the owner's report that a 450 board
  in a 550 simply prints;
* it reads three ADC channels and compares one against 10-bit thresholds 176
  and 232 with hysteresis. Which channel is head thermistor, supply sense or
  gap sensor is **not** established, so those numbers stay out of `thermal.c`.

The 450 is a different MCU, a different generation and a different command
dialect. Its value is as corroboration and as a worked example of how a real
LabelWriter is built — not as a source of constants for this firmware.

### The better oracle

The stock Windows port monitor (`lw5xxmon.dll`, the DPL engine) turned out to
be the authoritative source for the 550 language, and it settles more than the
tech reference does:

* the **complete opcode table** (38 commands) and, separately, the **exact byte
  length of every command** — see the PROTOCOL.md appendix. Six commands take
  more arguments than our default rule assumed; `protocol.c` now consumes all of
  them correctly and scenario 58 pins it.
* `ESC o` (`0x6F`) and `ESC $` (`0x24`) are **not commands** in the genuine
  language; both land in its Unknown case. The real copy count is `ESC #`
  (`0x23`) and the real factory reset is `ESC *` (`0x2A`). We keep `ESC o` and
  `ESC $` as our own accepted aliases, now labelled as such.
* the `ESC V` record is **load-bearing**: `FWAP`/`FWBL` must match exactly, the
  major and minor fields must parse base-10 as non-zero, and the PID in the
  reply overrides the host's idea of the model — which in turn selects whether
  it expects a 32-byte or a 70-byte `ESC A` reply.
* `ESC D`'s header (`BPP`, `Align`, u32 lines, u32 dots) is confirmed byte for
  byte by both the driver's builder and its parser; the `MainBayStatus` 0–10
  enum and `ErrorID` at byte 23 are confirmed from the driver's own decoder,
  and only values 6, 7 and 8 are non-error.
* the IEEE-1284 `MFG`+`MDL` halves are proven rather than derived: DYMO's INF
  binds `USBPRINT\DYMOLabelWriter_550C80D` and `...5XLB920`, so those CRC
  suffixes no longer need a bench check. Only the optional keys (`CMD`,
  `CLASS`, `DESCRIPTION`, `SERN`) remain unobserved.

Recorded so nobody re-derives it: the tables come from `lw5xxmon.dll` version
1.1.0.266, with the opcode names at one jump table and the lengths at another.

## D30 — The genuine energy model, recovered from LabelWriter 450 firmware

The 450 firmware image described in D29 was disassembled in full. Because the
device owner reports that a 450 mainboard fitted into a 550 **prints
correctly**, this firmware is driving the very mechanism this project targets:
the same head, motor and sensors, over compatible connectors. Its numbers are
therefore not an analogue — they are a working drive model for our hardware.
Everything below is read from the image, not inferred.

### The machine it runs on

NXP LPC11Uxx Cortex-M0 at 48 MHz from a 12 MHz crystal (PLL M=4, P=2) — the
same clock arrangement as ours. Three timers: a 0.375 µs tick for the strobe
and the line engine, and a 1 µs general timer.

### The head interface

| Signal | Pin | Detail |
|---|---|---|
| Data | PIO1_22 | SSP1 MOSI, 8-bit SPI, CPOL=0 CPHA=0, bit clock PCLK/4 = 12 MHz |
| Clock | PIO1_20 | SSP1 SCK |
| Latch | PIO1_23 | **active low**, a minimum-width pulse right after the line is shifted |
| Strobe | PIO1_16 | **active low**, released by a timer match interrupt |

Three things follow directly:

1. **The strobe is active low on hardware that prints on our mechanism.** Our
   `MODEL_STB_ACTIVE_LEVEL = 0` is now backed by working firmware, not by a
   datasheet drawing that in fact shows the opposite (D28). It stays an
   assumption for the 550's own board, but a much better supported one.
2. **The genuine firmware fires ONE strobe for all 672 dots.** There is no
   strobe grouping in it at all. We fire two halves sequentially on the
   reasoning that a 42 W brick cannot supply a whole line — and that reasoning
   may simply be wrong for this mechanism, since DYMO's own firmware does not
   split. `MODEL_STROBE_SEGMENTS` stays 2 (it is the conservative choice, and
   the 550's board is not the 450's), but the evidence is recorded and the
   sequential split is now known to cost line time the genuine product does not
   spend.
3. **The head is 84 bytes = 672 dots**, hard-coded in the firmware. An
   independent confirmation of our geometry from the vendor's own code.

### The pulse-width computation

In timer ticks of 0.375 µs:

    w = 400 + P/2 + 3·B + 3·(V/4),   clamped to 200 ≤ w ≤ 2500

* `400` ticks = 150 µs base;
* `P` = the current **line period** in the same ticks, so a slower line gets
  more energy — exactly the adaptive-speed behaviour ROHM publish as a curve
  and which our fixed dwell does not model at all;
* `B` = the number of **dot-data bytes in this line** (0…84), i.e. a real
  dot-count term worth up to 252 ticks = 94.5 µs. This is the rail-sag
  compensation our `HEAD_SEGMENT_SAG_US` knob was left at zero for;
* `V` = a 10-bit ADC reading (the firmware reads three channels; which one
  feeds this term is not yet pinned down), contributing up to 765 ticks =
  287 µs;
* the clamp is **75 µs … 937.5 µs**.

Then the density command multiplies it: `ESC c ×0.75`, `ESC d ×0.875`,
`ESC e ×1.0`, `ESC g ×1.125` — integer shift-and-add in the firmware, and
exactly the 75 / 87.5 / 100 / 112.5 % ladder we already implement. That part of
our model is now confirmed against the vendor's own code.

### What this does NOT license yet

Our own dwell is a fixed 270 µs at density 100 %, scaled by temperature and
capped at 410 µs (D28). The genuine model would give roughly 850 µs for a full
line at the 550's rated line rate — about three times more energy per dot, in
one pulse rather than two. That is a real conflict with the ROHM family rating
(TON 0.28 ms) that D28's ceiling was derived from, and it is not a conflict to
resolve by taking the larger number: the two could differ because the 450 runs
its line slower, because `V` is a supply-voltage term that idles high, or
because the 450's head is driven at a different effective rail. Raising the
energy into an irreplaceable head on one reading of one disassembly is exactly
the kind of step this project does not take.

So this entry records the model and **queues the port as the next piece of
work**, with its own verification: re-derive `P`, `B` and `V` from the
disassembly independently, establish which ADC channel `V` is, and reconcile
the result with ROHM's rated operating point before any constant in `head.c`
moves. Until then `head.c` keeps its conservative ceiling, and the parts of the
model that are already confirmed — the density ladder, the dot-count term's
existence, the active-low strobe and latch — are documented here.
