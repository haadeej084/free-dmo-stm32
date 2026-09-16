#!/usr/bin/env python3
# OpenDMOfw - wire-protocol verification harness (host, no board needed).
#
# Independent of the native C parser test (test_protocol.c). This harness
# transcribes the response
# generators (send_status / send_sku_record / send_version in
# src/printer/protocol.c) into Python and checks their output byte-for-byte
# against two independent sources of truth:
#
#   1. A real capture from a genuine LabelWriter 550 (capture_long.log):
#        ESC A (1b 41 00) -> 32-byte status struct.
#   2. The decompiled stock driver's struct definitions (DYMO.PrinterCommands):
#        LW550_STATUS  [Pack=1, Size=32]
#        LW550_VERSION [Pack=1, Size=34] = HwVer[16] + FwVer[16] + ProdID(u16 LE)
#
# It also asserts the structural invariants a well-formed reply must satisfy
# (exact lengths, magic bytes, field positions).
#
# SCOPE - read this before trusting it as a regression test. The generators here
# are a HAND transcription of the C, so this harness checks the transcription
# against the capture and the driver structs; it does NOT execute protocol.c and
# cannot catch C code drifting away from this file. The executable regression
# test on the real parser is test/test_protocol.c (run by `make test` whenever a
# host C compiler is present). Keep the two in sync when protocol.c changes.
#
# Run:  python3 test/test_protocol_wire.py
# Exit 0 = all checks pass.

import sys

FAILS = []
def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        FAILS.append(msg)

# ---------------------------------------------------------------------------
# Real captured status struct (capture_long.log, genuine LW550, roll+tag present).
# The log printed raw[0..28]; bytes 29-31 are the trailing EPS/voltage/reserved
# fields per the tech ref. PrintStatus=0x02 and LabelCount=0 are transient
# roll-detection states, not the idle steady state.
# ---------------------------------------------------------------------------
CAPTURED_STATUS = bytes([
    0x02,                                   # [0]  PrintStatus (Error - transient)
    0x00, 0x00, 0x00, 0x00,                 # [1-4] JobID u32 LE
    0x00, 0x00,                             # [5-6] LabelIndex u16 LE
    0x00,                                   # [7]  CutterStatus / reserved
    0x00,                                   # [8]  HeadStatus (ok)
    0x64,                                   # [9]  Density = 100 %
    0x08,                                   # [10] MainBayStatus = OK
    *([0x00] * 12),                         # [11-22] SKU (empty on the real unit)
    0x00, 0x00, 0x00, 0x00,                 # [23-26] ErrorID u32
    0x00, 0x00,                             # [27-28] LabelCount u16 LE (=0 transient)
])
# bytes 29-31 not in the capture; tech-ref defaults: EPS present / voltage ok / reserved
assert len(CAPTURED_STATUS) == 29

# ---------------------------------------------------------------------------
# Transcription of src/printer/protocol.c :: send_status()
# ---------------------------------------------------------------------------
def firmware_status(job_active=0, job_id=0, label_index=0, density_pct=100,
                    paper_present=True, sku=b"", label_count=220):
    r = bytearray(32)
    r[0] = 1 if job_active else 0
    r[1:5] = (job_id & 0xFFFFFFFF).to_bytes(4, "little")
    r[5:7] = (label_index & 0xFFFF).to_bytes(2, "little")
    r[7] = 0
    r[8] = 0
    r[9] = density_pct
    r[10] = 8 if paper_present else 2
    sku = sku[:12]
    for i in range(12):
        if i < len(sku):
            r[11 + i] = sku[i]
    # r[23..26] ErrorID = 0
    r[27:29] = (label_count & 0xFFFF).to_bytes(2, "little")
    r[29] = 0x01   # EPS present
    r[30] = 0x01   # head voltage ok
    r[31] = 0xFF   # reserved
    return bytes(r)

