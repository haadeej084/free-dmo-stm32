# OpenDMOfw — delivery summary

All robustness points (A1–A7) plus the host-side sender are implemented and
validated — everything up to (but not including) the physical board.

## What it is now

OpenDMOfw deliberately **clones** the genuine D.mo LabelWriter 550 / 5XL: real USB
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
| A7 | Host unit test of the parser (mocked hardware) — compiled and run natively: 111 checks / 49 scenarios, both models | OK |
| B8 | Host sender `tools/opsend.py` (genuine D.mo protocol via libusb, PNG→raster) | OK |

## Label counter (D.mo-like, as requested)

Counts down per **printed label** (not per dot line) and is written persistently to
EEPROM, so the value survives a power cycle — a D.mo-like roll counter **without** any
tag / DRM / authentication.

## Build result

- **OP104** (default, 5XL-class 1248 dots / 300 dpi) and **OP57** (550, 672 dots):
  build clean, flashable `.bin` per model.
- Identity: cloned genuine D.mo — VID `0x0922`, PID `0x002A` (5XL) / `0x0028` (550),
  `DYMO` manufacturer + per-model product strings.
- Flash ~8.6%, RAM ~35% — well within budget (128 K / 16 K).
- Parser test: 111 checks pass for both models; sender byte-matched to the decompiled driver.
- PC-side patcher: 27 offline checks on a synthetic assembly (no vendor DLL needed).

## Docs

`README.md` (DRM framing), `PROTOCOL.md` (wire protocol + ESC U/V records + GS D),
`FIELDWORK.md` (what a board owner measures, with a fill-in report template),
`BUILD.md`, `PINMAP.md`, `DECISIONS.md`, `pc-patch/README.md`.

## Remaining: hardware bring-up (cannot be done in software; needs a board)

Narrowed to **seven** measurements — see `FIELDWORK.md` section 8. Everything
that could be settled from the datasheets has been: STB polarity, DI1/DI2
topology, strobe-segment count and the EEPROM part are no longer open questions,
the NTC curve and the divider lookup are tabulated so two readings finish the
thermal calibration, and the silicon narrows the thermistor search to 10 pads and
I2C to 4. What is left is the GPIO routing, the motor drive train, the divider
resistor, the paper-sensor type, the VH enable pin, the half-2 dot order, and
host acceptance.

GPIO pin routing (including VH enable), LATCH/STROBE polarity/timing, heat-segment
count, dwell/density calibration, motor steps/line, I2C TIMINGR + EEPROM WP,
thermistor direction/curve, and USB PMA/EP verification on silicon — per point in
`PINMAP.md` / `DECISIONS.md`. Stock F072 parts are RDP2 and cannot be flashed over
SWD until RDP is lowered. The `GS D` diagnostic backdoor reports the raw values to
tune against on the board. See `FIELDWORK.md` for exactly what to measure.
