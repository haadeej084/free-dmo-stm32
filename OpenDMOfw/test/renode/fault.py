#!/usr/bin/env python3
"""OpenDMOfw - fault safe state, executed on the REAL image in Renode.

A Cortex-M0 has no recovery from a HardFault: there is no CFSR, no BFAR, and a
fault taken inside the fault handler is lockup rather than a nested exception.
So the handler is the last code that runs before the watchdog reboots the part,
and for 3.2 to 5.3 s (IWDG PR=5, RLR=1250, LSI 50 kHz max to 30 kHz min) it is
the only thing standing between a stuck strobe and the print head.

The GPIO output latches do not care that the CPU has faulted. If the fault
arrives inside head_print_line(), a heat strobe is asserted and the 24 V rail
is enabled, and with the old `for(;;){}` handler they simply stayed that way:
about 10,400x the ROHM KF3002 rated pulse energy into every dot of that half.

This test arms exactly that state, forces each way into the handler that the
hardware offers, and asserts the pins afterwards. It is written to FAIL on a
firmware without the safe-state handler - it was, four ways, before the fix.

Four triggers, all of them real paths on this part:
  hardfault  - UDF #0 executed from RAM (an undefined instruction)
  nmi        - SCB ICSR.NMIPENDSET
  irq        - an unused vector: NVIC ISER+ISPR for TIM3, which this firmware
               never enables (usb_core.c holds the only NVIC ISER write in the
               tree, so this cannot happen by itself today - it is what a
               corrupted NVIC or a remapped vector table would look like)
  cold       - the same HardFault with no boot at all, so SystemInit() never
               ran and the GPIO port clocks are still gated. This is the case
               the handler's RCC_AHBENR write exists for: a write to a gated
               port is silently discarded.

Usage:  RENODE=/path/to/renode python3 test/renode/fault.py [OP57|OP104]
"""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import elf_symbols, symbol  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL = sys.argv[1] if len(sys.argv) > 1 else "OP57"
ELF = os.path.join(ROOT, "build", MODEL, f"opendmo-{MODEL}.elf")
RENODE = os.environ.get("RENODE", "renode")

GPIOA, GPIOB = 0x48000000, 0x48000400
MODER, ODR, BSRR = 0x00, 0x14, 0x18
ICSR = 0xE000ED04
NVIC_ISER, NVIC_ISPR = 0xE000E100, 0xE000E200

# pins.h. The polarities are the firmware's own constants; if either flips,
# these flip with it and the test still asserts "not firing".
VH, VH_ON = 8, 0                      # PIN_HEAD_VH = PA8, HEAD_VH_ON_LEVEL = 0
LATCH = 4                             # PIN_HEAD_LATCH = PA4, HOLD = high
STB = [0, 1]                          # PB0, PB1 (HEAD_STROBE_SEGMENTS == 2)
STB_ACTIVE = 0                        # MODEL_STB_ACTIVE_LEVEL
MOTOR = [4, 5, 6, 7]                  # PIN_MOTOR_A1/A2/B1/B2, de-energised low


