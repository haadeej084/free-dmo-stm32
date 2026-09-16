#!/usr/bin/env python3
"""OpenDMOfw - USB DFU entry of the REAL image in Renode.

Two halves, run separately because Renode's platform does not alias flash at
address 0, so a system reset in the emulator cannot re-enter our image the way
the chip does (boot from main flash maps it there):

  1. request:  running sys_enter_bootloader() stores BOOT_MAGIC in the .noinit
     flag and writes AIRCR = VECTKEY | SYSRESETREQ.
  2. hand-over: booting with that flag already set jumps straight to the ST boot
     loader - a stub is mapped at 0x1FFFC800 (initial SP, reset vector, a
     branch-to-self) - with the MSP loaded from the boot loader's vector table,
     and clears the flag first. Without the flag the image boots normally
     (covered by smoke.py).

The GS D 0x09 command path itself ('D' 'F' 'U' confirmation, refusal mid-job,
reply before reset) is covered by test/test_protocol.c scenario 53.

Usage:  RENODE=/path/to/renode python3 test/renode/dfu.py [OP57|OP104]
"""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import elf_symbols, symbol  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL = sys.argv[1] if len(sys.argv) > 1 else "OP57"
ELF = os.path.join(ROOT, "build", MODEL, f"opendmo-{MODEL}.elf")
RENODE = os.environ.get("RENODE", "renode")

BOOT_MAGIC = 0xDF00B007
SYSMEM = 0x1FFFC800
STUB_SP = 0x20001000
AIRCR_RESET = 0x05FA0004


def renode(lines):
    base = [
        'mach create "d"',
        "machine LoadPlatformDescription @platforms/cpus/stm32f072.repl",
        f'machine LoadPlatformDescriptionFromString "sysmem: Memory.MappedMemory @ sysbus {SYSMEM:#x} {{ size: 0x3000 }}"',
        f"sysbus LoadELF @{ELF}",
        "logLevel 3",
        "sysbus.nvic Frequency 48000000",
        'sysbus SetHookAfterPeripheralRead sysbus.rcc "value = value | '
        '(0x20000 if offset == 0x34 else 0) | '
        '(0x0C if (offset == 0x04 and (value & 3) == 3) else 0)"',
        'sysbus SetHookAfterPeripheralRead sysbus.adc "value = 1638 if offset == 0x40 else value"',
        "sysbus.gpioPortA OnGPIO 0 false",
        "sysbus.gpioPortA OnGPIO 3 true",
        f"sysbus WriteDoubleWord {SYSMEM:#x} {STUB_SP:#x}",
        f"sysbus WriteDoubleWord {SYSMEM + 4:#x} {SYSMEM + 9:#x}",   # thumb bit
        f"sysbus WriteWord {SYSMEM + 8:#x} 0xE7FE",                 # b .
    ]
    with tempfile.NamedTemporaryFile("w", suffix=".resc", delete=False) as f:
        f.write("\n".join(base + lines + ["quit"]) + "\n")
        script = f.name
    try:
        res = subprocess.run([RENODE, "--disable-gui", "--console", "-e", f"include @{script}"],
                             capture_output=True, text=True, timeout=600)
    finally:
        os.unlink(script)
    return re.sub(r"\x1b\[[0-9;]*m", "", res.stdout + res.stderr)


def hexvals(out):
    return [int(v, 16) for v in re.findall(r"^\s*(0x[0-9A-Fa-f]+)L?\s*$", out, re.M)]


def main():
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} missing - build first")
    syms = elf_symbols(ELF)
    flag = symbol(syms, "g_boot_request")
    enter = symbol(syms, "sys_enter_bootloader")
    fails = 0

    # 1. request
    out = renode([
        'emulation RunFor "1.0"',
        "logLevel 2",
        'sysbus SetHookBeforePeripheralWrite sysbus.nvic '
        '"self.WarningLog(\'@@AIRCR \' + hex(value)) if offset == 0xD0C else None"',
        f"cpu PC {enter:#x}",
        'emulation RunFor "0.01"',
        f"sysbus ReadDoubleWord {flag:#x}",
    ])
    aircr = re.search(r"@@AIRCR (0x[0-9a-fA-F]+)", out)
    vals = hexvals(out)
    f = []
    if not aircr or int(aircr.group(1), 16) != AIRCR_RESET:
        f.append(f"AIRCR write {aircr.group(1) if aircr else 'missing'}, expected {AIRCR_RESET:#x}")
    if not vals or vals[-1] != BOOT_MAGIC:
        f.append("boot flag not set before the reset")
    if f:
        fails += 1
        print(f"FAIL {MODEL} request: " + "; ".join(f))
    else:
        print(f"ok   {MODEL} request: flag {BOOT_MAGIC:#x} stored, AIRCR {AIRCR_RESET:#x} written")

    # 2. hand-over on the next boot
    out = renode([
        f"sysbus WriteDoubleWord {flag:#x} {BOOT_MAGIC:#x}",
        'emulation RunFor "0.05"',
        "cpu PC",
        "cpu GetRegister 13",
        f"sysbus ReadDoubleWord {flag:#x}",
    ])
    vals = hexvals(out)
    f = []
    if len(vals) < 3:
        f.append("could not read the emulator state")
    else:
        pc, sp, fl = vals[-3:]
        if pc != SYSMEM + 8: f.append(f"PC {pc:#x}, expected the boot loader stub {SYSMEM + 8:#x}")
        if sp != STUB_SP: f.append(f"MSP {sp:#x}, expected the boot loader's {STUB_SP:#x}")
        if fl != 0: f.append("flag not cleared before the jump")
    if f:
        fails += 1
        print(f"FAIL {MODEL} hand-over: " + "; ".join(f))
        print(out[-1500:])
    else:
        print(f"ok   {MODEL} hand-over: boot loader entered with its own MSP, flag cleared")

    print("\nALL DFU CHECKS PASSED" if not fails else f"\n{fails} DFU CHECK(S) FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
