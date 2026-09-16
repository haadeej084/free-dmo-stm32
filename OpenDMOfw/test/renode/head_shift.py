#!/usr/bin/env python3
"""OpenDMOfw - head shift test of the REAL image in Renode: bits and cost.

Calls head_print_line() in the firmware image directly (after a normal boot to
the main loop) with a known dot pattern, records every write to GPIOA's BSRR
and ODR, and rebuilds what a KF3002-class head would latch: on each rising CLK
edge, the current DI1 and DI2 levels. Checks:

  * exactly HEAD_DI1_DOTS clock pulses, and the DI1/DI2 streams equal the
    first/second half of the pattern, MSB of byte 0 first (tech reference);
  * data never changes in the same write that raises CLK (setup before edge);
  * dots beyond the bytes the host sent come out white.

It also prints the number of CPU instructions from entry to the latch, the
per-line cost that has to fit the genuine speed budget (550: 0.92 ms/line,
5XL: 1.08 ms/line, see head.c). Renode counts instructions, not time; the
conversion used below (1.2 to 2.0 clock cycles per instruction at 48 MHz on a
Cortex-M0 fetching from flash with one wait state) is an estimate, stated as
a range.

Usage:  RENODE=/path/to/renode python3 test/renode/head_shift.py [OP57|OP104]
"""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import elf_symbols, symbol  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL = sys.argv[1] if len(sys.argv) > 1 else "OP57"
ELF = os.path.join(ROOT, "build", MODEL, f"opendmo-{MODEL}.elf")
RENODE = os.environ.get("RENODE", "renode")

DOTS = {"OP104": 1248, "OP57": 672}[MODEL]
HALF = DOTS // 2
BUDGET_MS = {"OP104": 1.08, "OP57": 0.92}[MODEL]
CLK, DI1, DI2 = 5, 6, 7            # pins.h: PA5, PA6, PA7
BUF = 0x20000B00                   # unused heap area of the image
DWELL_US = 270 * 2                 # HEAD_BASE_DWELL_US at density 8, two segments


def pattern(nbytes):
    # A non-repeating, non-symmetric byte sequence so a reversed, shifted or
    # half-swapped stream cannot pass by accident.
    return bytes(((i * 73 + 41) ^ (i >> 1)) & 0xFF for i in range(nbytes))


def run(sent):
    syms = elf_symbols(ELF)
    hpl = symbol(syms, "head_print_line")
    dly = symbol(syms, "delay_us")
    data = pattern(sent)
    words = data + bytes((-len(data)) % 4)
    lines = [
        'mach create "t"',
        "machine LoadPlatformDescription @platforms/cpus/stm32f072.repl",
        f"sysbus LoadELF @{ELF}",
        "logLevel 3",
        "sysbus.nvic Frequency 48000000",
        'sysbus SetHookAfterPeripheralRead sysbus.rcc "value = value | '
        '(0x20000 if offset == 0x34 else 0) | '
        '(0x0C if (offset == 0x04 and (value & 3) == 3) else 0)"',
        'sysbus SetHookAfterPeripheralRead sysbus.adc "value = 1638 if offset == 0x40 else value"',
        "sysbus.gpioPortA OnGPIO 0 false",
        "sysbus.gpioPortA OnGPIO 3 true",
        'emulation RunFor "1.0"',
        "logLevel 2",
    ]
    for i in range(0, len(words), 4):
        w = int.from_bytes(words[i:i + 4], "little")
        lines.append(f"sysbus WriteDoubleWord {BUF + i:#x} {w:#x}")
    lines += [
        f'cpu AddHook {hpl:#x} "self.WarningLog(\'@@ENTER \' + str(self.ExecutedInstructions))"',
        f'cpu AddHook {dly:#x} "self.WarningLog(\'@@DELAY \' + str(self.ExecutedInstructions))"',
        'sysbus SetHookBeforePeripheralWrite sysbus.gpioPortA '
        '"self.WarningLog(\'@@W \' + str(offset) + \' \' + str(value)) if offset in (0x14, 0x18) else None"',
        f"cpu SetRegisterUnsafe 0 {BUF:#x}",
        f"cpu SetRegisterUnsafe 1 {sent}",
        f"cpu SetRegisterUnsafe 14 {symbol(syms, 'Default_Handler') | 1:#x}",
        f"cpu PC {hpl:#x}",
        'emulation RunFor "0.005"',
        "quit",
    ]
    with tempfile.NamedTemporaryFile("w", suffix=".resc", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        script = f.name
    try:
        res = subprocess.run([RENODE, "--disable-gui", "--console", "-e", f"include @{script}"],
                             capture_output=True, text=True, timeout=600)
    finally:
        os.unlink(script)
    return data, re.sub(r"\x1b\[[0-9;]*m", "", res.stdout + res.stderr)


def check(sent):
    data, out = run(sent)
    fails = []
    enter = re.search(r"@@ENTER (\d+)", out)
    delay = re.search(r"@@DELAY (\d+)", out)
    if not enter or not delay:
        return [f"hooks did not fire (sent={sent})"], None
    writes = [(int(o), int(v)) for o, v in re.findall(r"@@W (\d+) (\d+)", out)]
    # only the writes of the shift phase: before the first delay_us (latch)
    stop = out.index("@@DELAY")
    writes = [(int(o), int(v)) for o, v in re.findall(r"@@W (\d+) (\d+)", out[out.index("@@ENTER"):stop])]
    odr = 0
    s1, s2 = [], []
    for off, v in writes:
        prev = odr
        if off == 0x18:
            odr = (odr | (v & 0xFFFF)) & ~((v >> 16) & ~(v & 0xFFFF)) & 0xFFFF
        else:
            odr = v & 0xFFFF
        rising = not (prev >> CLK) & 1 and (odr >> CLK) & 1
        if rising:
            if ((prev ^ odr) >> DI1) & 1 or ((prev ^ odr) >> DI2) & 1:
                fails.append("data changed in the same write that raised CLK")
                break
            s1.append((odr >> DI1) & 1)
            s2.append((odr >> DI2) & 1)
    bits = []
    for i in range(DOTS // 8 * 1):
        byte = data[i] if i < len(data) else 0
        bits += [(byte >> (7 - k)) & 1 for k in range(8)]
    if len(s1) != HALF:
        fails.append(f"{len(s1)} clock pulses, expected {HALF}")
    if s1 != bits[:HALF]:
        fails.append("DI1 stream differs from the first half of the pattern")
    if s2 != bits[HALF:DOTS]:
        fails.append("DI2 stream differs from the second half of the pattern")
    return fails, int(delay.group(1)) - int(enter.group(1))


def main():
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} missing - build first")
    total = 0
    cost = None
    for sent in (DOTS // 8, DOTS // 8 - 30):     # full line, and a short one
        fails, n = check(sent)
        if fails:
            total += 1
            print(f"FAIL {MODEL} sent={sent}: " + "; ".join(fails))
        else:
            print(f"ok   {MODEL} sent={sent}: {HALF} clocks, DI1/DI2 streams exact")
            if sent == DOTS // 8:
                cost = n
    if cost:
        lo, hi = cost * 1.2 / 48e6 * 1e3, cost * 2.0 / 48e6 * 1e3
        print(f"shift cost {MODEL}: {cost} instructions = {lo:.2f}-{hi:.2f} ms "
              f"(+{DWELL_US / 1000:.2f} ms strobe at density 8; budget {BUDGET_MS} ms/line)")
    print("\nALL HEAD SHIFT CHECKS PASSED" if not total else f"\n{total} HEAD SHIFT CHECK(S) FAILED")
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