def resc(syms, trigger):
    """Renode script: boot (or not), arm the head dangerous, take the fault."""
    # Scratch for the UDF must not collide with the image. .bss on OP104 ends
    # at 0x20000b40, so a hard-coded 0x20000B00 would land on a live variable.
    scratch = (symbol(syms, "_ebss") + 0x1F) & ~3
    estack = symbol(syms, "_estack")
    cold = trigger == "cold"
    lines = [
        'mach create "t"',
        "machine LoadPlatformDescription @platforms/cpus/stm32f072.repl",
        f"sysbus LoadELF @{ELF}",
        "logLevel 3",
        "sysbus.nvic Frequency 48000000",
        # HSE/PLL ready and the paper sensor, as the other suites do.
        'sysbus SetHookAfterPeripheralRead sysbus.rcc "value = value | '
        '(0x20000 if offset == 0x34 else 0) | '
        '(0x0C if (offset == 0x04 and (value & 3) == 3) else 0)"',
        'sysbus SetHookAfterPeripheralRead sysbus.adc "value = 1638 if offset == 0x40 else value"',
        "sysbus.gpioPortA OnGPIO 0 false",
        "sysbus.gpioPortA OnGPIO 3 true",
    ]
    if not cold:
        lines += ['emulation RunFor "1.0"']          # boot to the main loop
    lines += ["logLevel 2"]

    # --- arm the dangerous state -----------------------------------------
    # Rail on, both strobes firing, all motor phases energised. Firing both
    # halves at once is a deliberate superset: head_print_line() fires them
    # strictly in sequence, so the firmware never holds both. The test wants
    # to notice a handler that safes only one.
    arm_a = (1 << VH) if VH_ON else 0                 # ODR bit: the ON level
    clr_a = 0 if VH_ON else (1 << VH)
    arm_b = sum(1 << p for p in STB if STB_ACTIVE) + sum(1 << p for p in MOTOR)
    clr_b = sum(1 << p for p in STB if not STB_ACTIVE)
    if not cold:
        lines += [
            # MODER: outputs on every pin under test, so "driven" is the
            # baseline and the handler cannot pass by leaving them alone.
            f"sysbus WriteDoubleWord {GPIOA + MODER:#x} "
            f"{(1 << (VH * 2)) | (1 << (LATCH * 2)):#x}",
            f"sysbus WriteDoubleWord {GPIOB + MODER:#x} "
            f"{sum(1 << (p * 2) for p in STB + MOTOR):#x}",
            f"sysbus WriteDoubleWord {GPIOA + BSRR:#x} {(arm_a | (clr_a << 16)):#x}",
            f"sysbus WriteDoubleWord {GPIOB + BSRR:#x} {(arm_b | (clr_b << 16)):#x}",
        ]
    lines += [
        # Prove the arming took, so a future change that makes it a no-op
        # cannot turn this test green by accident. (In the cold case these
        # two reads are the untouched reset state, and check() skips them.)
        f"sysbus ReadDoubleWord {GPIOA + ODR:#x}",
        f"sysbus ReadDoubleWord {GPIOB + ODR:#x}",
        # Renode's stm32f072 RCC is a stub: it accepts AHBENR writes and reads
        # back the reset value, so the register itself proves nothing. Hook the
        # write instead. Added only now, after SystemInit() has had its turn, so
        # what it catches is the handler's own write and not the boot's.
        'sysbus SetHookBeforePeripheralWrite sysbus.rcc '
        '"self.WarningLog(\'@@RCC \' + str(offset) + \' \' + str(value))"',
        # The final register state cannot show the ORDER of the writes, and the
        # order is the safety property in the cold case: reset leaves the ODR
        # latch at 0, which is the FIRING level for both the strobes and the VH
        # gate, so a handler that sets MODER first drives a pulse before it
        # drives the safe level. Record the writes and check the sequence.
        'sysbus SetHookBeforePeripheralWrite sysbus.gpioPortA '
        '"self.WarningLog(\'@@A \' + str(offset) + \' \' + str(value))"',
        'sysbus SetHookBeforePeripheralWrite sysbus.gpioPortB '
        '"self.WarningLog(\'@@B \' + str(offset) + \' \' + str(value))"',
    ]

    # --- take the fault ---------------------------------------------------
    if trigger in ("hardfault", "cold"):
        lines += [
            f"sysbus WriteDoubleWord {scratch:#x} 0xDE00DE00",   # UDF #0, twice
            f"cpu SetRegisterUnsafe 13 {estack:#x}",             # a usable MSP
            f"cpu PC {scratch:#x}",
        ]
    elif trigger == "nmi":
        lines += [f"sysbus WriteDoubleWord {ICSR:#x} 0x80000000"]
    elif trigger == "irq":
        lines += [
            f"sysbus WriteDoubleWord {NVIC_ISER:#x} {1 << 16:#x}",   # TIM3
            f"sysbus WriteDoubleWord {NVIC_ISPR:#x} {1 << 16:#x}",
        ]
    lines += ['emulation RunFor "0.05"']

    # --- read the result --------------------------------------------------
    lines += [
        "cpu PC",
        f"sysbus ReadDoubleWord {GPIOA + ODR:#x}",
        f"sysbus ReadDoubleWord {GPIOA + MODER:#x}",
        f"sysbus ReadDoubleWord {GPIOB + ODR:#x}",
        f"sysbus ReadDoubleWord {GPIOB + MODER:#x}",
        "quit",
    ]
    return "\n".join(lines) + "\n"


def run(syms, trigger):
    with tempfile.NamedTemporaryFile("w", suffix=".resc", delete=False) as f:
        f.write(resc(syms, trigger))
        script = f.name
    try:
        res = subprocess.run([RENODE, "--disable-gui", "--console", "-e", f"include @{script}"],
                             capture_output=True, text=True, timeout=600)
    finally:
        os.unlink(script)
    return re.sub(r"\x1b\[[0-9;]*m", "", res.stdout + res.stderr)


def values(out):
    """Renode echoes each read as its own 0x... line, in order."""
    return [int(v, 16) for v in re.findall(r"^0x([0-9A-Fa-f]+)$", out, re.M)]


