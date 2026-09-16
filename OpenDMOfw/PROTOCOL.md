# PROTOCOL — OpenDMOfw (genuine D.mo LabelWriter 550/5XL wire protocol)

OpenDMOfw does **not** invent a protocol. It implements the real D.mo host
protocol exactly as published in D.mo's *LabelWriter 550 Series Printers Technical
Reference Manual* and as emitted by the decompiled stock driver, so D.MO Connect
(or any host) can drive it unchanged.

The device is a **USB Printer Class** device (interface class 7, subclass 1,
protocol 2 = bidirectional). The OS binds its generic printer driver
(`usbprint` on Windows), and the vendor spooler driver + port monitor take over
the wire. Bulk **OUT** (`0x02`) carries commands/raster; bulk **IN** (`0x82`)
carries replies (status, SKU record, version).

## USB identity (per model, `src/model.h`)

| Field | 5XL (default, OP104) | 550 (`MODEL=OP57`) |
|-------|----------------------|--------------------|
| idVendor | `0x0922` (D.mo) | `0x0922` |
| idProduct | `0x002A` | `0x0028` |
| Manufacturer | `DYMO` | `DYMO` |
| Product | `DYMO LabelWriter 5XL` | `DYMO LabelWriter 550` |
| Serial | 12 decimal digits from the MCU UID (unique per chip) | same |
| Endpoints | bulk IN `0x82` (listed first), bulk OUT `0x02` | same |
| IEEE-1284 ID | `MFG:DYMO;CMD: ;MDL:LabelWriter 5XL;CLASS:PRINTER;DESCRIPTION:DYMO LabelWriter 5XL;` | same with `550` |

The `MFG`+`MDL` pair is what makes Windows derive the hardware ID
`USBPRINT\<MFG+MDL, spaces to underscores, cut to 20><4-char OS CRC>`, here
`USBPRINT\DYMOLabelWriter_5XLB920` / `...550C80D` - exactly the IDs DYMO's own
`DYMO_LW5xx.inf` binds (that INF has no compatible IDs, so the other 1284 keys
cannot affect binding). The key layout follows the published LabelWriter 450
family string; the genuine 550/5XL string has not been captured. Microsoft
warns its CRC "may not match ... any other CRC algorithm", so the bench check is
`setupapi.dev.log` (FIELDWORK section 5). Head widths (1248 / 672 dots) come from the tech reference and
the driver GPDs' `MaxPrintableWidth`.

## Class requests (USB Printer Class 1.1)

| bRequest | Name | Dir | Reply |
|----------|------|-----|-------|
| 0 | GET_DEVICE_ID | IN (`0xA1`) | 2-byte BE length + IEEE-1284 string |
| 1 | GET_PORT_STATUS | IN (`0xA1`) | 1 byte: bit3 no-error, bit4 select, bit5 paper-out |
| 2 | SOFT_RESET | OUT (`0x21`, `0x23` also accepted) | reset the print pipeline, drop a queued bulk-IN reply and clear an IN stall (DTOG untouched) |

## Data commands (bulk OUT)

Multi-byte fields are little-endian unless noted — **`ESC L` is the exception and
is big-endian**. `n` = 1 byte, `n1 n2` = u16 LE, `n1..n4` = u32 LE.

