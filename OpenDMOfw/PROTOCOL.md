# PROTOCOL — OpenDMOfw (genuine D.mo LabelWriter 550/5XL wire protocol)

OpenDMOfw does **not** invent a protocol. It implements the real D.mo host
protocol exactly as published in D.mo's *LabelWriter 550 Series Printers Technical
Reference Manual* and as emitted by the decompiled stock driver, so D.MO Connect
(or any host) can drive it unchanged.

The device is a **USB Printer Class** device (interface class 7, subclass 1,
protocol 2 = bidirectional). The OS binds its generic printer driver
(`usbprint` on Windows), and the vendor spooler driver + port monitor take over
the wire. Bulk **OUT** (`0x01`) carries commands/raster; bulk **IN** (`0x81`)
carries replies (status, SKU record, version).

## USB identity (per model, `src/model.h`)

| Field | 5XL (default, OP104) | 550 (`MODEL=OP57`) |
|-------|----------------------|--------------------|
| idVendor | `0x0922` (D.mo) | `0x0922` |
| idProduct | `0x002A` | `0x0028` |
| Manufacturer | `DYMO` | `DYMO` |
| Product | `LabelWriter 5XL` | `LabelWriter 550` |
| Serial | 12 decimal digits from the MCU UID (unique per chip) | same |
| IEEE-1284 ID | `MFG:DYMO;MDL:LabelWriter 5XL;CID:DYMOLabelWriter_5XLB;CLS:PRINTER;DES:...` | `...MDL:LabelWriter 550;CID:DYMOLabelWriter_550B;...` |

The `MFG`+`MDL` pair is what makes Windows derive the genuine driver-model match
ID (`USBPRINT\DYMOLabelWriter_5XLB920` / `...550C80D`) that D.mo's own driver
package expects. Head widths (1248 / 672 dots) come from the tech reference and
the driver GPDs' `MaxPrintableWidth`.

## Class requests (USB Printer Class 1.1)

| bRequest | Name | Dir | Reply |
|----------|------|-----|-------|
| 0 | GET_DEVICE_ID | IN (`0xA1`) | 2-byte BE length + IEEE-1284 string |
| 1 | GET_PORT_STATUS | IN (`0xA1`) | 1 byte: bit3 no-error, bit4 select, bit5 paper-out |
| 2 | SOFT_RESET | OUT (`0x21`) | reset the print pipeline |

## Data commands (bulk OUT)

Multi-byte fields are little-endian unless noted. `n` = 1 byte, `n1 n2` = u16 LE,
`n1..n4` = u32 LE.

| Bytes | Name | Meaning | Source |
|-------|------|---------|--------|
| `1B 73` + JobID(u32) | **ESC s** | Start of print job (mandatory; ID echoed in status) | tech ref p.11 |
| `1B 4C` + len(u16) | **ESC L** | Set max label length (dots). `0` = die-cut (tag sets pitch); a plain dot length is used as the feed pitch | tech ref p.11; decompiled driver |
| `1B 68` / `1B 69` | **ESC h / i** | Text / graphics output mode | tech ref p.11 |
| `1B 74` + speed | **ESC T** | Speed: `0x10` normal, `0x20` high | tech ref p.11 |
| `1B 6E` + idx(u16) | **ESC n** | Set label index (echoed in status) | tech ref p.12 |
| `1B 44` + BPP Align W(u32) H(u32) + data | **ESC D** | Start of label print data: **W = number of lines**, **H = number of dots**; then `W × ⌈H·BPP/8⌉` bytes. Driver sends `BPP=1, Align=0x80`. MSB of the first byte = leftmost dot | tech ref p.12; decompiled driver |
| `1B 47` | **ESC G** | Feed to print head (short form feed, between labels) | tech ref p.13 |
| `1B 45` | **ESC E** | Feed to tear position (long form feed) | tech ref p.13 |
| `1B 51` | **ESC Q** | End of print job (releases the lock) | tech ref p.13 |
| `1B 41` + lock | **ESC A** | Request status → 32-byte struct on bulk-IN. Lock: 0 read, 1 lock, 2 no-lock-multiple | tech ref p.13 |
| `1B 43` + duty | **ESC C** | Print density, `0–200` % (0 = off); echoed in status byte 9 | tech ref p.16; capture byte9=0x64 |
| `1B 65` | **ESC e** | Reset print density to default (100 %) — the driver's "Normal" | tech ref p.16; decompiled driver |
| `1B 4D` + 8 bytes | **ESC M** | Media-type descriptor (`mtDefault` = 8 zero bytes); always sent by the driver, consumed + ignored | decompiled driver |
| `1B 55` | **ESC U** | Get SKU info → 63-byte consumable record (below) | tech ref p.16 |
| `1B 56` | **ESC V** | Get version → 34-byte reply (below) | tech ref p.20 |
| `1B 24` | **ESC \*** | Restore factory settings (config back to defaults) | tech ref p.20 |
| `1B 6F` + count(u16) | **ESC o** | Set label count | tech ref p.20 |
| `1B 40` | **ESC @** | Restart print engine → full pipeline reset here | tech ref p.20 |
| `1B 57` len dir obj(2) + payload | **ESC W** | Control-command framing; payload consumed, ignored | driver |

