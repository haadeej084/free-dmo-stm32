#!/usr/bin/env python3
"""OpenDMOfw - boot smoke test of the REAL firmware image in the Renode emulator.

Runs build/<MODEL>/opendmo-<MODEL>.elf on Renode's STM32F072 platform and checks
what can be checked without a board:

  * the image boots through SystemInit, the watchdog, SysTick, TIM3, the I2C
    EEPROM probe (no EEPROM attached: must fall back to defaults, not hang),
    the ADC and the GPIO set-up, and reaches the main loop;
  * it never enters Default_Handler (HardFault, stray IRQ);
  * SysTick really counts milliseconds of emulated time;
  * the status LED follows main.c's contract in three situations:
      cold head + paper present, no USB host  -> 1 Hz blink
      head over the 70 degC limit             -> 5 Hz blink
      paper out                               -> double blink every 1.2 s

Two things Renode does not model are bridged, and only these two:
  * RCC_CR2.HSI48RDY and RCC_CFGR.SWS for HSI48 (Renode's STM32F0 RCC leaves
    them 0, so SystemInit would wait forever);
  * ADC_DR, which is forced to a chosen thermistor code per scenario.
(The platform's SysTick reference clock is also set to our 48 MHz SYSCLK.)
USB is not modelled by Renode at all; test/test_usb.c covers that layer.

Usage:  RENODE=/path/to/renode python3 test/renode/smoke.py [OP104|OP57]
"""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL = sys.argv[1] if len(sys.argv) > 1 else "OP104"
ELF = os.path.join(ROOT, "build", MODEL, f"opendmo-{MODEL}.elf")
RENODE = os.environ.get("RENODE", "renode")

GPIOA_ODR = 0x48000014
LED_BIT = 2          # PIN_LED  = PA2
PAPER_PIN = 0        # PIN_PAPER_SENSE = PA0, present = low
BUTTON_PIN = 3       # PIN_BUTTON = PA3, pressed = low

COLD_RAW = 1638      # thermal.c THERMAL_COLD_RAW  (25 degC)
HOT_RAW = 3500       # above THERMAL_LIMIT_RAW 3240 (70 degC)


def elf_symbols(path):
    """Minimal ELF32 little-endian .symtab reader: {name: (value, size, type)}.
    Avoids depending on binutils/pyelftools on the test machine."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        sys.exit(f"{path}: not an ELF32 little-endian file")
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for sh in secs:
        if sh[1] != 2:                       # SHT_SYMTAB
            continue
        strtab = secs[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, _other, _shndx = struct.unpack_from("<IIIBBH", d, off)
            end = d.index(b"\x00", strtab[4] + name_off)
            name = d[strtab[4] + name_off:end].decode()
            if name:
                syms.setdefault(name, (value, size, info & 0xF))
    return syms


def symbol(syms, name):
    if name not in syms:
        sys.exit(f"symbol {name} not found in {ELF}")
    return syms[name][0] & ~1               # clear the Thumb bit on functions


def led_expected(kind, m):
    if kind == "hot":
        return (m // 100) & 1
    if kind == "nopaper":
        p = m % 1200
        return 1 if (p < 100 or 200 <= p < 300) else 0
    return (m // 500) & 1                      # waiting for host


def run_scenario(kind, millis_addr, fault_addr, main_lo, main_hi):
    adc_raw = HOT_RAW if kind == "hot" else COLD_RAW
    paper_level = "true" if kind == "nopaper" else "false"
    lines = [
        'mach create "opendmo"',
        "machine LoadPlatformDescription @platforms/cpus/stm32f072.repl",
        f"sysbus LoadELF @{ELF}",
        "logLevel 3",
        # the platform file assumes a 72 MHz core; this firmware runs at 48 MHz
        # and derives SysTick->LOAD from that
        "sysbus.nvic Frequency 48000000",
        # emulator gaps, see the module docstring
        'sysbus SetHookAfterPeripheralRead sysbus.rcc "value = value | '
        '(0x20000 if offset == 0x34 else 0) | '
        '(0x0C if (offset == 0x04 and (value & 3) == 3) else 0)"',
        f'sysbus SetHookAfterPeripheralRead sysbus.adc "value = {adc_raw} if offset == 0x40 else value"',
        f"sysbus.gpioPortA OnGPIO {PAPER_PIN} {paper_level}",
        f"sysbus.gpioPortA OnGPIO {BUTTON_PIN} true",
        f'cpu AddHook {fault_addr:#x} "self.WarningLog(\'@@FAULT\')"',
        'emulation RunFor "1.0"',
    ]
    samples = 90
    for _ in range(samples):
        lines += ['emulation RunFor "0.037"',
                  f"sysbus ReadDoubleWord {millis_addr:#x}",
                  f"sysbus ReadDoubleWord {GPIOA_ODR:#x}",
                  "cpu PC"]
    lines.append("quit")

    with tempfile.NamedTemporaryFile("w", suffix=".resc", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        script = f.name
    try:
        res = subprocess.run([RENODE, "--disable-gui", "--console", "-e", f"include @{script}"],
                             capture_output=True, text=True, timeout=600)
    finally:
        os.unlink(script)
    out = re.sub(r"\x1b\[[0-9;]*m", "", res.stdout + res.stderr)
    values = [int(v, 16) for v in re.findall(r"^\s*(0x[0-9A-Fa-f]+)\s*$", out, re.M)]

    fails = []
    if "@@FAULT" in out:
        fails.append("entered Default_Handler")
    if len(values) != samples * 3:
        fails.append(f"expected {samples * 3} readings, got {len(values)}")
        return fails, out
    t_prev = None
    mism = 0
    in_loop = 0
    bad_tick = None
    for i in range(samples):
        m, odr, pc = values[3 * i: 3 * i + 3]
        led = (odr >> LED_BIT) & 1
        if led not in (led_expected(kind, m), led_expected(kind, max(m - 1, 0))):
            mism += 1
        if main_lo <= pc < main_hi:
            in_loop += 1
        if t_prev is not None and not (30 <= m - t_prev <= 45) and bad_tick is None:
            bad_tick = m - t_prev
        t_prev = m
    if bad_tick is not None:
        fails.append(f"SysTick: {bad_tick} ms counted in 37 ms of emulated time")
    if mism:
        fails.append(f"LED disagreed with the '{kind}' pattern in {mism}/{samples} samples")
    if in_loop < samples * 0.9:
        fails.append(f"PC in main() only {in_loop}/{samples} samples (stuck elsewhere?)")
    return fails, out


def main():
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} missing - build first")
    syms = elf_symbols(ELF)
    millis_addr = symbol(syms, "s_millis")
    fault_addr = symbol(syms, "Default_Handler")
    main_lo = symbol(syms, "main")
    main_hi = main_lo + syms["main"][1]

    total_fail = 0
    for kind in ("cold", "hot", "nopaper"):
        fails, out = run_scenario(kind, millis_addr, fault_addr, main_lo, main_hi)
        if fails:
            total_fail += 1
            print(f"FAIL {MODEL} {kind}: " + "; ".join(fails))
            print(out[-3000:])
        else:
            print(f"ok   {MODEL} {kind}: boots, stays in main loop, SysTick 1 ms, LED pattern correct")
    print(f"\n{'ALL RENODE SMOKE TESTS PASSED' if not total_fail else f'{total_fail} RENODE SCENARIO(S) FAILED'}")
    sys.exit(1 if total_fail else 0)


if __name__ == "__main__":
    main()
