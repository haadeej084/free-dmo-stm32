#!/usr/bin/env python3
"""OpenDMOfw - config EEPROM test of the REAL image in Renode.

Boots the built image with a Renode I2C EEPROM model (Memory.GenericI2cEeprom
on I2C1 at 0x50, backed by an ArrayMemory the test can pre-load and inspect)
and checks store.c against both parts the boards are known to carry:

  * BL24C128A class (Rev H/I/K): 16 KB, 2-byte addressing, 64-byte pages
  * AT24C02 class (Rev E): 256 B, 1-byte addressing, 8-byte pages

Scenarios: blank part -> defaults persisted at the right offset with the right
addressing, and the low 256 bytes of the big part left untouched; a stored
record is loaded instead of the defaults; the legacy 2-byte offset 0 is still
found; an out-of-range density is sanitised; a write-protected part leaves the
firmware running on RAM defaults without hanging and without writing.

Not tested here, on purpose: a record already stored on the small 1-byte part.
store.c probes 2-byte addressing first, which on a 1-byte part clocks one
address byte followed by one data byte and then a repeated START. A real
24Cxx only commits written data on a STOP condition, so nothing is written;
Renode's GenericI2cEeprom commits each byte as it arrives, which would
overwrite the stored magic and make the test measure the model, not the
firmware. The blank-part scenario still covers 1-byte detection end to end.

Usage:  RENODE=/path/to/renode python3 test/renode/eeprom.py [OP57|OP104]
"""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import elf_symbols, symbol  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL = sys.argv[1] if len(sys.argv) > 1 else "OP57"
ELF = os.path.join(ROOT, "build", MODEL, f"opendmo-{MODEL}.elf")
RENODE = os.environ.get("RENODE", "renode")

MAGIC = 0x4F444D32                      # store.c CFG_MAGIC "ODM2"
REC_LEN = 33                            # sizeof(op_config_t): 32 + the sum byte
DEFAULTS = {"OP104": ("S0904980", 220), "OP57": ("30387", 100)}[MODEL]
EE_BASE = 0x70000000                    # where the backing memory is mapped
FLAG_PAPER_FORCE = 1
FLAG_VH_INHIBIT = 2                     # store.h OP_FLAG_VH_INHIBIT

PART_BIG = dict(size=0x4000, abits=16, page=64)
PART_SMALL = dict(size=0x100, abits=8, page=8)


def cfg_sum(body):
    """store.c cfg_sum(): plain 8-bit sum over every byte but the sum itself."""
    return sum(body) & 0xFF


def record(sku, count, density=8, flags=FLAG_PAPER_FORCE, sum_ok=True):
    body = struct.pack("<I24sHBB", MAGIC, sku.encode(), count, density, flags)
    s = cfg_sum(body)
    return body + bytes([s if sum_ok else (s ^ 0xFF)])


