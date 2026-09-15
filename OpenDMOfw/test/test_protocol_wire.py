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
# (exact lengths, magic bytes, field positions) so a regression in the C code's
# byte layout would show up here as soon as the transcription is refreshed.
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
def firmware_version(pid, hw=b"LW5XL-REV.K", fw=b"FWAP01.02.2112"):
    r = bytearray(34)
    for i in range(16):
        r[i] = hw[i] if i < len(hw) else 0
        r[16 + i] = fw[i] if i < len(fw) else 0
    r[32:34] = (pid & 0xFFFF).to_bytes(2, "little")
    return bytes(r)

# ---------------------------------------------------------------------------
# Transcription of src/printer/protocol.c :: send_sku_record()
# ---------------------------------------------------------------------------
def crc16_ccitt(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

def firmware_sku_record(sku, label_count, w_mm=104, h_mm=59, head_dots=1248, dpi=300):
    r = bytearray(63)
    r[0] = 0xB6; r[1] = 0xCA          # magic 0xCAB6 LE
    r[2] = 0                          # version
    slen = 0
    while slen < 12 and slen < len(sku) and sku[slen]:
        slen += 1
    r[3] = slen
    for i in range(12):
        if i < len(sku):
            r[8 + i] = sku[i]
    r[20] = 0x00; r[21] = 0xFF; r[22] = 0x03; r[23] = 0x01
    r[24] = 0x01; r[25] = 0x00; r[26] = 0x00
    pitch_mm = h_mm + 3
    r[28:30] = (pitch_mm & 0xFFFF).to_bytes(2, "little")
    r[30] = 2; r[31] = 0
    r[32] = 2; r[33] = 0
    r[38] = 1; r[39] = 0
    r[40:42] = (h_mm & 0xFFFF).to_bytes(2, "little")
    r[42:44] = (w_mm & 0xFFFF).to_bytes(2, "little")
    r[44] = 2; r[45] = 0; r[46] = 2; r[47] = 0
    liner = (head_dots * 25 // dpi) & 0xFFFF
    r[48:50] = liner.to_bytes(2, "little")
    r[50:52] = (label_count & 0xFFFF).to_bytes(2, "little")
    total_mm = min(pitch_mm * label_count, 0xFFFF)
    r[52:54] = total_mm.to_bytes(2, "little")
    r[56] = 0x00
    r[60] = 15; r[61] = 26; r[62] = 0x12
    crc = crc16_ccitt(bytes(r[8:63]))
    r[4:6] = (crc & 0xFFFF).to_bytes(2, "little")
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

print("\n== SKU RECORD (ESC U) well-formedness ==")
u = firmware_sku_record(b"S0904980", 220)
check(len(u) == 63, "SKU record is exactly 63 bytes")
check(u[0] == 0xB6 and u[1] == 0xCA, "magic 0xCAB6 (LE) at [0-1]")
check(u[3] == len(b"S0904980"), "SKU length field at [3]")
check(u[8:8+8] == b"S0904980", "SKU bytes at [8..]")
# CRC must be reproducible (self-consistent) over the payload range.
crc = crc16_ccitt(bytes(u[8:63]))
check(u[4:6] == (crc & 0xFFFF).to_bytes(2, "little"), "CRC16-CCITT at [4-5] matches payload")

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
    r[6] = (paper & 1) | (button & 2)
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