Unknown bytes outside a command are ignored. The parser is byte-driven and
resumable: if the ring buffer runs dry mid-raster it continues on the next
`protocol_task()` without losing the job.

### 32-byte status struct (ESC A reply)

Layout per tech ref p.13–16; values cross-checked against a live capture
(`capture_long.log`: byte9=`0x64`=100 % density, byte10=`8` OK / `10` counterfeit).

| Byte | Field | Meaning |
|------|-------|---------|
| 0 | PrintStatus | 0 idle, 1 printing, 2 error, 3 cancel, 4 busy, 5 unlock |
| 1–4 | PrintJobID (u32 LE) | Job ID of the ongoing job (from ESC s) |
| 5–6 | LabelIndex (u16 LE) | Label index (from ESC n) |
| 7 | Reserved | 0 |
| 8 | PrintHeadStatus | 0 ok, 1 overheated, 2 unknown |
| 9 | PrintDensity (%) | 0–200 (last ESC C / default 100) |
| 10 | MainBayStatus | 2 no-media, **8 ok**, 10 counterfeit — always `8` here |
| 11–22 | SKU info | 12 chars, NUL-padded (the configured SKU) |
| 23–26 | ErrorID (u32) | 0 |
| 27–28 | LabelCount (u16 LE) | Remaining labels (decrements per label) |
| 29 | EPS status | 1 = present |
| 30 | PrintHeadVoltage | 0 unknown, 1 ok |
| 31 | Reserved | `0xFF` |

### ESC U — 63-byte consumable record

Sent on `ESC U`. The geometry is in **mm**, derived from the configured paper's
dot dimensions at the model DPI. The CRC covers bytes 8–62 (the SKU + geometry)
and is stored little-endian at bytes 4–5; the magic/version/length are *not*
covered. See `protocol.c::send_sku_record`.

| Byte | Field | Value / meaning |
|------|-------|-----------------|
| 0–1 | Magic | `0xCAB6` (LE: `B6 CA`) |
| 2 | Version | `0` |
| 3 | SKU length | number of chars in the SKU |
| 4–5 | CRC16-CCITT (LE) | over bytes 8–62 — **assumption** (D12) |
| 6–7 | Reserved | 0 |
| 8–19 | SKU | 12 chars, NUL-padded (the configured SKU) |
| 20 | Brand | `0x00` = DYMO |
| 21 | Region | `0xFF` = global |
| 22 | Material | `0x03` = paper |
| 23 | Label type | `0x01` = die-cut |
| 24 | Label color | `0x01` = white |
| 25 | Content color | `0x00` = black |
| 26 | Marker type | `0x00` = none |
| 27 | Reserved | 0 |
| 28–29 | Label pitch (mm) | label length + gap, LE |
| 30–31 | Marker 1 width (mm) | 2 |
| 32–33 | Marker 1 to label start (mm) | 2 |
| 34–37 | Marker 2 | unused (0) |
| 38–39 | Vertical offset (mm) | 1 |
| 40–41 | Label length (mm) | from configured paper, LE |
| 42–43 | Label width (mm) | from configured paper, LE |
| 44–45 | Printable h-offset (mm) | 2 |
| 46–47 | Printable v-offset (mm) | 2 |
| 48–49 | Liner width (mm) | head width in mm, LE |
| 50–51 | Total label count | configured count, LE |
| 52–53 | Total length (mm) | pitch × count (clamped to 0xFFFF), LE |
| 54–55 | Counter margin | 0 |
| 56 | Counter strategy | `0x00` = count up from 0 |
| 57–59 | Reserved | 0 |
| 60–61 | Production date (DDYY) | day, year |
| 62 | Production time (HHMM low) | hour/minute |

