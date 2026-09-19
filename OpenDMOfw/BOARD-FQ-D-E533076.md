# BOARD — the owner's LabelWriter 550 mainboard, silkscreen `FQ-D E533076`

What was read off one physical unit on 19 Sep 2026, what it changed in the
firmware, and how to get the remaining answers out of the board *while its stock
MCU still drives it*. This is the board-specific companion to `FIELDWORK.md`
(the generic bench list), `PINMAP.md` (pad → GPIO tables; its "Board component
map" section holds the photo observations from these same 4080×3060 photos and
calls the board "Rev K") and `DECISIONS.md` D43/D44 (why).

**Sources.** Two top-side photos (not bundled), crops at native resolution to
read packages, and component markings read by the owner under magnification —
the black QFN/SOIC parts are not legible from the photos. Where a reading was
ambiguous it is said so.

---

## 1. Identification

| Item | Value | How known |
|------|-------|-----------|
| PCB | `FQ-D E533076`, `94V-0`, UL/CQC marks, date code `2614` (wk 14 2026) | silkscreen |
| Unit | **LabelWriter 550, 57 mm** — the default `make` | head label `3C56-9638` = the KF3002-GK11C 672-dot head (D16) |
| Head | `3056-9638 2604 09530558` → SHEC **3C56-9638**, date 2604, serial 09530558 | label (`0` read for `C`) |
| Motor | `LEILI 35BY412-339`, `IBN 60417-5041`, `No.: 20227` — 7.5°, 48 steps/rev (D24) | label |
| MCU | STM32F072 **C8U6 or CBU6** (UFQFPN48); fifth character faint | owner's reading of U1 |

Newer than the Rev K eevblog documents (2022): same UFQFPN48 MCU package, and the
U2 stepper-driver change eevblog reports between Rev H and Rev K is present.

---

## 2. Parts read

