#!/usr/bin/env python3
"""Find the board's pin routing by watching the printer, not by buzzing it out.

FIELDWORK measurement 1 asks for continuity tracing of nineteen nets from the
MCU pads to the head flex, the motor driver, the sensors and the EEPROM. That is
the single biggest block of bench time in the project. **Eight of those nineteen
do not need a meter at all**, because the firmware can already read every input
pin and drive every safe output pin, and the mechanism itself is the indicator.

This script drives those two diagnostics and works the map out:

  INPUTS  - GS D 0x06 returns all ten ADC channels and the input register of
            ports A, B and C in one 29-byte reply. Take a scan, change one thing
            in the physical world, take another, and diff. The bit that moved IS
            the pin.
              paper sensor  : block / unblock it
              button        : press it
              thermistor    : warm the head with a hand or a hairdryer on low
  OUTPUTS - GS D 0x07 toggles a candidate pin n times and restores it. Watch the
            mechanism. It REFUSES the pins that carry heat (the VH gate and every
            fitted strobe) and the USB/SWD pins, so an automated sweep cannot
            damage anything or end the session.
              motor phases  : the motor twitches or sings
              status LED    : the LED blinks

WHAT THIS DOES NOT FIND, and why - so nobody thinks the job is done:
  The head's six logic lines (CLK, DI1, DI2, LAT, STB1, STB2) and the VH gate.
  The head is a write-only shift register with no serial output (DECISIONS D30
  confirms: "No MISO"), so there is nothing to read back and nothing to watch
  that does not involve heat. Those seven stay on the meter. The I2C pair is
  found by store.c's own detection ladder at boot, reported by GS D 0x03/0x04.

SAFETY: every command this sends is a diagnostic. It never prints, never feeds,
and the firmware refuses to toggle a pin that can put energy into the head. Run
it with OP_FLAG_VH_INHIBIT set anyway (`opsend.py vh off`) - belt and braces
costs nothing.

USAGE
    python3 tools/discover_pins.py inputs     # guided: diff scans as you act
    python3 tools/discover_pins.py outputs    # sweep safe pins, you watch
    python3 tools/discover_pins.py all
"""
import argparse
import sys
import time

PORTS = "ABC"


def _opsend():
    """Reuse opsend.py's transport rather than duplicating it."""
    import importlib.util
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("opsend", os.path.join(here, "opsend.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def scan(op, dev):
    """One GS D 0x06: ten ADC channels and the three port input registers."""
    dev.write(op.EP_OUT, bytes([0x1D, ord("D"), 0x06]), timeout=1000)
    r = op.read_diag(dev)
    d = op.parse_diag(r)
    if "idr" not in d:
        raise SystemExit(f"GS D 0x06 did not decode: {d.get('raw')}")
    return d


def diff_scan(a, b):
    """Which pins and which ADC channels moved between two scans."""
    pins, adc = [], []
    for p in PORTS:
        x, y = a["idr"][p], b["idr"][p]
        for bit in range(16):
            if ((x >> bit) & 1) != ((y >> bit) & 1):
                pins.append(f"P{p}{bit}  ({(x >> bit) & 1} -> {(y >> bit) & 1})")
    for i, (x, y) in enumerate(zip(a["adc"], b["adc"])):
        if abs(x - y) > 40:            # 1 % of full scale: noise floor
            adc.append(f"ADC_IN{i}  ({x} -> {y}, delta {y - x:+d})")
    return pins, adc


def do_inputs(op, dev):
    steps = [
        ("PAPER SENSOR",
         "Take the roll OUT of the sensor path (or block it with a finger)",
         "the paper-present pin"),
        ("BUTTON",
         "PRESS AND HOLD the front-panel button",
         "the button pin"),
        ("THERMISTOR",
         "Warm the print head - a hand on it for 30 s is enough",
         "the thermistor ADC channel"),
    ]
    found = {}
    for name, action, what in steps:
        print(f"\n=== {name} ===")
        input("  Put the printer in its RESTING state, then press Enter...")
        before = scan(op, dev)
        print(f"  {action}")
        input("  ...then press Enter while it is still like that...")
        after = scan(op, dev)
        pins, adc = diff_scan(before, after)
        if not pins and not adc:
            print(f"  NOTHING MOVED. Either the action did not take, or {what}")
            print("  is not on a pin this firmware reads. Try again, or note it for the meter.")
            continue
        for p in pins:
            print(f"  -> {p}")
        for a in adc:
            print(f"  -> {a}")
        if len(pins) + len(adc) == 1:
            found[name] = (pins + adc)[0]
            print(f"  UNAMBIGUOUS: that is {what}.")
        else:
            print(f"  More than one thing moved. Repeat it - whatever moves BOTH times")
            print(f"  is {what}; the rest is noise or a coincidence.")
    return found


def do_outputs(op, dev, args):
    print("\n=== OUTPUT SWEEP ===")
    print("The firmware refuses the heat pins and the USB/SWD pins, so this is safe.")
    print("Watch the MOTOR and the LED. Say what moved when.\n")
    hits = {}
    for port_i, port in enumerate(PORTS):
        for pin in range(16):
            dev.write(op.EP_OUT,
                      bytes([0x1D, ord("D"), 0x07, port_i, pin, args.pulses]),
                      timeout=1000)
            r = op.read_diag(dev)
            d = op.parse_diag(r)
            ok = d.get("raw", "")[4:6] == "01" if "raw" in d else False
            if not ok:
                continue                      # refused: a heat, USB or SWD pin
            ans = input(f"  P{port}{pin:<2} toggled {args.pulses}x - anything? "
                        f"[enter=no / m=motor / l=LED / other text] ").strip()
            if ans:
                hits[f"P{port}{pin}"] = ans
                print(f"    noted: P{port}{pin} = {ans}")
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["inputs", "outputs", "all"])
    ap.add_argument("--pulses", type=int, default=20,
                    help="toggles per candidate pin in the output sweep (default 20)")
    args = ap.parse_args()

    op = _opsend()
    dev = op.open_dev(op.VID, op.PID)
    if dev is None:
        sys.exit("no OpenDMOfw device found - is it flashed and plugged in?")

    found, hits = {}, {}
    if args.mode in ("inputs", "all"):
        found = do_inputs(op, dev)
    if args.mode in ("outputs", "all"):
        hits = do_outputs(op, dev, args)

    print("\n" + "=" * 62)
    print("RESULT - paste this into the session and into PINMAP.md")
    for k, v in found.items():
        print(f"  {k:<14} {v}")
    for k, v in hits.items():
        print(f"  {k:<14} {v}")
    print("\nSTILL ON THE METER (this tool cannot reach them):")
    print("  head CLK, DI1, DI2, LAT, STB1, STB2 - a write-only shift register")
    print("     with no serial output, so nothing can be read back")
    print("  the VH gate - toggling it is refused on purpose, it switches 24 V")
    print("  (the I2C pair is found by store.c's own boot-time ladder: diag 3/4)")


if __name__ == "__main__":
    main()
