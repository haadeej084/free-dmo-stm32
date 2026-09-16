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
| `1B 4C` + len(u16) | **ESC L** | "Sets the print engine mode between normal label stock and continuous label stock" (tech ref p.11 — it gives no parameter table). We take a u16: `0` = die-cut (roll sets the pitch, clears any override), a plain dot length is used as the feed pitch. The u16 width comes from the driver GPD literal `<1B>L<0867>` | tech ref p.11; driver GPD |
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
| `1B 24` | **ESC $** | Restore factory settings (config back to defaults). `1B 2A` (`ESC *`) is accepted as an alias | tech ref p.20 |
| `1B 6F` + count(u8) | **ESC o** | Set label count. **One** argument byte: the tech ref's table is `Byte 0 1 2 / 'ESC' 'o' Count`, three bytes total, where `ESC n`/`ESC L` get explicit two-byte tables. If a host does send a u16, its `0x00` high byte is ignored as a stray rather than eaten as a command — the safe direction. Use `GS C` for counts above 255 | tech ref p.20 |
| `1B 71` + tray | **ESC q** | Select output tray — accepted, argument ignored | tech ref p.5 (job structure) |
| `1B 66 01` + n | **ESC f 1 n** | Skip `n` dot lines. Documented in the **450** series tech ref; dropped from the 550 manual but cheap to honour | LW450 tech ref p.10 |
| `1B 40` | **ESC @** | Restart print engine → full pipeline reset here | tech ref p.20 |
| `1B 57` len dir obj(2) + payload | **ESC W** | Control-command framing; 4 header bytes, then `len` payload bytes consumed and ignored. `len` is clamped to 250 | driver |

Unknown bytes outside a command are ignored. An unknown byte *after* `ESC` is
treated as a one-argument command, so one further byte is consumed — that is why
`ESC $` must be spelled exactly. The parser is byte-driven and resumable: if the
ring buffer runs dry mid-raster it continues on the next `protocol_task()`
without losing the job.

**Over-wide rasters.** `ESC D` may declare a height larger than the head
(`H > 1248` / `672`). The surplus bytes of every line are consumed and discarded
rather than clipped away, so the rest of the block stays aligned; only the first
`HEAD_BYTES` are printed. A header whose width or derived bytes-per-line does
not fit in 16 bits is dropped entirely (its length is unknowable) and the parser
resyncs on the next `ESC`.

### 32-byte status struct (ESC A reply)

Layout per tech ref p.13–16; values cross-checked against a live capture
(`capture_long.log`: byte9=`0x64`=100 % density, byte10=`8` OK / `10` counterfeit).

