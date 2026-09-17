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


def _shift_lines():
    """MODEL_HEAD_SHIFT_LINES from src/model.h - the firmware is the authority."""
    import re as _re
    src = open(os.path.join(ROOT, "src", "model.h"), encoding="utf-8").read()
    m = _re.search(r"^#define\s+MODEL_HEAD_SHIFT_LINES\s+(\d+)", src, _re.M)
    return int(m.group(1)) if m else 2


SHIFT_LINES = _shift_lines()
HALF = DOTS // 2
BUDGET_MS = {"OP104": 1.08, "OP57": 0.92}[MODEL]
CLK, DI1, DI2 = 5, 6, 7            # pins.h: PA5, PA6, PA7
VH = 8                             # pins.h: PIN_HEAD_VH = PA8, HEAD_VH_ON_LEVEL = 0
CFG_FLAGS_OFF = 31                 # op_config_t: magic u32 + sku[24] + count u16 + density u8
FLAG_PAPER_FORCE = 0x01
FLAG_VH_INHIBIT = 0x02
BUF = 0x20000B00                   # unused heap area of the image
DWELL_US = 270 * 2                 # HEAD_BASE_DWELL_US at density 8, two segments


def pattern(nbytes):
    # A non-repeating, non-symmetric byte sequence so a reversed, shifted or
    # half-swapped stream cannot pass by accident.
    return bytes(((i * 73 + 41) ^ (i >> 1)) & 0xFF for i in range(nbytes))


def run(sent, poke=(), window=0.005):
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
        *[f"sysbus WriteByte {addr:#x} {val:#x}" for addr, val in poke],
        "sysbus ReadDoubleWord 0x48000014",   # GPIOA ODR: the level before the call
        f"cpu SetRegisterUnsafe 0 {BUF:#x}",
        f"cpu SetRegisterUnsafe 1 {sent}",
        f"cpu SetRegisterUnsafe 14 {symbol(syms, 'Default_Handler') | 1:#x}",
        f"cpu PC {hpl:#x}",
        f'emulation RunFor "{window}"',
        f"sysbus ReadDoubleWord {symbol(syms, 's_vh_on'):#x}",
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
    # MODEL_HEAD_SHIFT_LINES decides the topology this build was compiled for.
    # 1 (the default, and what the vendor's own firmware does): the whole line
    #   goes out on DI1, HEAD_DOTS clocks, byte 0 bit 7 first.
    # 2: the two halves go out in parallel on DI1 and DI2, HEAD_DOTS/2 clocks.
    # Reading it from the source rather than assuming keeps this test honest
    # when someone flips the switch after a continuity check on the flex.
    if SHIFT_LINES == 1:
        if len(s1) != DOTS:
            fails.append(f"{len(s1)} clock pulses, expected {DOTS}")
        if s1 != bits[:DOTS]:
            fails.append("the DI1 stream is not the pattern, byte 0 bit 7 first")
        if any(s2):
            fails.append("DI2 moved, but this build shifts on one data line")
    else:
        if len(s1) != HALF:
            fails.append(f"{len(s1)} clock pulses, expected {HALF}")
        if s1 != bits[:HALF]:
            fails.append("DI1 stream differs from the first half of the pattern")
        if s2 != bits[HALF:DOTS]:
            fails.append("DI2 stream differs from the second half of the pattern")
    return fails, int(delay.group(1)) - int(enter.group(1))


def vh_odr_trace(out, seed):
    """Rebuild PA8's level over every GPIOA write after head_print_line() was
    entered. Returns (levels, latched) where latched is True once the latch
    pulse (first delay_us) has been seen."""
    body = out[out.index("@@ENTER"):] if "@@ENTER" in out else out
    first_delay = body.index("@@DELAY") if "@@DELAY" in body else len(body)
    odr, trace = seed & 0xFFFF, []
    for m in re.finditer(r"@@W (\d+) (\d+)", body):
        off, v = int(m.group(1)), int(m.group(2))
        if off == 0x18:
            odr = (odr | (v & 0xFFFF)) & ~((v >> 16) & ~(v & 0xFFFF)) & 0xFFFF
        else:
            odr = v & 0xFFFF
        trace.append(((odr >> VH) & 1, m.start() < first_delay))
    return trace


def check_interlock():
    """The 24 V interlock is the only thing protecting an irreplaceable head, and
    until now it was only ever exercised against a mock that reimplemented it.
    Run the real head.c both ways."""
    syms = elf_symbols(ELF)
    cfg = symbol(syms, "s_cfg") + CFG_FLAGS_OFF
    sent = DOTS // 8
    fails = []

    # A. interlock clear: VH must go low (= on) exactly once, and only after the
    #    latch - never while the line is still being shifted in.
    _, out = run(sent, poke=[(cfg, FLAG_PAPER_FORCE)], window=0.02)
    vals = [int(v, 16) for v in re.findall(r"^\s*(0x[0-9A-Fa-f]+)\s*$", out, re.M)]
    if len(vals) < 2:
        return ["could not read the emulator state (ODR seed / s_vh_on)"]
    trace = vh_odr_trace(out, vals[0])
    on_during_shift = [lvl for lvl, before_latch in trace if before_latch and lvl == 0]
    turned_on = [i for i in range(1, len(trace)) if trace[i][0] == 0 and trace[i - 1][0] == 1]
    vh_on = vals
    if on_during_shift:
        fails.append("the heat rail was switched on during the shift phase")
    if len(turned_on) != 1:
        fails.append(f"the heat rail was switched on {len(turned_on)} times, expected once")
    if not vh_on or vh_on[-1] != 1:
        fails.append("head.c did not record the rail as on (s_vh_on)")

    # B. interlock set: no write may ever drive PA8 low, and s_vh_on stays 0.
    _, out = run(sent, poke=[(cfg, FLAG_PAPER_FORCE | FLAG_VH_INHIBIT)], window=0.02)
    vals = [int(v, 16) for v in re.findall(r"^\s*(0x[0-9A-Fa-f]+)\s*$", out, re.M)]
    if len(vals) < 2:
        return fails + ["could not read the emulator state (inhibited pass)"]
    trace = vh_odr_trace(out, vals[0])
    vh_on = vals
    if any(lvl == 0 for lvl, _ in trace):
        fails.append("OP_FLAG_VH_INHIBIT was set and the heat rail still went on")
    if not vh_on or vh_on[-1] != 0:
        fails.append("s_vh_on was set while the interlock was armed")
    return fails


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
            n = DOTS if SHIFT_LINES == 1 else HALF
            how = "one data line" if SHIFT_LINES == 1 else "DI1/DI2 in parallel"
            print(f"ok   {MODEL} sent={sent}: {n} clocks on {how}, stream exact")
            if sent == DOTS // 8:
                cost = n
    f = check_interlock()
    if f:
        total += 1
        print(f"FAIL {MODEL} VH interlock: " + "; ".join(f))
    else:
        print(f"ok   {MODEL} VH interlock: rail only after the latch, and never while inhibited")

    if cost:
        lo, hi = cost * 1.2 / 48e6 * 1e3, cost * 2.0 / 48e6 * 1e3
        print(f"shift cost {MODEL}: {cost} instructions = {lo:.2f}-{hi:.2f} ms "
              f"(+{DWELL_US / 1000:.2f} ms strobe at density 8; budget {BUDGET_MS} ms/line)")
    print("\nALL HEAD SHIFT CHECKS PASSED" if not total else f"\n{total} HEAD SHIFT CHECK(S) FAILED")
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
