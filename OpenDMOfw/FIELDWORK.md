# FIELDWORK — OpenDMOfw (what a board owner needs to measure)

**Status.** The firmware is complete and builds clean for both models
(`make` → 5XL, `make MODEL=OP57` → 550). Every software-testable layer is done and
verified. The **only** remaining work is hardware verification + calibration on a
genuine LabelWriter 550/5XL mainboard (STM32F072CBT6). Everything listed below is
currently an *assumption* in `src/pins.h` / `store.c`; measure it, patch the values,
and the firmware is finished.

## How to report — email your findings

📧 **opendymofw@secret.fyi** (active for ~1 month from posting)

If you have a board and can spare an hour or two (it's not much work — a handful of
continuity checks plus a couple of readings), **email your results to
`opendymofw@secret.fyi`** — that is how this project gets its fieldwork. The most useful
report is the filled-in "GPIO routing" table above: for each signal, which F072 pad it
reaches (with the continuity reading), plus the answers to the "Other measurements"
list. A short prose description or a photo of each measurement is fine — **you do not
need to write any code**.

A pull request that updates `src/pins.h` (and `store.c` if the I2C pair differs) is a
welcome bonus, but the email report is the essential part.

## What you need

- A genuine **LabelWriter 550 or 5XL**, opened, mainboard exposed.
- A **multimeter** (continuity + DC voltage). A logic analyzer or scope is a big plus.
- The `src/pins.h` file (this repo) to compare against while you measure.

## What is ALREADY verified (don't redo these)

- **MCU = STM32F072CBT6**, LQFP48, 128 K flash / 16 K RAM — confirmed on the Rev K motherboard.
- **Complete physical pin map** (pad → GPIO), extracted from datasheet Table 13 — see
  `PINMAP.md`, section "F072CBT6 LQFP48 physical pin map". Use it to find each pad.
- **Head interface** = ROHM KF3002-family module: built-in shift registers + latch +
  heat drivers; signals CLK, DI1/DI2, LAT (High=HOLD/Low=THROUGH), STB1/STB2 (active-low),
  VH (24 V), VDD (3.3 V), TM (built-in NTC 30 kΩ B=3950). No MISO. Sourced from the head
  datasheet — only the *board routing* is unknown.
- **I2C = AF2**, and I2C1 exists only on PB6/PB7 or PB8/PB9 (datasheet Table 14).
- **USB identity** (VID `0x0922`, PID `0x002A`/`0x0028`, IEEE-1284 device ID) and the
  **full wire protocol** — sourced and byte-verified against real captures.
- **SWD flash points:** SWDIO = PA13 (pad 34), SWCLK = PA14 (pad 37); GND = any VSS pad
  (23/35/47); 3.3 V = a VDD pad (24/48). Boots from flash by default (BOOT0 pad 44 Low).

## The one thing that matters most: GPIO routing

The head/motor/sensor signals are plain GPIOs; the firmware bit-bangs them. The assumed
routing (in `src/pins.h`) is:

| Signal | Assumed pin | Pad | What to measure |
|--------|-------------|-----|-----------------|
| Head CLK   | PA5 | 15 | Continuity from pad 15 → which head-flex pin? |
| Head DI1   | PA6 | 16 | same |
| Head DI2   | PA7 | 17 | same |
| Head LAT   | PA4 | 14 | same (Low = THROUGH) |
| Head STB1  | PB0 | 18 | same (active-low heat strobe, half 1) |
| Head STB2  | PB1 | 19 | same (half 2) |
| Motor A1/STEP | PB4 | 40 | which motor-driver input does pad 40 reach? |
| Motor A2/DIR  | PB5 | 41 | same |
| Motor B1   | PB6 | 42 | same |
| Motor B2   | PB7 | 43 | same |
| Thermistor | PA1 | 11 | is the NTC divider on pad 11? (ADC_IN1) |
| Paper sensor | PA0 | 10 | is the paper sensor on pad 10? digital or analog? |
| I2C SCL    | PB8 | 45 | confirm PB8 vs PB6 (SCL); trace EEPROM SCL |
| I2C SDA    | PB9 | 46 | confirm PB9 vs PB7 (SDA); trace EEPROM SDA |

**Method:** with the board unpowered, use continuity mode between each F072 pad and
the pins of the **head flex connector**, the **motor-driver IC**, and the **EEPROM**.
The head side is already constrained by the ROHM pin order (CLK, DI1, DI2, LAT, STB1/2,
VH, VDD, GND, TM), so matching "which F072 pad reaches which flex pin" gives you the
full map.

## Other measurements

1. **STB polarity** — confirm the heat strobe is active-low (Low fires the driver). A
   single scope shot of a test print settles it; if wrong, flip one line in `head.c`.
2. **DI1/DI2 topology** — are the two shift-data lines driven **in parallel** (assumed)
   or daisy-chained? (The head supports either.)
3. **Motor driver** — identify the IC (likely a small dual-H-bridge, TB6612/MP6500 class,
   or a discrete 4-transistor bridge on 24 V). Confirm it's driven **IN1–IN4 directly**
   (assumed) vs STEP/DIR. Then **count µsteps per line**: scope the phase pins during one
   `ESC D` line / one label feed, and set `MOTOR_STEPS_PER_LINE` in `motor.c` so one dot
   line advances exactly 1/300 inch (0.08467 mm).
4. **Thermistor divider** — find the pull resistor R_p next to the NTC and its direction
   (pull-up vs pull-down). One ADC reading at a known temperature (25 °C ≈ 30 kΩ) pins
   both R_p and `THERMAL_HOTTER_IS_HIGHER` in `thermal.c`.
5. **VH voltage** — confirm the head heat supply is 24 V (assumed, via a P-MOS/load
   switch from the 24 V brick).
6. **EEPROM** — confirm it's a **BL24C128A @ 0x50** (rev K board), check the WP pin state,
   and verify writes are accepted (the `GS D 0x03` self-test does a write+read round-trip).

## On-board bring-up (safe order)

1. Flash over SWD with the **head connector disconnected**; confirm it enumerates as
   `0922:002a` (5XL) / `0922:0028` (550).
2. Scope the CLK/DI/LAT/STB lines during a test job; confirm the `pins.h` mapping + STB
   polarity; correct as needed.
3. Check the thermistor divider direction + ADC thresholds against the 30 kΩ/B3950 curve.
4. Connect head power; start with a short dwell (`HEAD_BASE_DWELL_US`) and low density,
   raising stepwise while watching head temperature (datasheet Fig.3/Fig.4 curves).
5. Calibrate `MOTOR_STEPS_PER_LINE` so one line advances exactly one line height.

The firmware ships a **`GS D` diagnostic backdoor** (fire head / step motor / read
EEPROM + GPIOs with no vendor driver) specifically for this bring-up — see `PROTOCOL.md`.