| Byte | Field | Meaning |
|------|-------|---------|
| 0 | PrintStatus | 0 idle, 1 printing, 2 error, 3 cancel, 4 busy, 5 unlock |
| 1–4 | PrintJobID (u32 LE) | Job ID of the ongoing job (from ESC s) |
| 5–6 | LabelIndex (u16 LE) | Label index (from ESC n) |
| 7 | Reserved | 0 |
| 8 | PrintHeadStatus | 0 ok, 1 overheated, 2 status unknown (the manual's default) — we report `0` |
| 9 | PrintDensity (%) | 0–200 (last ESC C / default 100) |
| 10 | MainBayStatus | Full range (tech ref p.14): 0 unknown, 1 bay open, 2 no media, 3 not inserted properly, 4 media present/status unknown, 5 empty, 6 critically low, 7 low, **8 media present – ok**, 9 jammed, 10 **counterfeit media**. Always `8` here |
| 11–22 | SKU info | 12 chars, NUL-padded (the configured SKU) |
| 23–26 | ErrorID (u32) | 0 |
| 27–28 | LabelCount (u16 LE) | Remaining labels (decrements per label) |
| 29 | EPS status | 1 = present |
| 30 | PrintHeadVoltage | 0 unknown, **1 ok**, 2 low, 3 critically low, 4 too low for printing. We report `1`; the genuine engine measures the rail and suspends below 19.3 V, resuming at 21 V (LW450 tech ref p.7) — see PINMAP.md "head voltage sense" |
| 31 | Reserved | `0xFF` |

### ESC U — 63-byte consumable record

Sent on `ESC U`. The geometry is in **mm**, derived from the configured paper's
dot dimensions at the model DPI (25.4 mm/inch, rounded to nearest — see
`protocol.c::dots_to_mm`). The CRC covers bytes 8–62 (the SKU + geometry)
and is stored little-endian at bytes 4–5; the magic/version/length are *not*
covered. See `protocol.c::send_sku_record`.

| Byte | Field | Value / meaning |
|------|-------|-----------------|
| 0–1 | Magic | `0xCAB6` (LE: `B6 CA`) |
| 2 | Version | `0` |
| 3 | SKU length | number of chars in the SKU |
| 4–5 | CRC16-CCITT (LE) | over bytes 8–62 — **assumption** (D12). The manual's own row reads "Byte 7…Byte 4 / b15…b0 / CRC", which is self-contradictory (four bytes, sixteen bits); every other row of that table is consistent, so the likely reading is CRC at 4–5 with 6–7 reserved. A single captured `ESC U` reply from a genuine printer with a real roll would settle it — see FIELDWORK section 1 |
| 6–7 | Reserved | 0 |
| 8–19 | SKU | 12 chars, NUL-padded (the configured SKU) |
| 20 | Brand | `0x00` = DYMO |
| 21 | Region | `0xFF` = global |
| 22 | Material | `0x03` = paper |
| 23 | Label type | `0x01` = die-cut |
| 24 | Label color | `0x01` = white |
| 25 | Content color | `0x00` = black |
| 26 | Marker type | `0x00` = marker-1 front edge gives both the cut location and the start of label (tech ref p.18) |
| 27 | Reserved | 0 |
| 28–29 | Marker pitch (mm) | label length + gap, LE |
| 30–31 | Marker 1 width (mm) | 2 |
| 32–33 | Marker 1 to label start (mm) | 2 |
| 34–37 | Marker 2 | unused (0) |
| 38–39 | Vertical offset (mm) | 1 |
| 40–41 | Label length (mm) | from configured paper, LE |
| 42–43 | Label width (mm) | from configured paper, LE |
| 44–45 | Printable h-offset (mm) | 2 |
| 46–47 | Printable v-offset (mm) | 2 |
| 48–49 | Liner width (mm) | head width in mm, LE |
| 50–51 | Total label count | the **roll's total** (`MODEL_DEFAULT_COUNT`, or the configured count if larger), LE — not the remaining count, which is status bytes 27–28 |
| 52–53 | Total length (mm) | pitch × total count (clamped to 0xFFFF), LE |
| 54–55 | Counter margin | 0 |
| 56 | Counter strategy | `0x00` = "counting up from 0x0000 to amount of labels + counter margin" (tech ref p.19). This describes the tag's own counter; our remaining-label counter in the status struct counts **down** and is independent of this byte |
| 57–59 | Reserved | 0 |
| 60–61 | Production date (DDYY) | day, year |
| 62 | Production time (HHMM low) | hour/minute. The manual lists production time at "Byte 63, Byte 62", which does not fit the 63-byte record it declares two pages earlier; date at 60–61 plus one time byte at 62 is the only self-consistent reading |

### ESC V — 34-byte version reply

Sent on `ESC V`. Two 16-char UTF-8 fields, zero-padded, then the PID.
The **structure of the firmware field is fixed by the manual** (tech ref p.20):
four four-character groups, no separators.

| Byte | Field | Value / meaning |
|------|-------|-----------------|
| 0–15 | Hardware string | 16 chars, zero-padded — `LW5XL-REV.K` (5XL) / `LW550-REV.K` (550) |
| 16–19 | FW kind | `FWAP` = application, `FWBL` = boot loader |
| 20–23 | Major release | 4 chars |
| 24–27 | Minor release | 4 chars |
| 28–31 | Release date | `MMYY` |
| 32–33 | USB PID (LE) | `0x002A` (5XL) / `0x0028` (550) |

We send `FWAP000100010921`. The *values* are still ours (D12) — the host treats
them as informational — but the shape is now the documented one; the earlier
`FWAP01.02.2112` was not a well-formed reply.

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
| `0x01` | n | Strobe the head up to n all-on lines at max density, then restore. Every line is gated on `thermal_ok()` (DECISIONS D7): it waits up to ~1 s per line and stops if the head is still over its limit | 4 B: `'D' sub fired thermal_ok` — `fired` is the number of lines that **actually** fired (< n means the thermal gate stopped it) |
| `0x02` | n | Step the feed motor n dot-lines | 3 B: `'D' sub n` |
| `0x03` | – | EEPROM write/read self-test (scratch: 2-byte `@0x140`, 1-byte `@0x40`) | 3 B: `'D' sub match(1/0)` |
| `0x04` | – | Diagnostic snapshot | 24 B (below) |
| `0x05` | – | Firmware build id (ASCII, from `git describe` at build time; `dev` outside a checkout) | 2–50 B: `'D' sub build[…]`, capped at 48 chars |

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