def check(syms, trigger):
    out = run(syms, trigger)
    v = values(out)
    if len(v) < 7:
        return [f"{trigger}: expected 7 register reads, got {len(v)} "
                f"(Renode did not run the script)"]
    arm_a, arm_b, pc, a_odr, a_mod, b_odr, b_mod = v[:7]
    fails = []
    cold = trigger == "cold"

    def driven(moder, pin):
        return (moder >> (pin * 2)) & 3 == 1

    # 0) the arming itself
    if not cold:
        if ((arm_a >> VH) & 1) != VH_ON:
            fails.append(f"{trigger}: setup failed, the 24 V rail was not armed on")
        if any(((arm_b >> p) & 1) != STB_ACTIVE for p in STB):
            fails.append(f"{trigger}: setup failed, the strobes were not armed firing")

    lo, hi = symbol(syms, "Fault_Handler"), symbol(syms, "Fault_Handler") + 0x200
    if not (lo <= pc < hi):
        fails.append(f"{trigger}: PC {pc:#x} is not inside Fault_Handler "
                     f"({lo:#x}); the fault did not reach the safe-state handler")

    # 1) the 24 V rail
    if not driven(a_mod, VH):
        fails.append(f"{trigger}: PIN_HEAD_VH (PA{VH}) was left undriven "
                     f"(MODER={a_mod:#010x}); the rail's state is then whatever "
                     f"the board pull-up decides")
    elif ((a_odr >> VH) & 1) == VH_ON:
        fails.append(f"{trigger}: the 24 V heat rail was left ENABLED "
                     f"(PA{VH} at HEAD_VH_ON_LEVEL)")

    # 2) every fitted heat strobe
    for p in STB:
        if not driven(b_mod, p):
            fails.append(f"{trigger}: heat strobe PB{p} was left undriven "
                         f"(MODER={b_mod:#010x})")
        elif ((b_odr >> p) & 1) == STB_ACTIVE:
            fails.append(f"{trigger}: heat strobe PB{p} was left FIRING")

    # 3) the latch, so the shift register cannot be re-opened by noise
    if not driven(a_mod, LATCH) or not ((a_odr >> LATCH) & 1):
        fails.append(f"{trigger}: PIN_HEAD_LATCH (PA{LATCH}) was not left at HOLD")

    # 4) the motor, so a stalled coil is not held at DC
    for p in MOTOR:
        if driven(b_mod, p) and ((b_odr >> p) & 1):
            fails.append(f"{trigger}: motor phase PB{p} was left energised")

    # 5) the port clocks the handler needs for all of the above. On real
    #    hardware a write to a gated port is discarded, so without this the
    #    cold case would safe nothing at all.
    gpio_en = (1 << 17) | (1 << 18)
    if not any(int(off) == 0x14 and (int(val) & gpio_en) == gpio_en
               for off, val in re.findall(r"@@RCC (\d+) (\d+)", out)):
        fails.append(f"{trigger}: the handler did not enable the GPIO port "
                     f"clocks (no RCC_AHBENR write with GPIOAEN|GPIOBEN); on "
                     f"real hardware its pin writes would go nowhere")

    # 6) level before direction, per pin and per port. Cold only: elsewhere
    #    the pin is already an output, so there is no transition to order and
    #    the handler's read-modify-write re-asserts the whole register.
    for tag, port, pins in () if not cold else (("A", "PA", [(VH, not VH_ON), (LATCH, 1)]),
                            ("B", "PB", [(p, not STB_ACTIVE) for p in STB]
                                        + [(p, 0) for p in MOTOR])):
        writes = [(int(o), int(v)) for o, v in
                  re.findall(rf"@@{tag} (\d+) (\d+)", out)]
        safe_level_set = {}
        for off, val in writes:
            if off == BSRR:
                for pin, want in pins:
                    bit = 1 << pin if want else 1 << (pin + 16)
                    if val & bit:
                        safe_level_set[pin] = True
            elif off == ODR:
                for pin, want in pins:
                    if ((val >> pin) & 1) == (1 if want else 0):
                        safe_level_set[pin] = True
            elif off == MODER:
                for pin, want in pins:
                    if (val >> (pin * 2)) & 3 == 1 and not safe_level_set.get(pin):
                        fails.append(
                            f"{trigger}: {port}{pin} was switched to an output "
                            f"before its safe level was written; on a cold fault "
                            f"the ODR latch still holds 0, so that drives the "
                            f"pin active for the width of two instructions")
                        pins = [(q, w) for q, w in pins if q != pin]
                        break

    # 7) the watchdog, so the safe state is temporary rather than permanent
    if not re.search(r"0x40003000, value 0x(?:0000)?CCCC", out, re.I):
        fails.append(f"{trigger}: the handler did not start the IWDG; a fault "
                     f"before main()'s wdt_init() would never reboot")
    return fails


def main():
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} not found - run `make MODEL={MODEL}` first")
    syms = elf_symbols(ELF)
    if "Fault_Handler" not in syms:
        sys.exit("Fault_Handler not found in the image: this firmware has no "
                 "fault safe state, so a HardFault leaves the head as it was")
    fails = []
    for trigger in ("hardfault", "nmi", "irq", "cold"):
        f = check(syms, trigger)
        fails += f
        print(("ok   " if not f else "FAIL ") + f"{MODEL} {trigger}")
        for line in f:
            print("       " + line)
    print()
    if fails:
        print(f"{len(fails)} FAULT SAFETY CHECK(S) FAILED")
        sys.exit(1)
    print("ALL FAULT SAFETY CHECKS PASSED")


if __name__ == "__main__":
    main()