| Bytes | Name | Meaning | Source |
|-------|------|---------|--------|
| `1B 73` + JobID(u32) | **ESC s** | Start of print job (mandatory; ID echoed in status) | tech ref p.11 |
| `1B 4C` + len(**u16 BE**) | **ESC L** | "Sets the print engine mode between normal label stock and continuous label stock" (tech ref p.11 — it gives no parameter table). We take a u16 **MSB-first**: `0` = die-cut (roll sets the pitch, clears any override), a plain dot length is the feed pitch. Three independent sources for the byte order: DYMO's own CUPS driver writes `(v>>8)` then `v&0xff` and pins it in a unit test; Microsoft's GPD rule makes `<1B>L<0867>` emit bytes `08 67` in that order; and read big-endian, 12 of 14 LW5XX.GPD entries are exactly `height_dots + 300` where byte-swapped they are noise | CUPS driver `SendLabelLength`; GPD; LW5XX.GPD |
| `1B 68` / `1B 69` | **ESC h / i** | Text / graphics output mode | tech ref p.11 |
| `1B 54` + speed | **ESC T** | Speed: `0x10` normal, `0x20` high. The tech ref prints `1B 74`, a typo: `0x74` is `t`; the driver GPD sends `<1B>T` | tech ref p.11; LW5XX.GPD |
| `1B 6E` + idx(u16) | **ESC n** | Set label index (echoed in status) | tech ref p.12 |
| `1B 44` + BPP Align W(u32) H(u32) + data | **ESC D** | Start of label print data: **W = number of lines**, **H = number of dots**; then `W × ⌈H·BPP/8⌉` bytes. Driver sends `BPP=1, Align=2` (2 = bottom, the only value the manual documents, and what genuine driver captures carry). MSB of the first byte = leftmost dot | tech ref p.12 |
| `1B 47` | **ESC G** | Feed to print head (short form feed, between labels) | tech ref p.13 |
| `1B 45` | **ESC E** | Feed to tear position (long form feed) | tech ref p.13 |
| `1B 51` | **ESC Q** | End of print job (releases the lock) | tech ref p.13 |
| `1B 41` + lock | **ESC A** | Request status → 32-byte struct on bulk-IN. Lock: 0 read, 1 lock, 2 no-lock-multiple | tech ref p.13 |
| `1B 43` + duty | **ESC C** | Print density, `0–200` % (0 = off); echoed in status byte 9. The Windows driver's density ladder is `0x4B`/`0x58`/`0x64`/`0x71` (75/88/100/113 %), mirroring the ESC c/d/e/g presets below | tech ref p.16; capture byte9=0x64; LW5XX.GPD |
| `1B 63` / `1B 64` / `1B 65` / `1B 67` | **ESC c / d / e / g** | Zero-argument print-density presets: Light 75 %, Medium 87.5 %, Normal 100 %, Dark 112.5 %. The CUPS driver emits one of these per job for the PPD's darkness choice. **`ESC d` was previously our feed backdoor** — a collision that would have eaten the next command's `ESC`; the feed now lives on `GS D 0x02` and `ESC f 1 n` | LW450 tech ref p.19; CUPS driver `SetPrintDensity` |
| `1B 4D` + 8 bytes | **ESC M** | Media-type descriptor (`mtDefault` = 8 zero bytes); always sent by the driver, consumed + ignored | decompiled driver |
| `1B 55` | **ESC U** | Get SKU info → 63-byte consumable record (below) | tech ref p.16 |
| `1B 56` | **ESC V** | Get version → 34-byte reply (below) | tech ref p.20 |
| `1B 24` | **ESC $** | Restore factory settings (config back to defaults). `1B 2A` (`ESC *`) is accepted as an alias | tech ref p.20 |
| `1B 6F` + count(u8) | **ESC o** | Set label count. **One** argument byte: the tech ref's table is `Byte 0 1 2 / 'ESC' 'o' Count`, three bytes total, where `ESC n`/`ESC L` get explicit two-byte tables. If a host does send a u16, its `0x00` high byte is ignored as a stray rather than eaten as a command — the safe direction. Use `GS C` for counts above 255 | tech ref p.20 |
| `1B 71` + ID | **ESC q** | "Select output tray" on the 550 ("will be supported by LW550 Twin Turbo"). On the 450 the same opcode is "Select Roll (Twin Turbo only)" with an **ASCII digit** argument: `0x30` '0' automatic, `0x31` '1' left, `0x32` '2' right - not binary 0/1/2. One roll here, so the byte is accepted and ignored under either encoding | tech ref p.5; LW450 tech ref p.16 |
| `1B 79` / `1B 7A` | **ESC y / z** | 400-series "set print resolution" 300x300 / 203x300. **Zero-argument**; accepted and ignored - this family is 300x300 in every mode, so the only point is not to eat the next command | LW400 tech ref p.19 |
| `1B 66 01` + n | **ESC f 1 n** | Skip `n` dot lines. Documented in the **450** series tech ref; dropped from the 550 manual but cheap to honour | LW450 tech ref p.10 |
| `1B 40` | **ESC @** | Restart print engine → full pipeline reset here | tech ref p.20 |
| `1B 57` len dir obj(2) + payload | **ESC W** | Control-command framing; 4 header bytes, then `len` payload bytes consumed and ignored. `len` is clamped to 250 | driver |