# ---------------------------------------------------------------------------
# Transcription of src/printer/protocol.c :: send_version()
# ---------------------------------------------------------------------------
def firmware_version(pid, hw=b"LW5XL-REV.K", fw=b"FWAP000100010921"):
    r = bytearray(34)
    for i in range(16):
        r[i] = hw[i] if i < len(hw) else 0
        r[16 + i] = fw[i] if i < len(fw) else 0
    r[32:34] = (pid & 0xFFFF).to_bytes(2, "little")
    return bytes(r)

# ---------------------------------------------------------------------------
# Transcription of src/printer/protocol.c :: send_sku_record()
# ---------------------------------------------------------------------------
import zlib

def dots_to_tenth_mm(dots, dpi=300):
    """protocol.c :: dots_to_tenth_mm - 254 tenths per inch, rounded."""
    return (dots * 254 + dpi // 2) // dpi

LABEL_GAP_TENTH_MM = 42

# Defaults = the OP104 default paper (0x0867, 1233 x 1883 dots = S0904980 4x6").
def firmware_sku_record(sku, label_count, w_tmm=None, h_tmm=None,
                        head_dots=1248, dpi=300, model_default_count=220):
    if w_tmm is None:
        w_tmm = dots_to_tenth_mm(1233, dpi)
    if h_tmm is None:
        h_tmm = dots_to_tenth_mm(1883, dpi)
    r = bytearray(63)
    r[0] = 0xB6; r[1] = 0xCA          # magic 0xCAB6 LE
    r[2] = 0                          # version
    r[3] = 0x3C                       # payload length, constant on 37/37 tags
    for i in range(12):
        if i < len(sku):
            r[8 + i] = sku[i]
    r[20] = 0x00; r[21] = 0xFF; r[22] = 0x04; r[23] = 0x01
    r[24] = 0x01; r[25] = 0x00; r[26] = 0x00
    pitch_tmm = h_tmm + LABEL_GAP_TENTH_MM
    r[28:30] = (pitch_tmm & 0xFFFF).to_bytes(2, "little")
    r[30] = 30; r[31] = 0             # marker1 width 3.0 mm (35/37)
    r[32] = 38; r[33] = 0             # marker1 to label start (S0904980)
    r[38] = 16; r[39] = 0             # vertical offset 1.6 mm (23/37)
    r[40:42] = (h_tmm & 0xFFFF).to_bytes(2, "little")
    r[42:44] = (w_tmm & 0xFFFF).to_bytes(2, "little")
    # 44-47 printable-area offsets stay zero (37/37)
    r[48:50] = (dots_to_tenth_mm(head_dots, dpi) & 0xFFFF).to_bytes(2, "little")
    total = max(model_default_count, label_count)
    r[50:52] = (total & 0xFFFF).to_bytes(2, "little")
    r[52:54] = min(pitch_tmm * total // 20, 0xFFFF).to_bytes(2, "little")
    r[54:56] = ((total // 10) & 0xFFFF).to_bytes(2, "little")
    r[56] = 0x01                      # counter strategy (37/37)
    # 57-62 stay zero: genuine tags carry nothing past byte 59
    crc = zlib.crc32(bytes(r[0:60])) & 0xFFFFFFFF    # 4-7 are still zero here
    r[4:8] = crc.to_bytes(4, "little")
    return bytes(r)

# ---------------------------------------------------------------------------
print("== STATUS STRUCT (ESC A) vs real capture ==")
cap = CAPTURED_STATUS
fw  = firmware_status()
check(len(fw) == 32, "status is exactly 32 bytes (driver reads 32)")
# Fields that MUST match the genuine device regardless of job state:
check(fw[7]  == cap[7],  "[7]  CutterStatus/reserved = 0x00")
check(fw[8]  == cap[8],  "[8]  HeadStatus = 0x00 (ok)")
check(fw[9]  == cap[9],  "[9]  Density = 0x64 (100 %)")
check(fw[10] == cap[10], "[10] MainBayStatus = 0x08 (OK, not counterfeit/no-media)")
check(fw[23:27] == cap[23:27], "[23-26] ErrorID = 0")
# Field positions must line up with the capture even where the value is state-dependent:
check(cap[9] == 0x64 and cap[10] == 0x08, "capture sanity: density@9=0x64, bay@10=0x08")
check(fw[29] == 1 and fw[30] == 1 and fw[31] == 0xFF, "[29-31] EPS/voltage/reserved = 1/1/0xFF (tech ref)")

print("\n== STATUS: full-field layout sanity (positions) ==")
# A synthetic 'printing' status with known fields must place them at the right offsets.
st = firmware_status(job_active=1, job_id=0x1234, label_index=7, density_pct=150,
                     sku=b"S0904980", label_count=220)
check(st[0] == 1, "PrintStatus=1 (printing) at [0]")
check(st[1:5] == (0x1234).to_bytes(4, "little"), "JobID u32 LE at [1-4]")
check(st[5:7] == (7).to_bytes(2, "little"), "LabelIndex u16 LE at [5-6]")
check(st[9] == 150, "Density=150 at [9]")
check(st[11:11+8] == b"S0904980", "SKU bytes at [11..]")
check(st[27:29] == (220).to_bytes(2, "little"), "LabelCount u16 LE at [27-28]")

print("\n== VERSION (ESC V) vs LW550_VERSION [Size=34] ==")
# Per-model hardware string, mirroring model.h (MODEL_HW_VERSION):
for pid, name, hw in [(0x002A, "5XL", b"LW5XL-REV.K"), (0x0028, "550", b"LW550-REV.K")]:
    v = firmware_version(pid, hw=hw)
    check(len(v) == 34, f"{name}: version is exactly 34 bytes")
    check(v[32:34] == (pid).to_bytes(2, "little"), f"{name}: ProdID u16 LE at [32-33]")
    check(v[0:len(hw)] == hw and len(v[0:16]) == 16, f"{name}: HwVer field is the per-model string")
    check(len(v[16:32]) == 16, f"{name}: FwVer field is 16 bytes")

print("\n== SKU RECORD (ESC U) vs 37 genuine roll-tag dumps ==")
u = firmware_sku_record(b"S0904980", 220)
check(len(u) == 63, "SKU record is exactly 63 bytes")
check(u[0] == 0xB6 and u[1] == 0xCA, "magic 0xCAB6 (LE) at [0-1]")
check(u[3] == 0x3C, "byte 3 is the constant payload length 0x3C, not the SKU length")
check(u[8:8+8] == b"S0904980", "SKU bytes at [8..]")
_z = bytearray(u[0:60]); _z[4:8] = b"\x00\x00\x00\x00"
check(u[4:8] == (zlib.crc32(bytes(_z)) & 0xFFFFFFFF).to_bytes(4, "little"),
      "CRC-32 (zlib) at [4-7] over bytes 0-59 with 4-7 zeroed")
check(u[22] == 0x04, "material byte is one real tags use, not the manual's 0x03")
check(u[44:48] == b"\x00\x00\x00\x00", "printable-area offsets [44-47] zero (37/37)")
check(u[56] == 0x01, "counter strategy [56] = 0x01 (37/37), not the manual's 0x00")
check(u[60:63] == b"\x00\x00\x00", "[60-62] zero - genuine tags stop at byte 59")
check(dots_to_tenth_mm(1233) == 1044 and dots_to_tenth_mm(1883) == 1594,
      "S0904980 1233x1883 dots -> 1044 x 1594 tenths = 104.4 x 159.4 mm")
check(int.from_bytes(u[42:44], "little") == 1044 and
      int.from_bytes(u[40:42], "little") == 1594,
      "record carries the label as 1044 x 1594 tenths of a mm")
check(dots_to_tenth_mm(1248) == 1057 and dots_to_tenth_mm(672) == 569,
      "liner width: 1248 dots -> 1057 tenths, 672 -> 569")
check(int.from_bytes(u[54:56], "little") == 22, "counter margin [54-55] = count/10")
check(int.from_bytes(u[52:54], "little") == (1594 + 42) * 220 // 20,
      "total media length [52-53] is in 2 mm units")

# ---------------------------------------------------------------------------
# Transcription of src/printer/protocol.c :: diagnose()  (GS D backdoor)
# Uniform layout: r[0]='D', r[1]=sub, then sub-dependent fields.
# ---------------------------------------------------------------------------
def firmware_diag(sub, arg=0, pid_lo=0x2A, traw=0x1234, therm_ok=1, paper=1,
                  button=0, density=100, flags=1, sku=b"S0904980", count=220,
                  head_bytes=156, eeprom=1):
    r = bytearray(24)
    r[0] = 0x44; r[1] = sub                      # 'D', sub
    if sub == 0x01:
        r[2] = arg; r[3] = therm_ok
        return bytes(r[:4])
    if sub == 0x02:
        r[2] = arg
        return bytes(r[:3])
    if sub == 0x03:
        r[2] = eeprom
        return bytes(r[:3])
    # 0x04 snapshot (default)
    r[2] = pid_lo
    r[3] = (traw >> 8) & 0xFF; r[4] = traw & 0xFF
    r[5] = therm_ok
    r[6] = (paper & 1) | ((button & 1) << 1)   # protocol.c: r[6] |= 2 when pressed
    r[7] = density
    r[8] = flags
    for i in range(12):
        if i < len(sku): r[9 + i] = sku[i]
    r[21] = count & 0xFF; r[22] = (count >> 8) & 0xFF
    r[23] = head_bytes
    return bytes(r)

print("\n== DIAGNOSTIC BACKDOOR (GS D) layout ==")
d4 = firmware_diag(0x04)
check(len(d4) == 24, "snapshot is exactly 24 bytes")
check(d4[0] == 0x44 and d4[1] == 0x04, "snapshot: 'D' marker + sub=0x04 at [0-1]")
check(d4[2] == 0x2A, "snapshot: model id (PID low byte) at [2]")
check(((d4[3] << 8) | d4[4]) == 0x1234, "snapshot: thermistor raw u16 BE at [3-4]")
check(d4[5] == 1, "snapshot: thermal_ok at [5]")
check(d4[6] & 1 == 1, "snapshot: paper-present bit0 at [6]")
check(firmware_diag(0x04, paper=1, button=1)[6] == 0x03,
      "snapshot: button-pressed bit1 at [6] (paper+button = 0x03)")
check(firmware_diag(0x04, paper=0, button=1)[6] == 0x02,
      "snapshot: button bit is independent of the paper bit")
check(d4[7] == 100, "snapshot: density % at [7]")
check(d4[8] == 1, "snapshot: flags at [8]")
check(d4[9:9+8] == b"S0904980", "snapshot: SKU bytes at [9..]")
check(int.from_bytes(d4[21:23], "little") == 220, "snapshot: label count u16 LE at [21-22]")
check(d4[23] == 156, "snapshot: head_bytes at [23]")
d1 = firmware_diag(0x01, arg=8)
check(len(d1) == 4 and d1[0] == 0x44 and d1[1] == 0x01 and d1[2] == 8,
      "strobe reply: 4 B = 'D', sub, count")
d2 = firmware_diag(0x02, arg=30)
check(len(d2) == 3 and d2[0] == 0x44 and d2[1] == 0x02 and d2[2] == 30,
      "motor reply: 3 B = 'D', sub, count")
d3 = firmware_diag(0x03, eeprom=1)
check(len(d3) == 3 and d3[0] == 0x44 and d3[1] == 0x03 and d3[2] == 1,
      "EEPROM reply: 3 B = 'D', sub, match")

print()
if FAILS:
    print(f"{len(FAILS)} CHECK(S) FAILED")
    sys.exit(1)
print("ALL WIRE-PROTOCOL CHECKS PASSED")
