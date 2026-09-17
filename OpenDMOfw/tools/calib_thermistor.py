#!/usr/bin/env python3
"""Solve the head-thermistor divider from ONE reading, and print the constants.

FIELDWORK measurement 3 used to ask for a meter on the board: identify the pull
resistor R_p, work out whether the NTC pulls the ADC pin up or down, then find
the matching column of a table and paste it into thermal.c. This removes the
meter from that job.

The arithmetic is one equation in one unknown. The NTC curve is known - 30 kOhm
at 25 C, B = 3950, published by ROHM for the KF3002 family and corroborated by
an independent mechanism reference to within 0.2 % - so a single ADC reading at
a KNOWN temperature pins R_p:

    R_ntc(T) = R25 * exp(B * (1/T - 1/T25))          T in kelvin
    pull-down (NTC to VDD, R_p to GND, hotter reads HIGHER):
        code = 4095 * R_p / (R_ntc + R_p)   ->   R_p = R_ntc * code / (4095 - code)
    pull-up   (R_p to VDD, NTC to GND, hotter reads LOWER):
        code = 4095 * R_ntc / (R_ntc + R_p) ->   R_p = R_ntc * (4095 - code) / code

The known temperature is the room, from a thermometer, with the printer cold -
unplugged or idle long enough that the head is at ambient. That is the whole
measurement.

TOPOLOGY, without a meter either: run this twice, once cold and once after a few
printed lines have warmed the head. Whichever topology gives the SAME R_p for
both readings is the one the board uses; the wrong one moves by a large factor.
This script does that comparison for you when given two readings.

    python3 tools/calib_thermistor.py --code 1638 --temp 25
    python3 tools/calib_thermistor.py --code 1638 --temp 25 --code2 2100 --temp2 40

Get the code from `opsend.py diag 4` (thermistor_raw), which reports the raw ADC
value exactly as the pin reads it, before any normalisation.

Sanity: this cannot tell a plausible wrong answer from a right one on its own.
If the printer is NOT at the temperature you typed, R_p comes out wrong by
roughly the same proportion the curve moves - about 4 %/K near 25 C. Let the
head settle, and prefer a reading taken after the printer has been off for an
hour over one taken two minutes after a print job.
"""
import argparse
import math

R25 = 30000.0          # ohm at 25 C  (ROHM KF3002 family)
BETA = 3950.0
FULL = 4095.0


def r_ntc(t_c):
    return R25 * math.exp(BETA * (1.0 / (t_c + 273.15) - 1.0 / 298.15))


def rp_from(code, t_c, pulldown):
    """R_p implied by one (code, temperature) pair under one topology."""
    if code <= 0 or code >= FULL:
        return None
    r = r_ntc(t_c)
    if pulldown:
        return r * code / (FULL - code)
    return r * (FULL - code) / code


def code_at(t_c, rp, pulldown):
    r = r_ntc(t_c)
    return round(FULL * (rp / (r + rp) if pulldown else r / (r + rp)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--code", type=int, required=True,
                    help="raw ADC code from `opsend.py diag 4`, head at a known temperature")
    ap.add_argument("--temp", type=float, required=True,
                    help="that temperature in degrees C, from a thermometer")
    ap.add_argument("--code2", type=int, help="a second reading, head warmer (optional)")
    ap.add_argument("--temp2", type=float, help="the second temperature in degrees C")
    a = ap.parse_args()

    if not (0 < a.code < FULL):
        raise SystemExit(f"code {a.code} is 0 or 4095 - that is an open or shorted "
                         f"sensor, not a reading (DECISIONS D33). Fix the wiring first.")

    cand = []
    for pulldown, name in ((True, "pull-down (NTC to VDD, R_p to GND, hotter reads HIGHER)"),
                           (False, "pull-up   (R_p to VDD, NTC to GND, hotter reads LOWER)")):
        rp = rp_from(a.code, a.temp, pulldown)
        line = f"  {name}\n      R_p = {rp:8.0f} ohm"
        if a.code2 is not None and a.temp2 is not None:
            rp2 = rp_from(a.code2, a.temp2, pulldown)
            spread = abs(rp2 - rp) / max(rp, 1.0) * 100.0
            line += f"   second reading gives {rp2:8.0f} ohm   spread {spread:5.1f} %"
            cand.append((spread, pulldown, rp, rp2))
        print(line)

    if cand:
        cand.sort()
        spread, pulldown, rp, rp2 = cand[0]
        best = (rp + rp2) / 2.0
        print(f"\nThe two readings agree under the {'PULL-DOWN' if pulldown else 'PULL-UP'} "
              f"topology (spread {spread:.1f} %), so that is the board's wiring.")
        print(f"Take R_p = {best:.0f} ohm.")
        if spread > 15.0:
            print("  ...but that spread is large. Either a temperature is wrong, or the "
                  "head had not settled, or the NTC is not the part assumed.")
    else:
        pulldown = True
        best = rp_from(a.code, a.temp, True)
        print("\nOnly one reading given, so the topology is NOT determined - the numbers "
              "above are what each would imply.\nTake a second reading with the head "
              "warmer and pass --code2/--temp2 to settle it.")

    print("\n--- paste into src/printer/thermal.c ---")
    print(f"#define THERMAL_HOTTER_IS_HIGHER {1 if pulldown else 0}")
    for t, name in ((70.0, "70 degC: halt printing (TRM)           "),
                    (56.0, "56 degC: printing may resume (TRM)     "),
                    (25.0, "25 degC: at/below this, full dwell     ")):
        c = code_at(t, best, pulldown)
        if not pulldown:
            c = 4095 - c          # thermal.c normalises so higher always means hotter
        macro = {70.0: "THERMAL_LIMIT_RAW ", 56.0: "THERMAL_RESUME_RAW", 25.0: "THERMAL_COLD_RAW  "}[t]
        print(f"#define {macro}  {c:4d}   /* {name}*/")
    print(f"\nand regenerate the dwell table with:\n"
          f"    python3 tools/gen_thermal_table.py      # after editing R_P = {best:.0f} in it")


if __name__ == "__main__":
    main()