### ESC V — 34-byte version reply

Sent on `ESC V`. The two strings are 16 chars each, zero-padded. The exact
string values are an **assumption** (D12) — kept consistent with the model's
PID/MDL so a 5XL reports a 5XL hardware string and a 550 a 550 one.

| Byte | Field | Value / meaning |
|------|-------|-----------------|
| 0–15 | Hardware string | 16 chars, zero-padded — `LW5XL-REV.K` (5XL) / `LW550-REV.K` (550) |
| 16–31 | Firmware string | 16 chars, zero-padded — `FWAP01.02.2112` |
| 32–33 | USB PID (LE) | `0x002A` (5XL) / `0x0028` (550) |

## Backdoor commands (never sent by the stock host)

Kept for configuration and driver-less bring-up via `tools/opsend.py`:

| Bytes | Name | Meaning |
|-------|------|---------|
| `1D 43` len lo hi sku[len] | **GS C** | Set roll config in EEPROM: `label_count = hi<<8\|lo`, then `len` SKU bytes |
| `1B 64` n | **ESC d** | Feed `n` dot lines |
| `1D 44` sub [arg] | **GS D** | Self-test / diagnostic (see below) |

### GS D — self-test / diagnostic

Replies are `'D'`-prefixed so they can't be mistaken for a 32-byte status struct.
The stock host never sends `GS D`, so this cannot collide with the genuine protocol.

| sub | arg | Action | Reply (len) |
|-----|-----|--------|-------------|
| `0x01` | n | Strobe the head n all-on lines at max density, then restore | 4 B: `'D' sub n thermal_ok` |
| `0x02` | n | Step the feed motor n dot-lines | 3 B: `'D' sub n` |
| `0x03` | – | EEPROM write/read self-test (scratch area @ offset 64) | 3 B: `'D' sub match(1/0)` |
| `0x04` | – | Diagnostic snapshot | 24 B (below) |

Snapshot (24 B): `[0]'D' [1]sub [2]model id (PID low) [3-4]thermistor raw u16 BE
[5]thermal_ok [6]bit0 paper present / bit1 button pressed [7]density % [8]flags
[9-20]SKU (12 B, NUL-padded) [21-22]label count u16 LE [23]head bytes/line`.

Host: `python tools/opsend.py --model OP57 diag 4` (snapshot), `diag 1 8`,
`diag 2 30`, `diag 3`. Layout verified in `test/test_protocol_wire.py`.

## Example job (one full-width black label, 550)

```
1B 73 01 00 00 00      # ESC s, JobID=1
1B 65                  # ESC e, density normal (100 %)
1B 69                  # ESC i, graphics mode
1B 4D 00*8             # ESC M, media type (mtDefault)
1B 4C 00 00            # ESC L, die-cut (tag/length 0)
1B 6E 01 00            # ESC n, label index 1
1B 44 01 80 <W u32> <H u32> <data>   # ESC D, W=lines H=672 dots, 84 B/line
1B 47                  # ESC G, short feed
1B 45                  # ESC E, feed to tear
1B 51                  # ESC Q, end job
```

`tools/opsend.py` emits exactly this sequence (byte-matched to the decompiled
driver) and can send a test pattern or a PNG.