Unknown bytes outside a command are ignored. An unknown byte *after* `ESC` is
treated as a one-argument command, so one further byte is consumed — that is why
`ESC $` must be spelled exactly. **Exception: a further `ESC`.** A run of bare
`ESC` bytes collapses to one pending escape, so the parser resynchronises on the
first real opcode whatever the run length. DYMO's CUPS driver sends such a run
at the start of every document (156 bytes in the code, 100 in its own unit
test); no DYMO command is `ESC ESC`.

**`ESC L` sentinels.** The value is a length, but two values are not: `7F 00`
(custom size) and `FF FF` (continuous stock). Both clear the length override and
take the label pitch from the raster height of the page just printed.

**Label counter vs. eject (deliberate deviation).** On a genuine 550 the roll
tag's counter advances on every label **eject**, including a feed with nothing
printed (EEVblog "Dymo 550 Thermal Printer DRM Hacking", reply #25ff., bench
observation). Our count decrements only on a completed raster block, so a bare
`ESC E` / `ESC G` / button feed leaves it unchanged. There is no tag here, the
count is configuration (D14), and the genuine over-count exists only to enforce
the DRM. Whether status bytes 27-28 follow a bare feed on a genuine unit is
unverified. The parser is byte-driven and resumable: if the
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

This layout is no longer read off the manual alone. **The root of this very
repository embeds 37 dumps of genuine DYMO 550-series roll tags**
(`Src/main.c`, the Bluepill tag emulator), and the consumable record sits at
tag offset 12 in every one. Checking each field against all 37 settled what the
manual leaves ambiguous — and corrected several things it states outright.

Geometry is in **tenths of a millimetre**, not millimetres: SKU 30256 carries
`1016 × 587`, i.e. exactly 4″ × 2.3125″. See `protocol.c::send_sku_record`.

| Byte | Field | Value / meaning |
|------|-------|-----------------|
| 0–1 | Magic | `0xCAB6` (LE: `B6 CA`) |
| 2 | Version | `0` |
| 3 | **Payload length** | `0x3C` = 60, **constant on 37/37** regardless of SKU length. An 8-byte header plus 60 payload bytes; the next record's magic begins at offset 68 on every tag. The manual's "Length" row invites reading this as the SKU length — it is not |
| 4–7 | **CRC-32 (LE)** | CRC-32/ISO-HDLC (zlib: poly `0x04C11DB7` reflected, init/xorout `0xFFFFFFFF`) over **bytes 0–59 with 4–7 zeroed**. Reproduces on **37/37** genuine tags. The manual's row reads "Byte 7…Byte 4 / b15…b0 / CRC" — right about the four-byte span, wrong about the width |
| 8–19 | SKU | 12 chars, NUL-padded (the configured SKU) |
| 20 | Brand | `0x00` = DYMO |
| 21 | Region | `0xFF` = global |
| 22 | Material | Real tags use `0x02/0x04/0x06/0x08` and `0x20/0x23/0x24/0x25/0x26`. The manual's `0x00–0x07` enum does not describe them, and its "paper" value `0x03` appears on **none** of the 37. We send `0x04`, what our default SKU S0904980 carries |
| 23 | Label type | `0x01` = die-cut (33/37; 3 are `0x02` card, 1 is `0x00` continuous) |
| 24 | Label color | `0x01` = white |
| 25 | Content color | `0x00` = black |
| 26 | Marker type | `0x00` |
| 27 | Reserved | 0 |
| 28–29 | Marker pitch (0.1 mm) | label length + gap. Genuine gaps run 42–118 tenths depending on stock (mode 42); we use 42 |
| 30–31 | Marker 1 width (0.1 mm) | `30` = 3.0 mm (35/37) |
| 32–33 | Marker 1 to label start (0.1 mm) | per-roll, 15–88; we send `38`, the S0904980 value |
| 34–37 | Marker 2 | unused (0) |
| 38–39 | Vertical offset (0.1 mm) | `16` = 1.6 mm (23/37) |
| 40–41 | Label length (0.1 mm) | from the configured paper, LE |
| 42–43 | Label width (0.1 mm) | from the configured paper, LE |
| 44–47 | Printable-area offsets | **0 on 37/37** |
| 48–49 | Liner width (0.1 mm) | head width. Genuine liners are a little wider than the head (1075 vs our 1057 on the 5XL), so this is our best available approximation |
| 50–51 | Total label count | the roll's total, LE — not the remaining count, which is status bytes 27–28 |
| 52–53 | Total media length, **2 mm units** | `(pitch × total)/20`. The manual says "length in mm"; the continuous roll 30270 carries `45720`, and 45720 × 2 mm = 91440 mm = exactly 300 ft — a length no u16 could hold in mm |
| 54–55 | Counter margin | `total / 10` (36/37; the 37th is a known mis-copied dump) |
| 56 | Counter strategy | **`0x01`** on 37/37, not the `0x00` the manual describes |
| 57–62 | Padding | 0. Genuine tags carry nothing past byte 59, and the manual's "production date/time" rows have no counterpart in real data |

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
| `0x06` | – | **Scan**: sample every ADC-capable pin and read the input level of every pin on ports A/B/C. Drops the 24 V rail first, because sampling briefly floats pins — including the strobes | 29 B (below) |
| `0x07` | port pin n | **Toggle** port `p` (0=A, 1=B, 2=C) pin `n`, `n` times at ~1 ms per half period, then restore its mode. PA11/PA12 (USB) and PA13/PA14 (SWD) are refused | 3 B: `'D' sub done(1/0)` |
| `0x08` | 0 or 1 | Clear/set **`OP_FLAG_VH_INHIBIT`**, the hard interlock on the 24 V heat rail, and persist it. Setting it drops the rail immediately | 3 B: `'D' sub flags` |

