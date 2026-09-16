# PINMAP — OpenDMOfw (STM32F072xB, 48-pin, 128 K)

All pins live in **one editable table**: `src/pins.h`. This file records the
reasoning and the **confidence** per choice.

> **Status: interface SOURCED, routing ASSUMED.** The head *interface* (which
> signals exist, latch polarity, thermistor spec) is sourced from the ROHM
> KF3002 head datasheet — see "Thermal head identification" below. The
> board-level pin *routing* (which MCU pin D.mo wired each signal to) is still
> an assumption: no board dump was used (the reference MCU is RDP-protected
> against read-out). Measure each pin before you power the head or motor.

## Thermal head identification (sourced)

The D.mo LabelWriter 550 series is a refresh of the 450 series (same 57 mm /
672-dot / 300 dpi head; both tech references agree), and the 5XL continues the
4XL (1248-dot / 300 dpi = 105.7 mm).

| Model  | Head (ROHM)                              | D.mo assembly | Source |
|--------|------------------------------------------|---------------|--------|
| 57 mm  | SHEC 3C56-9638 / GK11C308 / **KF3002-GK11C** | PRTA05412 | Replacement-head listings (eBay/Amazon) for the 400/400 Turbo/450 Turbo, which shares this head |
| 105.7 mm | **TE3004-TP1W00A** class (1248 dots @ 300 dpi, 105.706 mm) | — | ROHM official catalog SF2024_EN_Thermal_Printheads.pdf (exact dot count) |

