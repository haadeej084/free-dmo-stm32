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
| A7 | Host unit test of the parser (mocked hardware) — compiled and run natively: 37 checks / 23 scenarios, both models | OK |
| B8 | Host sender `tools/opsend.py` (genuine D.mo protocol via libusb, PNG→raster) | OK |

## Label counter (D.mo-like, as requested)

Counts down per **printed label** (not per dot line) and is written persistently to
EEPROM, so the value survives a power cycle — a D.mo-like roll counter **without** any
tag / DRM / authentication.

## Build result

- **OP104** (default, 5XL-class 101 mm / 300 dpi) and **OP57** (550, 57 mm): build clean,
  flashable `.bin` per model.
- Identity: cloned genuine D.mo — VID `0x0922`, PID `0x002A` (5XL) / `0x0028` (550),
  `DYMO` manufacturer + per-model product strings.
- Flash ~6.5%, RAM ~35% — well within budget (128 K / 16 K).
- Parser test: 37 checks pass for both models; sender byte-matched to the decompiled driver.

## Docs

`README.md` (DRM framing), `PROTOCOL.md` (wire protocol + ESC U/V records + GS D),
`FIELDWORK.md`, `BUILD.md`, `PINMAP.md`, `DECISIONS.md`.

## Remaining: hardware bring-up (cannot be done in software; needs a board)

GPIO pin routing (including VH enable), LATCH/STROBE polarity/timing, heat-segment
count, dwell/density calibration, motor steps/line, I2C TIMINGR + EEPROM WP,
thermistor direction/curve, and USB PMA/EP verification on silicon — per point in
`PINMAP.md` / `DECISIONS.md`. Stock F072 parts are RDP2 and cannot be flashed over
SWD until RDP is lowered. The `GS D` diagnostic backdoor reports the raw values to
tune against on the board. See `FIELDWORK.md` for exactly what to measure.
