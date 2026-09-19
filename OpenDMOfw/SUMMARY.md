# OpenDMOfw — delivery summary

All robustness points (A1–A7) plus the host-side sender are implemented and
validated — everything up to (but not including) the physical board.

## What it is now

OpenDMOfw deliberately **clones** the genuine D.mo LabelWriter 550 (STM32F072
board; the 5XL and 550 Turbo use an STM32F407, DECISIONS D25): real USB
identity (VID `0x0922`, per-model PID), real wire protocol, real roll-state semantics —
so stock D.MO Connect enumerates it unchanged and any physical roll prints. See
`README.md` for the 3-layer DRM framing and `DECISIONS.md` D1/D2.

## Implemented and verified

| # | Point | Status |
|---|-------|--------|
| A1 | EEPROM page-wise writes (page-wrap bug fixed) | OK |
| A2 | Resumable byte-driven raster FSM (survives ring-buffer underflow) | OK |
| A3 | USB endpoint-HALT (GET_STATUS / SET / CLEAR_FEATURE + DTOG reset) | OK |
| A4 | Head strobe for N segments via a pin array | OK |
| A5 | Print-density command (genuine `ESC C` / `ESC e`) | OK |
| A6 | IWDG watchdog (per-line kick) + LED fault patterns + unique serial from the MCU UID | OK |
| A7 | Host unit test of the parser (mocked hardware) — compiled and run natively: 179 checks, both models | OK |
| B8 | Host sender `tools/opsend.py` (genuine D.mo protocol via libusb, PNG→raster) | OK |
| C1 | USB stack host test against a register-level peripheral model — 127 checks per model | OK |
| C2 | Real image in Renode: boot, SysTick, LED patterns, head bit stream + per-line cost, config EEPROM on both known parts, and the fault safe state (four triggers: HardFault, NMI, an unused vector, and a fault before `SystemInit()`) | OK |
| C3 | Worst-case stack (936 OP104 / 792 OP57 of 2048 bytes) and static analysis (GCC analyzer, cppcheck) | OK |
| C4 | End-to-end host test: USB stack + parser, a full job through 64-byte packets with flow control — 29 checks per model | OK |
| C5 | USB DFU entry (`GS D 0x09 'D' 'F' 'U'`): reflash over USB with `dfu-util` after the first SWD flash; request and hand-over checked in Renode | OK |

## Label counter (D.mo-like, as requested)

Counts down per **printed label** (not per dot line) and is written persistently to
EEPROM, so the value survives a power cycle — a D.mo-like roll counter **without** any
tag / DRM / authentication.

## Build result

- **OP57** (default, 550, 672 dots) and **OP104** (4" geometry, 1248 dots / 300 dpi):
  build clean. OP57 is the image with a real board; OP104 waits for an F407 port.
- Identity: cloned genuine D.mo — VID `0x0922`, PID `0x002A` (5XL) / `0x0028` (550),
  `DYMO` manufacturer + per-model product strings.
- Flash ~11.7 KB of the 64 KB the linker allows (fits the F072C8 and CB), RAM ~35 % of 16 KB.
- Parser test: 179 checks pass for both models; sender byte-matched to the decompiled driver.
- Thermal/energy test: 51 checks per model, built TWICE — the second build arms
  `HEAD_SAG_FULL_US` (53 checks) so the energy ceiling is exercised rather than
  asserted against zero.
- Motor test: 12 checks per model. It asserts the ORDER of the phase writes, not
  the final pin state, because a coil shorted only between two writes is exactly
  what a final-state test cannot see.
- System test: 18 checks per model on the real `sys_pin_toggle()` hot-pin guard.
- Config store: 28 scenarios per model against a register-level I2C EEPROM model
  (`test/i2c_eeprom_model.h`), including the last-byte NAK, the checksum and
  the I2C1 alternate-function number (AF1 on the F072, D38).
- PC-side patcher: 27 offline checks on a synthetic assembly (no vendor DLL needed).

## Docs

`README.md` (DRM framing), `PROTOCOL.md` (wire protocol + ESC U/V records + GS D),
`FIELDWORK.md` (what a board owner measures, with a fill-in report template),
`BUILD.md`, `PINMAP.md`, `DECISIONS.md`, `pc-patch/README.md`,
`BOARD-FQ-D-E533076.md` (the owner's 550 board: U2 = SGM42630, Q6 = Si4459, J2, the
stock-board probe plan).

## Remaining: hardware bring-up (cannot be done in software; needs a board)

Narrowed to **nine** measurements — see `FIELDWORK.md` section 8, which opens
with the one 450-board capture that answers six of them at once. The count was
seven until DECISIONS D28 withdrew the strobe-polarity claim and put it back on
the list as measurement 6b, paired with the VH gate polarity (measurement 5);
D35/D36 then added the flex pin count (5b) and the head's Po (7); host
acceptance moved out of the list into bring-up step C, where it belongs.
Much of what could be settled from the datasheets and the vendor firmware has
been: the head data topology (one line, 672 clocks — D36),
the strobe-segment count and the EEPROM part are no longer open questions, the
NTC curve and the divider lookup are tabulated so two readings finish the
thermal calibration, and the silicon narrows the thermistor search to 10 pads and
I2C to 4. **STB polarity is NOT among them** — this paragraph used to list it as
settled three lines after the sentence above says D28 withdrew it. What is left
is the GPIO routing, the motor drive train, the divider resistor, the
paper-sensor pad and direction (4; its type is analog, from the 450 - D42), the
VH enable pin (5), the flex pin count (5b), the strobe polarity (6b, corroborated
by the 450 and answered by the capture), the half-2 dot order (6, only if 5b
finds two data pins) and Po at the head (7) — nine, matching the count above.

GPIO pin routing (the VH switch is now named: Q6 = Si4459, D44), LATCH/STROBE
polarity/timing, heat-segment count, dwell/density calibration, motor µsteps/line
(driver now known: SGM42630, STEP/DIR, D43), I2C TIMINGR + EEPROM WP,
thermistor direction/curve, and USB PMA/EP verification on silicon — per point in
`PINMAP.md` / `DECISIONS.md`. The image goes onto a blank F072CB fitted in place
of the stock part: the stock MCU is RDP2, which is permanent, so the genuine
boards serve as probes only. The `GS D` diagnostic backdoor reports the raw values to
tune against on the board. See `FIELDWORK.md` for exactly what to measure.