The public sibling datasheet **KF3002-GL50A** (ROHM, "Thick Film Thermal
Printhead 300DPI", via alldatasheet) documents the family architecture:

- **Built-in shift registers + latch + heat drivers.** Host signals: `CLK`,
  `DI1`/`DI2` (one serial data line per half), `LAT`, `STB1`/`STB2` (heat strobe
  per half), `VH` (heat supply, 24 V standard for the family), `VDD` (logic,
  3.13–5.25 V), `GND`, `TM` (thermistor). `DO1`/`DO2` are data-out for
  daisy-chaining extra heads — **there is no MISO line**.
- **Two shift-register halves** (GL50A: 2x320 dots; D.mo 672-dot head: 2x336;
  1248-dot head: 2x624 unless the board shows 4 heat lines).
- **LAT polarity: High = HOLD, Low = THROUGH** (active-low latch) — sourced.
- **Built-in NTC thermistor: 30 kOhm, B = 3950** (equivalent circuit + Fig.5
  curve) — sourced; the divider topology on the D.mo board is still an
  assumption.
- **Timing:** CLK min period ~100 ns class (bit-banging at a few MHz is far
  inside spec); data "High = BLACK, Low = WHITE".
- **Calibration curves:** Fig.3 maximum energy (SLT ms/line vs TON), Fig.4
  density vs energy (mJ/dot) — reference material for dwell/density tuning.

**Confirmed:** `STB` is **active-low** (Low = heat on); DI1/DI2 are driven **in
parallel**; VH = **24 V**. **Still verify on hardware:** exact part marking on the
550/5XL boards (GK11C vs a newer revision; TE3004-TP1W00A vs a custom variant), and
the thermistor divider R_p / direction (one 25 °C reading pins it).

## Board-level facts

- **No public 550 schematic exists** (FCC RGDLW550 circuit diagram is
  confidential "metadata only"); the F072 GPIO map for head/sensor/motor must be
  measured on the board. The head flex carries: CLK, DI, LAT, STB (possibly
  multiple), VDD 3.3/5 V, VH, GND, two NTC wires.
- **VH = 24 V** — now first-party sourced, not inferred: the 550 Technical
  Reference p.9 lists the adapters as 24 VDC at 1.75 A (550), 2.5 A (550 Turbo)
  and 3.75 A (5XL), and names the DC jack (JP2, 5.5 × 2.5 mm, centre positive).
  The head's heat supply is that rail via a P-MOS/load switch, not a separate
  buck. Logic VDD comes from the 3V3 rail.
- **EEPROM:** Rev H/I/K = **BL24C128A** (Belling, 128 kbit, **64 B page**,
  **2-byte addressing**), 7-bit address **0x50** (A0–A2 to GND), WP pins shorted.
  Rev E = smaller Atmel **AT24C01D/02D** (8 B page, 1-byte addressing), same
  address. `store.c` detects the scheme at init and works on both.
- **NFC front-end:** SLRC610 on the same I2C bus @ **0x28** — a different device
  address; our firmware ignores it (no tag emulation in scope).
- **Feed motor driver:** not named in public teardowns; likely a 4-transistor or
  small dual-H-bridge (TB6612/MP6500 class) on 24 V, driving the four phases
  directly (**IN1-IN4 expected**, no separate STEP/DIR chip — `motor.c` default
  is now 4-phase). One raster line = 1/300 inch = **0.08467 mm** (the LW450
  reference confirms the elements are "0.085 mm square ... spaced at 300 per
  inch"); µsteps per line are not in either manual — count them by scoping the
  phase pins during one ESC D line. **Time budget:** DYMO rates the 550 at 62
  labels/min and the 5XL at 53 on a 4-line address label (89 mm = 1050 lines),
  i.e. **0.92 ms and 1.08 ms per line**. Anything slower than that is not
  genuine-speed, which is why `head.c` shifts via BSRR and the motor step
  overlaps the strobe.
- **Head voltage sense — an input we do NOT have.** The genuine engine "measure[s]
  the print voltage and the head temperature before each print cycle" and
  suspends printing below 19.3 V, resuming at 21 V (LW450 reference p.7). That
  needs a divider from the 24 V rail to an ADC pin; `pins.h` has no such pin, and
  the status struct's PrintHeadVoltage field is hardcoded to "ok". If you find
  the divider on the board, that is where it plugs in.
- **Top-of-form sensing:** an **infrared LED photocell** reading the sense hole
  between labels, with the engine counting motor steps between holes (550
  reference p.7). So it is an emitter/detector pair, the detector may be analog,
  and the emitter may need its own drive pin — none of which `pins.h` models yet.
- **Head interface:** STB **active-low**, DI1/DI2 driven **in parallel**, NTC
  **30 kΩ B3950** with sourced R(T) curve — in `head.c` / `thermal.c`.
- **FCC RGDLW550 internal photos** are too low-res for GPIO traces; the circuit
  diagram is confidential. The MCU is an **STM32F072CB** on both Rev E and Rev K
  photos; the Rev E shot reads as the LQFP48 (`...CBT6`) while the Rev K close-up
  looks like the leadless **UFQFPN48** (`...CBU6`) — a plausible cost-down
  between revisions, and harmless to us since the two packages share one
  pin-number column. Motor-IC PN and GPIO routing still need a probe.

## Confirmed from a rev E board photo

A high-res top-view photo of a **rev E** mainboard (image not bundled with this
repo) confirms three part IDs:

| Component  | Part (as marked) | Meaning |
|------------|------------------|---------|
| Main MCU   | **STM32F072CB** | 48-pin, 128 K flash / 16 K RAM — exactly the target part (LQFP48 on this Rev E shot; see the Rev K note for the package difference) |
| EEPROM     | **24C02A**        | 2 Kbit (256 B), **1-byte addressing**, 8 B page (the small rev-E part) |
| NFC FE     | **SLRC610**       | NXP NFC reader/writer front-end (I2C @ 0x28) |

This confirms the **two-EEPROM model**:

| Rev  | EEPROM                    | Size | Addressing | Notes |
|------|---------------------------|------|------------|-------|
| E    | Atmel **AT24C01D/02D** (marked `24C02A` here) | 128 / 256 B | 1-byte, 8 B page | the small part |
| H/I/K | **BL24C128A**, I2C `0x50` | 16 KB | 2-byte, 64 B page | WP pins 7–8 shorted |

`store.c` detects the scheme at init using the config magic as the external
reference (a round-trip probe can't tell them apart — it's self-consistent under
either). First boot writes and **reads the magic back**; if 2-byte fails it
retries 1-byte; if both fail, config stays in RAM (WP high / missing EEPROM).
The MCU is confirmed as **F072CB** (LQFP48), which bounds the pin map to that
package's pinout. PC6/PC7 are **not bonded** on LQFP48.

**WP note (rev H/I/K):** the WP pins are tied by a solder blob on the board — a
hardware configuration our firmware does not control. If that tie write-protects
the EEPROM, `store_save()` degrades gracefully (config falls back to defaults)
and the **`GS D 0x03` self-test** flags it (write+read mismatch). Verify writes
are accepted on a rev K board via that self-test; no firmware change needed.

## Confirmed from Rev K board close-ups

Two high-resolution close-ups of a Rev K mainboard (images not bundled) add
three facts that no datasheet could give:

| Observation | Reading |
|---|---|
| **U1 is a 48-pin package**, twelve joints on each of the four sides, and the solder fillets sit flush against the body rather than on protruding gull-wing leads | Almost certainly **UFQFPN48**, i.e. **STM32F072CBU6**, not the LQFP48 `...CBT6`. **The pin map is unaffected**: ST's datasheet Table 13 carries a single shared `LQFP48/UFQFPN48` pin-number column, so every pad number below holds for both. What changes is probing — see the note under the table |
| **Y1 = HC-49 can marked `AXC12.00-115`**, directly beside U1, with its load capacitors C7/C8 | A **12 MHz HSE crystal** on the MCU. 12 × PLL4 = exactly 48 MHz — see DECISIONS D3 and `-DOPENDMO_CLOCK_HSE12=1` |
| **Banks of SMD `220` (= 22 Ω) resistors** ringing U1 — R7–R12, R18–R27, R29, R31–R33, R39 — interleaved with `102` (1 kΩ) parts | The head/motor interface lines are **series-damped at 22 Ω**. Continuity checks will read tens of ohms, not a short |
| A SOIC-8 at **U6** near C18/C37 | Candidate for the config EEPROM; marking not legible in these shots |

The silkscreen carries reference designators only (`U1`, `R27`, `C33`, `Y1`…) and
never signal names, which is exactly why the routing still has to be probed.

> ⚠ **Do not probe the MCU pads directly.** A UFQFPN48 has no leads: the pads are
> 0.5 mm pitch and flush with the package edge, so a meter probe bridges two of
> them easily, and a slip across two powered pins can take the part with it.
> Probe the **22 Ω series resistors** instead — every head and motor line passes
> through one, each is an accessible 0402/0603 pad, and electrically it *is* the
> MCU pin. The exposed thermal pad underneath is tied to VSS, which at least
> makes ground easy to find.

## Board component map

Two high-res (4080×3060) photos of the mainboard from different angles (images
not bundled with this repo). **These are Rev K boards** — so per the two-EEPROM
model above, the config EEPROM on this board is the **BL24C128A** (16 KB, 2-byte
addressing, 64 B page, I2C `0x50`), *not* the small AT24C02 seen on the rev E
photo. `store.c` still auto-detects, but this board's default path is the
2-byte/BL24C128A scheme.


| Component | As marked / seen | Reading | Confidence |
|-----------|------------------|---------|------------|
| Main MCU  | **STM32F072CB** (readable from two angles; lot `ARM 114928 B02`, `P49 1850 247`) | 48-pin print-engine MCU — the target part, **confirmed on this board**. Package reads as UFQFPN48 in the close-ups; see the Rev K table above | high (sourced) |
| Large square BGA, center-left | **"DYMO"** printed on package, green orientation dot | **Network coprocessor SoC** — the built-in "LabelWriter Print Server" (runs the Linux-style TCP/IP/IPP/SNMP/HTTP OS found in the firmware dump). The 5XL / 550-Turbo have built-in LAN; D.mo's docs put that in a coprocessor. **Out of scope** for our USB-only firmware — ignore it. Part number not readable from the photo. | medium-high (inference) |
| Small chip, mid-board | `A8` / `1611` (week-11-2016 date code), swoosh logo | Unidentified — likely a power switch / MOSFET or small driver. Verify on hardware. | low |
| Small chip, lower-left | `310` / `1735` (week-35-2017 date code), same swoosh logo | Unidentified — likely a power switch / MOSFET or the motor driver. Verify on hardware. | low |

**What the photos do NOT give us:** individual GPIO trace routing, the exact
network-SoC part number, and the two small chips' identities. Those still need a
board probe (continuity from F072 pads to the head/motor/EEPROM) — see the
bring-up order below. The photos DO confirm the MCU part and give a reliable
component-location map for the fieldworker.

**Why no photo can supply the GPIO map:** full-res iFixit board photos of the
450-generation mainboard (a 450 Turbo board swaps into a 550 Turbo, so the family
is the same) confirm the layout and the D.mo-branded BGA network coprocessor, but —
critically — **D.mo's silkscreen carries only reference designators** (`U1`, `C4`,
`D1`, `JP2`…), never signal names. So even a perfectly sharp photo cannot tell us
which F072 pad is CLK vs DI vs STB, or which head-connector pin is which: that
information is simply not printed on the board. The official LW550 Tech Ref
documents board *connectors* (JP2 = DC power jack, RJ45 LAN) but not pin-level
routing; the FCC circuit diagram is confidential ("metadata only").

**Consequence for the fieldwork:** the definitive GPIO map comes from **continuity
probing on the physical board** (multimeter: F072 pad → head/motor/EEPROM line),
not from any photo. The *head side* is already well-constrained — it's a standard
ROHM KF3002-family module whose pin order (CLK, DI1, DI2, LAT, STB1/2, VH, VDD,
GND, TM) is in the head datasheet — so the fieldworker mainly needs to measure
which F072 pad reaches which head-connector pin. The photos' value is component
identification + layout, not wiring.

## F072CB 48-pin physical pin map (sourced)

The main MCU is an **STM32F072CB** — 48 pins, 128 K flash / 16 K RAM. This table
holds for **both** 48-pin packages: ST's Table 13 numbers LQFP48 and UFQFPN48 in
one shared column, so it does not matter which one your board carries. The
package has 12 pins per side; **pin 1 is the corner marked by the dimple/dot**,
numbered counter-clockwise from there: 1–12 down the left edge, 13–24 along the
bottom, 25–36 up the right edge, 37–48 along the top (the standard LQFP
convention used in the ST datasheet's package drawing — orient the part by the
dot, then follow the numbering counter-clockwise, and check that pad 7 lands on
NRST as a sanity check).

> **The STM32F0 has no JTAG.** Debug is SWD-only on PA13/PA14. PA15, PB3 and PB4
> are plain GPIOs after reset here — any "JTDI/JTDO/NJTRST by default" note you
> may remember from an STM32F1 board does not apply.

Complete physical pad → GPIO map, extracted from datasheet **DocID025004 Rev 2,
Table 13** ("STM32F072xx pin definitions"):

| Pad | Pin            | OpenDMOfw role (bold = assumed, to confirm) |
|-----|----------------|----------------------------------------------|
| 1   | VBAT           | battery backup                               |
| 2   | PC13           | tamper / RTC                                 |
| 3   | PC14-OSC32_IN  | 32.768 kHz in                                |
| 4   | PC15-OSC32_OUT | 32.768 kHz out                               |
| 5   | PF0-OSC_IN     | HSE in                                       |
| 6   | PF1-OSC_OUT    | HSE out                                      |
| 7   | NRST           | reset (active-low)                           |
| 8   | VSSA           | analog ground                                |
| 9   | VDDA           | analog supply                                |
| 10  | PA0-WKUP       | **paper sensor**                             |
| 11  | PA1            | **head thermistor (ADC_IN1)**                |
| 12  | PA2            | **status LED**                               |
| 13  | PA3            | **button**                                   |
| 14  | PA4            | **head LATCH**                               |
| 15  | PA5            | **head CLK**                                 |
| 16  | PA6            | **head DI1**                                 |
| 17  | PA7            | **head DI2**                                 |
| 18  | PB0            | **head STB1**                                |
| 19  | PB1            | **head STB2**                                |
| 20  | PB2            | head STB3 (spare)                            |
| 21  | PB10           | **motor ENABLE** (STEPDIR mode only)         |
| 22  | PB11           | —                                            |
| 23  | VSS            | GND                                          |
| 24  | VDD            | power                                        |
| 25  | PB12           | —                                            |
| 26  | PB13           | —                                            |
| 27  | PB14           | —                                            |
| 28  | PB15           | —                                            |
| 29  | PA8            | **head VH enable** (P-MOS, assumed active-low) |
| 30  | PA9            | —                                            |
| 31  | PA10           | —                                            |
| 32  | PA11           | **USB DM** (fixed)                           |
| 33  | PA12           | **USB DP** (fixed)                           |
| 34  | PA13           | **SWDIO** (SWD flash point)                  |
| 35  | VSS            | GND                                          |
| 36  | VDDIO2         | IO supply                                    |
| 37  | PA14           | **SWCLK** (SWD flash point)                  |
| 38  | PA15           | — (plain GPIO after reset)                   |
| 39  | PB3            | head STB4 (spare)                            |
| 40  | PB4            | **motor A1 / STEP**                          |
| 41  | PB5            | **motor A2 / DIR**                           |
| 42  | PB6            | **motor B1** (alt: I2C1_SCL pair)            |
| 43  | PB7            | **motor B2** (alt: I2C1_SDA pair)            |
| 44  | BOOT0          | boot config (Low = flash)                    |
| 45  | PB8            | **I2C1 SCL** (AF2)                           |
| 46  | PB9            | **I2C1 SDA** (AF2)                           |
| 47  | VSS            | GND                                          |
| 48  | VDD            | power                                        |

**SWD bring-up:** to flash over SWD, connect SWDIO→pad 34 (PA13), SWCLK→pad 37
(PA14), GND→any of pads 23/35/47, and 3.3 V→pad 24 or 48 (VDD). On the QFN these
are edge pads, not leads — find a via or a test point on each net rather than
clipping to the package. The chip boots
from flash by default (BOOT0 = pad 44 held Low), so no boot jumper is needed.

**I2C note:** on the STM32F0 line I2C is **AF2**, and I2C1 exists *only* on
PB6/PB7 or PB8/PB9 (Table 14). The board's EEPROM SCL/SDA must be traced to one
of those two pairs; `pins.h` currently assumes PB8/PB9.

> ⚠ **If the EEPROM turns out to be on PB6/PB7, there is a pin conflict.** The
> default motor mode (`MOTOR_DRIVE_4PHASE`) uses PB4/PB5/**PB6/PB7** as A1/A2/
> B1/B2. Moving I2C to PB6/PB7 means the motor phases must move too. There is
> room: PB10/PB11 are free (PB10 is only used as motor ENABLE in the STEPDIR
> fallback), as are PB12–PB15 and PA9/PA10/PA15. Resolve this **before** powering
> the head or the motor — driving a phase pin while the I2C pull-ups hold the
> line is the kind of mistake that takes a board with it.

## Overview

| Function           | Pin (assumed) | Alt-func / role         | Confidence | How to confirm |
|--------------------|---------------|-------------------------|------------|----------------|
| Head CLK           | PA5           | GPIO out (shift clock)  | medium     | Signal set sourced (KF3002 datasheet); routing assumed — follow the head-connector CLK trace |
| Head DI1           | PA6           | GPIO out (shift data, half 1) | medium | same, DI1 line |
| Head DI2           | PA7           | GPIO out (shift data, half 2) | medium | same, DI2 line |
| Head LATCH         | PA4           | GPIO out, Low = THROUGH (sourced) | medium-high | Scope: pulse just before the heat pulses |
| Head STROBE 1      | PB0           | GPIO out (STB1, half 1; active-low) | medium     | Scope: wide pulse that sets the dwell |
| Head STROBE 2      | PB1           | GPIO out (STB2, half 2) | medium-low | same |
| Head STROBE 3/4    | PB2 / PB3     | GPIO out (spare, wider heads) | low        | Only if the wide head has >2 heat lines |
| Paper sensor       | PA0           | GPIO in / or ADC        | low        | Reflection/transmission sensor; may be analog rather than digital |
| Head thermistor    | PA1           | ADC_IN1                 | medium     | Built into the head (TM pin, 30 kOhm B3950 NTC — sourced); measure the board's divider topology |
| Motor STEP         | PB4           | GPIO / TIM3_CH1 (AF1)   | low        | Driver IC likely a small dual-H-bridge (TB6612/MP6500 class) on 24 V; count µsteps/line by scoping the phase pins during one ESC D line |
| Motor DIR          | PB5           | GPIO                    | low        | same |
| Motor ENABLE       | PB10          | GPIO, active-low        | low        | STEPDIR mode only (PB8 is I2C SCL) |
| Motor 4-phase A1..B2 | PB4/5/6/7   | GPIO                    | low        | Only for direct phase drive (`MOTOR_DRIVE_4PHASE`) |
| I2C SCL            | PB8           | I2C1_SCL (AF2)          | medium     | I2C1 is AF2 and exists only on PB6 or PB8 (datasheet Table 14). Trace the EEPROM SCL to confirm PB8 vs PB6; config EEPROM + NFC front-end share this bus |
| I2C SDA            | PB9           | I2C1_SDA (AF2)          | medium     | valid only on PB7 or PB9 (Table 14); EEPROM @ 0x50, SLRC610 NFC front-end @ 0x28 (ignored by our firmware) |
| Head VH enable     | PA8           | GPIO, assumed active-low P-MOS | low | Trace the 24 V load-switch gate |
| Status LED         | PA2           | GPIO                    | low        | Follow the LED (not PC6 — unbonded on LQFP48) |
| Button (feed/power)| PA3           | GPIO in, pull-up        | low        | Follow the button (not PC7 — unbonded on LQFP48) |
| USB D+/D-          | PA12 / PA11   | fixed (USB peripheral)  | high       | Fixed on the F0; internal pull-up via `USB->BCDR` |

## Head geometry (per model, in `src/model.h`)

| Model            | Dots | dpi | Width | Bytes/line | Strobe segments |
|------------------|------|-----|-------|------------|-----------------|
| **OP104** (default) | 1248 | 300 | 105.7 mm | 156     | 2 (two 624-dot halves) |
| OP57 (`MODEL=OP57`) | 672 | 300 | 56.9 mm  | 84      | 2 (two 336-dot halves) |

- Both heads are two-half KF3002-family modules (sibling datasheet
  KF3002-GL50A); the halves are fired sequentially to split peak current.
  Confirm the half count on the board (a wide head could have 4 heat lines —
  spare strobe pins PB2/PB3 are already mapped).
- **Half-2 dot order is an assumption.** `head.c` feeds dot `i` to DI1 and dot
  `half + i` to DI2 on the same clock, i.e. both halves shift in the same
  direction from the centre outwards. Some two-half heads shift the second bank
  in the opposite direction. If a test print comes out with the right half
  mirrored, reverse the DI2 index in `head_print_line()` — that is the whole
  fix. Listed in FIELDWORK as measurement 6.
- `HEAD_BYTES = ceil(HEAD_DOTS/8)`. The line buffer is on the stack (2 KB reserve),
  so 156 bytes fits comfortably.
- Peak current is the critical point at OP104 (1248 dots @ 300 dpi): firing the
  two halves sequentially halves the peak; raise the dwell only after measuring
  the head temperature (compare against the datasheet Fig.3/Fig.4 curves).

## Bring-up order (safe)

1. **Writable F072 only** (stock chips are RDP2). Head connector disconnected.
   Flash, confirm the MCU boots and enumerates as a USB printer (`lsusb` shows
   `0922:002a` for 5XL / `0922:0028` for 550, `make MODEL=OP57`).
2. Scope the CLK/DI1/DI2/LAT/STB lines during a test print job; confirm the
   mapping in `pins.h` is right (and the STB polarity). Correct as needed.
3. Check the **thermistor divider direction** (`THERMAL_HOTTER_IS_HIGHER` in
   `thermal.c`) and the ADC thresholds against the 30 kOhm/B3950 curve + a known
   temperature.
4. Then connect the **head power**; start with a short dwell (`HEAD_BASE_DWELL_US`)
   and low density, raising it stepwise while watching the head temperature.
5. Calibrate `MOTOR_STEPS_PER_LINE` so one dot line advances exactly one line height
   (84.7 um at 300 dpi).
