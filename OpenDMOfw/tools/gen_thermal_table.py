#!/usr/bin/env python3
"""OpenDMOfw - generate the dwell-scale table in src/printer/thermal.c.

The published energy law for a thermal head is linear in TEMPERATURE:

    E(T) = E25 - Tc * (T - 25)

(Seiko LTPD245C Technical Reference 3.5.2/3.5.7 gives Tc per paper; a shipping
24 V mechanism's own strobe table is linear in T with the same shape at every
line rate.) Our head's NTC plus its divider is not linear in temperature, so a
straight taper in raw ADC code delivers a different curve than the law asks for
- it sagged about 5 % low in the middle of the range.

This script turns the law into a 32-entry uint16 lookup indexed by `raw >> 7`
(64 bytes of flash), so the firmware does no floating point and no logarithms. Change an input here, re-run,
paste the table. Inputs are all declared, so the assumption is visible:

    R25, B         the head thermistor (ROHM KF3002 family and SII both publish
                   30 kOhm / B = 3950 for this class; SII also publish the
                   resistance table, which this script reproduces to 0.2 %)
    R_P, TO_VDD    the board divider - ASSUMED until measured (FIELDWORK 3)
    SCALE_25/70    the endpoints of our dwell scale, unchanged by this script

Usage:  python3 tools/gen_thermal_table.py [--check]
        --check prints the table and the resulting curve error instead of C.
"""
import math
import sys

R25 = 30000.0          # ohm at 25 C   (ROHM KF3002 family, SII LTPD245C 3.5.8)
B = 3950.0             # K             (same two sources)
R_P = 20000.0          # ohm divider resistor - ASSUMED, see FIELDWORK 3
TO_VDD = True          # True: NTC to VDD, R_P to GND -> hotter reads higher
ADC_MAX = 4095

T_COLD, T_HOT = 25.0, 70.0      # the two ends of the energy law we implement
SCALE_COLD, SCALE_HOT = 320, 160  # dwell scale (256 = 1.0) at those ends


def r_ntc(t_c):
    """Thermistor resistance at t_c degrees Celsius."""
    return R25 * math.exp(B * (1.0 / (t_c + 273.15) - 1.0 / 298.15))


def code_of(t_c):
    """12-bit ADC code for that temperature, for the declared divider."""
    r = r_ntc(t_c)
    frac = R_P / (r + R_P) if TO_VDD else r / (r + R_P)
    return max(0, min(ADC_MAX, int(round(frac * ADC_MAX))))


def temp_of(code):
    """Invert code_of() by bisection - only used here, never on the MCU."""
    lo, hi = -40.0, 150.0
    for _ in range(60):
        mid = (lo + hi) / 2.0
        c = code_of(mid)
        if (c < code) == TO_VDD:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def scale_of_temp(t_c):
    """The published law, clamped to the ends we support."""
    if t_c <= T_COLD:
        return SCALE_COLD
    if t_c >= T_HOT:
        return SCALE_HOT
    span = SCALE_COLD - SCALE_HOT
    return int(round(SCALE_COLD - span * (t_c - T_COLD) / (T_HOT - T_COLD)))


def table():
    """32 entries; entry i covers raw codes i*128 .. i*128+127, sampled in the
    middle of the bin so the quantisation error is symmetric."""
    return [scale_of_temp(temp_of(min(ADC_MAX, i * 128 + 64))) for i in range(32)]


def main():
    tab = table()
    if "--check" in sys.argv:
        print(f"R25={R25:.0f} B={B:.0f} R_P={R_P:.0f} "
              f"{'NTC to VDD' if TO_VDD else 'NTC to GND'}")
        print(f"codes: 25 C -> {code_of(25)}, 56 C -> {code_of(56)}, 70 C -> {code_of(70)}")
        worst = 0.0
        for t in [25, 30, 35, 40, 45, 47.5, 50, 55, 60, 65, 70]:
            want = scale_of_temp(t)
            got = tab[min(31, code_of(t) >> 7)]
            worst = max(worst, abs(got - want))
            print(f"  {t:5.1f} C  code {code_of(t):4d}  law {want:3d}  table {got:3d}")
        print(f"worst quantisation error: {worst:.0f}/256")
        print("resistances (compare SII LTPD245C Table 3-20):")
        for t in (10, 25, 55, 70, 85):
            print(f"  {t:3d} C  {r_ntc(t)/1000:8.3f} kOhm")
        return
    print("static const uint16_t k_dwell_scale[32] = {")
    for i in range(0, 32, 8):
        row = ", ".join(f"{v:3d}" for v in tab[i:i + 8])
        print(f"    {row},")
    print("};")


if __name__ == "__main__":
    main()