def run(part, preload, writable=True, read_len=0x140):
    syms = elf_symbols(ELF)
    cfg = symbol(syms, "s_cfg")
    addrw = symbol(syms, "s_addrw")
    main_lo = symbol(syms, "main")
    main_hi = main_lo + syms["main"][1]
    lines = [
        'mach create "e"',
        "machine LoadPlatformDescription @platforms/cpus/stm32f072.repl",
        f'machine LoadPlatformDescriptionFromString "eemem: Memory.ArrayMemory @ sysbus {EE_BASE:#x} {{ size: {part["size"]:#x} }}"',
        'machine LoadPlatformDescriptionFromString "eeprom: Memory.GenericI2cEeprom @ i2c1 0x50 '
        f'{{ memory: eemem; addressBitSize: {part["abits"]}; pageSize: {part["page"]}; writable: {"true" if writable else "false"} }}"',
        f"sysbus LoadELF @{ELF}",
        "logLevel 3",
        "sysbus.nvic Frequency 48000000",
        'sysbus SetHookAfterPeripheralRead sysbus.rcc "value = value | '
        '(0x20000 if offset == 0x34 else 0) | '
        '(0x0C if (offset == 0x04 and (value & 3) == 3) else 0)"',
        'sysbus SetHookAfterPeripheralRead sysbus.adc "value = 1638 if offset == 0x40 else value"',
        "sysbus.gpioPortA OnGPIO 0 false",
        "sysbus.gpioPortA OnGPIO 3 true",
    ]
    for off, data in preload:
        for i, b in enumerate(data):
            lines.append(f"sysbus WriteByte {EE_BASE + off + i:#x} {b:#x}")
    lines.append('emulation RunFor "2.0"')
    n = min(read_len, part["size"])
    for i in range(n):
        lines.append(f"sysbus ReadByte {EE_BASE + i:#x}")
    for i in range(REC_LEN):
        lines.append(f"sysbus ReadByte {cfg + i:#x}")
    lines += [f"sysbus ReadByte {addrw:#x}", "cpu PC", "quit"]
    with tempfile.NamedTemporaryFile("w", suffix=".resc", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        script = f.name
    try:
        res = subprocess.run([RENODE, "--disable-gui", "--console", "-e", f"include @{script}"],
                             capture_output=True, text=True, timeout=900)
    finally:
        os.unlink(script)
    out = re.sub(r"\x1b\[[0-9;]*m", "", res.stdout + res.stderr)
    vals = [int(v, 16) for v in re.findall(r"^\s*(0x[0-9A-Fa-f]+)\s*$", out, re.M)]
    if len(vals) != n + REC_LEN + 2:
        return None, out
    ee = bytes(vals[:n])
    ram = bytes(vals[n:n + REC_LEN])
    return dict(ee=ee, ram=ram, addrw=vals[n + REC_LEN],
                in_main=main_lo <= vals[n + REC_LEN + 1] < main_hi), out


def unpack(rec):
    magic, sku, count, density, flags = struct.unpack("<I24sHBB", rec[:32])
    return magic, sku.split(b"\0", 1)[0].decode("ascii", "replace"), count, density, flags


def main():
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} missing - build first")
    sku0, count0 = DEFAULTS
    low = bytes((i * 7 + 3) & 0xFF for i in range(0x100))   # "stock" data below 0x100
    custom = record("30252", 350, density=5, flags=FLAG_PAPER_FORCE)
    scenarios = []

    def scen(name, part, preload, expect, writable=True):
        scenarios.append((name, part, preload, expect, writable))

    # 1. blank 16 KB part: defaults written at 0x100 with 2-byte addressing,
    #    and the low 256 bytes (a stock image's area) left exactly as they were
    def exp_big_blank(r):
        f = []
        if r["addrw"] != 2: f.append(f"address width {r['addrw']}, expected 2")
        if unpack(r["ee"][0x100:0x100 + REC_LEN])[:3] != (MAGIC, sku0, count0): f.append("defaults not persisted at 0x100")
        if r["ee"][:0x100] != low: f.append("bytes below 0x100 were modified")
        if unpack(r["ram"])[:3] != (MAGIC, sku0, count0): f.append("RAM config is not the defaults")
        return f
    scen("16 KB blank", PART_BIG, [(0, low)], exp_big_blank)

    # 2. blank 256 B part (Rev E): defaults at 0 with 1-byte addressing
    def exp_small_blank(r):
        f = []
        if r["addrw"] != 1: f.append(f"address width {r['addrw']}, expected 1")
        if unpack(r["ee"][0:REC_LEN])[:3] != (MAGIC, sku0, count0): f.append("defaults not persisted at 0")
        if unpack(r["ram"])[:3] != (MAGIC, sku0, count0): f.append("RAM config is not the defaults")
        return f
    scen("256 B blank", PART_SMALL, [], exp_small_blank)

    # 3. stored record on the 16 KB part is loaded, not overwritten
    def exp_loaded(r):
        f = []
        if unpack(r["ram"]) != unpack(custom): f.append(f"RAM config {unpack(r['ram'])} != stored record")
        if r["ee"][0x100:0x100 + REC_LEN] != custom: f.append("stored record was rewritten")
        return f
    scen("16 KB stored record", PART_BIG, [(0x100, custom)], exp_loaded)

    # 3b. A record whose magic is valid but whose checksum is not must NOT be
    #     accepted, and the firmware must fail SAFE rather than fall back to the
    #     compiled defaults as if the part were blank.
    #
    #     This is the case a 4-byte magic could never see: one flipped bit
    #     anywhere in the record - or a write torn by a power loss, which on the
    #     8-byte-page part spans several pages and leaves the magic intact in the
    #     first one. The byte that decides whether the head may be heated lives in
    #     that record, so an unverifiable record locks the heat rail out and waits
    #     for the operator (GS D 0x08) instead of guessing.
    corrupt = record("BADSUM", 42, flags=FLAG_PAPER_FORCE, sum_ok=False)

    def exp_corrupt(r):
        f = []
        magic, sku, count, density, flags = unpack(r["ram"])
        if sku == "BADSUM" or count == 42:
            f.append("a record with a bad checksum was ACCEPTED")
        if not (flags & FLAG_VH_INHIBIT):
            f.append(f"heat rail not locked out after a corrupt record (flags={flags:#04x})")
        rec = r["ee"][0x100:0x100 + REC_LEN]
        if rec[:4] != struct.pack("<I", MAGIC) or cfg_sum(rec[:REC_LEN - 1]) != rec[REC_LEN - 1]:
            f.append("defaults were not re-persisted with a valid checksum")
        return f
    scen("16 KB corrupt checksum", PART_BIG, [(0x100, corrupt)], exp_corrupt)

    # 4. legacy location: 2-byte part with the record at offset 0
    def exp_legacy(r):
        f = []
        if unpack(r["ram"]) != unpack(custom): f.append("legacy record at 0 not found")
        if r["addrw"] != 2: f.append(f"address width {r['addrw']}, expected 2")
        return f
    scen("16 KB legacy offset 0", PART_BIG, [(0, custom)], exp_legacy)

    # 5. out-of-range density is sanitised to 8
    bad = record("30252", 350, density=99)
    def exp_density(r):
        return [] if unpack(r["ram"])[3] == 8 else [f"density {unpack(r['ram'])[3]}, expected 8"]
    scen("density sanitised", PART_BIG, [(0x100, bad)], exp_density)

    # 6. write-protected blank part: firmware runs on RAM defaults, EEPROM untouched
    def exp_wp(r):
        f = []
        if unpack(r["ram"])[:3] != (MAGIC, sku0, count0): f.append("RAM config is not the defaults")
        if any(r["ee"][0x100:0x100 + REC_LEN]): f.append("write-protected part was written")
        return f
    scen("16 KB write-protected", PART_BIG, [], exp_wp, writable=False)

    fails = 0
    for name, part, preload, expect, writable in scenarios:
        r, out = run(part, preload, writable)
        if r is None:
            fails += 1
            print(f"FAIL {MODEL} {name}: could not read the emulator state")
            print(out[-2000:])
            continue
        f = expect(r)
        if not r["in_main"]:
            f.append("not in the main loop after 2 s (hung?)")
        if f:
            fails += 1
            print(f"FAIL {MODEL} {name}: " + "; ".join(f))
        else:
            print(f"ok   {MODEL} {name}")
    print("\nALL EEPROM CHECKS PASSED" if not fails else f"\n{fails} EEPROM SCENARIO(S) FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