| Ref | Package | Read as | Part | Role | Confidence |
|-----|---------|---------|------|------|------------|
| **U1** | 48-pad QFN, 12/side, 0.5 mm, no leads | `STM32F072C8?? … U6` | STM32F072**C8U6** / **CBU6** | MCU — linker is 64 K so either runs; replacement **`STM32F072CBU6`** | high; C8/CB open |
| **U2** | TSSOP-28 EP | `SOM42630` | **SGM42630** (SGMICRO) | stepper driver, **STEP/DIR** indexer, 8–35 V, 2.6 A/winding → `MOTOR_DRIVE_STEPDIR` (D43) | high |
| **Q6** | SO-8 | `4459` | **Si4459ADY** (Vishay) P-ch 30 V | **VH 24 V load switch** to the head | high |
| **Q3** | DPAK | `D4130 BL6B1A` | **AOD4130** (AOS) N-ch 60 V | low-side switch — likely Q6's gate driver (then `HEAD_VH_ON_LEVEL` = 1) | medium |
| **D4** | SMC | `SMCJ4A` (digit lost) | SMCJ24A (or SMCJ33A) | TVS on the 24 V input | medium |
| **D5** | — | `K51QQA 533LJ` | not identified | — | — |
| **U5** | SOIC-8 beside C18/C37 (PINMAP's Rev K table names it "U6") | not legible | BL24C128A per Rev H/I/K | config EEPROM @ 0x50; WP pin 7 | medium-high |
| **Y1** | HC-49S | `AXC12.00-115` (PINMAP) | 12 MHz | HSE; the firmware runs HSI48+CRS by default (D3) | high |
| **J2** | 6-pin 0.1", unpopulated, pin 1 square, beside SW2 / USB-B | — | — | **SWD header** (candidate) — flash the replacement QFN here; verify to pads 34/37/7 | medium-high |
| **J4, J5** | 2×5 shrouded | — | — | motor + sensor harnesses | medium |
| **J6** | FFC, right edge | — | — | head flex (CLK, DI, LAT, STB, VH, TM) | high |
| **J7** | 6-pin JST | — | — | sensor / button-LED board | low |
| R7–R12, R18–R27, R29, R31–R33, R39 | 0603 | `220` | **22 Ω** | series damping on the head/motor lines — the accessible probe points (PINMAP) | high |
| R51, R53 | 0603 | `473` | 47 kΩ | the SGM42630 reference schematic's logic-input pull value | high |
| R109 | 0603 | `01C` | 10 kΩ | VREF divider class (reference: 10 k / 10 k) | medium |
| Q1, U3, Q2, Q7, Q9, Q10, D2, D11 | SOT-223 / SOT-23 / SOD | — | — | regulator, VBUS detect, small switches, flyback | low |

---

## 3. What this changed in the firmware

| Finding | Change | Where |
|---------|--------|-------|
| U2 = SGM42630 | `MOTOR_DRIVE` → **`MOTOR_DRIVE_STEPDIR`** (D40's condition met); 4-phase kept, still built and tested (`-DMOTOR_DRIVE=1`) | `src/pins.h`, D43 |
| SGM42630 nSLEEP pulls **down** | optional **`PIN_MOTOR_SLEEP`**: `motor_enable(1)` raises it and waits tWAKE = 1 ms before the first STEP; `motor_enable(0)` and `Fault_Handler` drop it; compiles away when undefined | `pins.h`, `motor.c`, `startup.c` |
| SGM42630 timing / order | `test_motor.c` asserts, in STEP/DIR mode: ENABLE low before the first rising edge, SLEEP high ≥ 1 ms before it, every pulse ≥ 1 µs high and low, exactly one rising edge per line; `make test` runs it plain, with nSLEEP, and 4-phase | `test/test_motor.c`, `Makefile` |
| SGM42630 does 1–⅛ only | `MOTOR_STEPS_PER_LINE` ∈ {1,2,4,8} × full steps/line — the 450's 12 is not reproducible here; `1` stays until measured | `motor.c` comment, D43 |
| Q6 / Q3 named | measurement 5 now says which gate to trace, and that the polarity may be the inverse of `pins.h` | `FIELDWORK.md` 8.5, D44 |
| Stock board as probe | FIELDWORK 8 item **0b** | `FIELDWORK.md`, D44 |

Unchanged on purpose: the 64 K linker (already in place for the C8/CB doubt), the
pad assignments (all still *assumed* until measurement 1), `MOTOR_STEPS_PER_LINE 1`.

---

## 4. SGM42630 reference (SGMICRO datasheet, Dec 2024 Rev B.1)

TSSOP-28, top view, pin 1 top-left, counter-clockwise.

| Pin | Name | Dir | Note | Pin | Name | Dir | Note |
|----:|------|-----|------|----:|------|-----|------|
| 1 | ISENA | — | bridge A sense → R_sense → PGND | 28 | VMA | — | 24 V |
| 2 | nHOME | O | low at indexer home state | 27 | **nSLEEP** | I | **High = awake**; internal pull-**down** |
| 3 | **DIR** | I | internal pull-down | 26 | **nENABLE** | I | **Low = outputs on**; internal pull-**up** |
| 4 | AOUT1 | O | winding A+ | 25 | AOUT2 | O | winding A− |
| 5 | DECAY | I | fast / mixed / slow by voltage | 24 | CP2 | IO | charge pump |
| 6 | RCA | I | R‖C blanking/off-time, A | 23 | CP1 | IO | charge pump |
| 7 | GND | — | | 22 | VCP | IO | HS gate supply |
| 8 | VREF | I | full-scale current | 21 | GND | — | |
| 9 | RCB | I | R‖C, B | 20 | VGD | IO | LS gate supply |
| 10 | VCC | — | 3–5.5 V logic | 19 | **STEP** | I | **rising edge**; internal pull-down |
| 11 | BOUT1 | O | winding B+ | 18 | BOUT2 | O | winding B− |
| 12 | **USM1** | I | µstep select; pull-down | 17 | **nRESET** | I | Low = reset; internal pull-**up** |
| 13 | **USM0** | I | µstep select; pull-down | 16 | nSR | I | floating = auto-decay |
| 14 | ISENB | — | bridge B sense | 15 | VMB | — | 24 V |
| EP | GND | | | | | | |

V_IL ≤ 0.2·VCC, V_IH ≥ 0.8·VCC, internal pulls 270 kΩ.

| USM1 | USM0 | Step |
|------|------|------|
| 0 | 0 | full |
| 0 | 1 | ½ |
| 1 | 0 | ¼ |
| 1 | 1 | ⅛ |

| Timing | Datasheet | Firmware |
|--------|-----------|----------|
| STEP high / low, min | 1 µs each | half the step period each (`step_pulse`) |
| DIR set-up before STEP↑ | 250 ns | DIR static from `motor_init` |
| f_STEP max | 500 kHz | ≤ ~9 kHz even at ⅛ |
| **tWAKE**, nSLEEP↑ → STEP accepted | ≤ 1 ms | `driver_wake()` waits 1 ms |
| tnENABLE, nENABLE↓ → outputs | ≤ 20 µs | first edge ≥ one step period later |
| tnRESETR | ≤ 5 µs | nRESET assumed strapped high |

Current is fixed by VREF and the sense resistors; the firmware cannot over-drive
the winding on this path.

---

## 5. The feed constant

One raster line = 1/300 in = 0.08467 mm. With this driver:

```
MOTOR_STEPS_PER_LINE = µstep_divisor (1|2|4|8) × full_steps_per_line
```

D24's estimate is **one full step per line** from the rated 62 labels/min, and the
motor has no gearbox of its own (the catalog 35BY412's 1:42.5 head is absent on the
`-339`); whatever reduction exists is in the printer's gear train. So the constant
is 1, 2, 4 or 8 — and it is *read*, not calibrated: the USM strapping gives the
divisor, or one STEP-pulse count on U2 pin 19 during a stock print gives the whole
number. The 25.4 mm / 300 lines feed test (FIELDWORK 8.2) remains the acceptance
check afterwards.

---

## 6. Getting the rest out of the stock board (FIELDWORK 8 item 0b)

The stock MCU is RDP2 and stays where it is until the swap; until then it is the
one thing on the bench that *knows* the routing. Power the board, print one label,
and only listen — clips on the far ends of the traces, never on the QFN.

| Probe | Settles | Closes |
|-------|---------|--------|
| U2 pin 19 STEP | pulses per line | `MOTOR_STEPS_PER_LINE` |
| U2 pin 3 DIR | forward level | `motor_init` DIR value |
| U2 pins 27 / 26 / 17 | which of nSLEEP / nENABLE / nRESET move vs sit at a rail | whether `PIN_MOTOR_SLEEP` / `PIN_MOTOR_ENABLE` are needed |
| U2 pins 12 / 13 | USM levels | µstep divisor |
| Q3 gate, Q6 gate, VH at J6 | polarity and timing of the VH switch | `PIN_HEAD_VH`, `HEAD_VH_ON_LEVEL` (D31 warning applies if inverted) |
| 22 Ω pads toward J6 | CLK count per line, data-pin count, STB idle level and pulse direction, LAT | 5b, 6b, D36 |
| U5 pins 5 / 6 | I2C traffic; follow the trace | `PIN_I2C_SCL/SDA` (PB6/7 vs PB8/9) |
| J4 / J5 / J7 | which harness is which | motor / photocell / buttons |

Then, unpowered, one continuity check per 22 Ω pad to the MCU side finishes
measurement 1 — knowing which signal you are looking for. Not obtainable this way:
C8 vs CB, and the head's Po (item 0's current probe).

---

## 7. For the swap

| Item | Spec |
|------|------|
| MCU | **STM32F072CBU6** (UFQFPN48; C8U6 also runs the 64 K image) |
| Probe | ST-Link V2 or any CMSIS-DAP, on **J2** once its pinout is confirmed |
| Flash tool | `st-flash`, OpenOCD or STM32CubeProgrammer — none on the build PC yet |
| Rework | hot air, flux, wick; Kapton over Y1/U5. Exposed pad — not an iron job |

Then `make` → `st-flash write build/OP57/opendmo-OP57.bin 0x08000000` → USB only →
`0922:0028` → `tools/opsend.py status` / `diag` before the head goes on (BUILD.md,
FIELDWORK 5–7).

---

## 8. Still open

| Item | Blocks | Close by |
|------|--------|----------|
| U1 C8 vs CB | nothing | isopropanol drop + raking light + phone macro; or just fit a CBU6 |
| D5 | nothing | a legible photo |
| Q1 / U3 | only if Q3 is not Q6's driver | markings |
| J6 contact count | 5b | count them |
| J2 pinout | flashing | continuity to pads 34 / 37 / 7 / VSS / VDD |
| Bottom-side photo under U1 / J2 / U2 | most of §6 | one photo |

## 9. Reading log

| Date | Item | Reading | Resolved to |
|------|------|---------|-------------|
| 19 Sep 2026 | U1 | `STM32F072C8** (U6?) 7858C ZT** PHL 78` | STM32F072C8U6 / CBU6 |
| 19 Sep 2026 | U2 | `SOM42630` | SGM42630 |
| 19 Sep 2026 | Q6 | `4459` | Si4459ADY |
| 19 Sep 2026 | Q3 | `D4130 BL6B1A` | AOD4130 |
| 19 Sep 2026 | D4 | `SMCJ4A` | SMCJ24A (probable) |
| 19 Sep 2026 | D5 | `K51QQA 533LJ` | — |
| 19 Sep 2026 | motor | `LEILI 35BY412-339 IBN 60417-5041 No.: 20227` | 35BY412, 48 steps/rev |
| 19 Sep 2026 | head | `3056-9638 2604 09530558` | SHEC 3C56-9638 |

References: SGM42630 datasheet (SGMICRO, Dec 2024 Rev B.1); Vishay Si4459ADY doc
69979; AOS AOD4130; Leili 35BY412 product page; eevblog "Dymo 550 Thermal Printer
DRM Hacking" (Rev H→K notes, BL24C128A).