Snapshot (24 B): `[0]'D' [1]sub [2]model id (PID low) [3-4]thermistor raw u16 BE
[5]thermal_ok [6]bit0 paper present / bit1 button pressed [7]density % [8]flags
[9-20]SKU (12 B, NUL-padded) [21-22]label count u16 LE [23]head bytes/line`.

### GS D 0x06 — scan (29 B)

`[0]'D' [1]0x06 [2-21] ADC IN0..IN9, u16 **BE** each [22-23] GPIOA IDR (u16 LE)
[24-25] GPIOB IDR [26-27] GPIOC IDR [28] bit0 VH on, bit1 VH inhibited`

ADC channels map to pins: **IN0–IN7 = PA0–PA7, IN8 = PB0, IN9 = PB1** — the only
ADC-capable pins on this part. Warm the head and diff two scans to find the
thermistor; block the top-of-form sensor and diff to find the photocell. That is
two of the seven fieldwork measurements answered by reading a table instead of
tracing a net.

Host: `opsend.py diag 4` (snapshot), `diag 1 8`, `diag 2 30`, `diag 3`,
`diag 6` (scan), `diag 7 1 4 20` (toggle PB4), `opsend.py vh off` (interlock).
Layouts verified in `test/test_protocol.c` and `test/test_protocol_wire.py`.

## Example job (one full-width black label, 5XL, 4x6 shipping)

The Windows driver's DOC_SETUP emits its preamble in GPD order: density
(`ESC C`), quality (`ESC h` / `ESC i`), speed (`ESC T`), then paper (`ESC L`) -
and for continuous stock a second `ESC L FF FF`.

```
1B 73 01 00 00 00      # ESC s, JobID=1
1B 43 64               # ESC C, density 100 %
1B 68                  # ESC h, text quality (ESC i = graphics)
1B 54 10               # ESC T, normal speed
1B 4C 08 67            # ESC L, 0x0867 big-endian (Shipping 4x6)
1B 6E 01 00            # ESC n, label index 1
1B 44 01 02 <W u32> <H u32> <data>   # ESC D, BPP=1 Align=2, W=lines H=1248 dots, 156 B/line
1B 47                  # ESC G, short feed
1B 45                  # ESC E, feed to tear
1B 51                  # ESC Q, end job
```

`tools/opsend.py` sends the same command set (plus `ESC M` with 8 zero bytes,
as the decompiled driver does) and can send a test pattern or a PNG.
