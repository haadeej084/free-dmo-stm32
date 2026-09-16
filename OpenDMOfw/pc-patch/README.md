# pc-patch — D.MO roll patch (PC side)

PC-side companion to the **OpenDMOfw** printer firmware in this repo: together they make
**D.MO Connect** accept and drive **any** roll in a LabelWriter 550 / 5XL. OpenDMOfw handles the
printer side (the firmware presents a genuine D.mo device and defeats the roll-tag DRM); this tool
handles the PC side — D.MO Connect's own roll validation in `DYMO.LabelAPI.dll`.

## How D.MO Connect validates rolls (and what gets patched)

D.MO Connect extracts its DLLs to `%TEMP%\.net\<hash>\` on every start and update. The roll check
lives in `DYMO.LabelAPI.dll`, type `LabelWriterRollDetectionPrinterCommunication`. This tool
applies four patches to that DLL (all idempotent — always rebuilt from a pristine `.orig` copy):

| # | Patch | What it does |
|---|-------|--------------|
| A | Catalog | In the embedded SKU catalog, set `Region="Global"` on the active SKU (length-preserving byte edit), keeping its real paper name. |
| B | IL inject | In `UpdatePrinterStatus…::MoveNext`, right after `set_SkuNumber`: if the printer reports an **empty** SKU, force `SkuNumber` / `LabelsRemaining` / `eRollValidity = Valid` from a local flag file. This is what lets a printer that "accepts" the emulated tag but conveys no usable SKU still pass on the PC side. |
| C | ValidateResult | Flip the non-null `return false` sites to `true`, so a printer reporting Error/Counterfeit state still passes validation. |
| D | ExcludedPapers | Blank the active SKU's real paper name in the per-driver exclusion lists, so the paper resolves (otherwise e.g. S0904980 would not resolve, and Connect shows the roll as empty — "Leeg" on a Dutch install). |

## Roll selection — the flag file

`%LOCALAPPDATA%\dmo_roll.flag`, one line: `<SKU> <count>` (e.g. `30387 100`).

- **Flag present** → that SKU/count is injected when the printer reports an empty SKU, and the
  catalog/exclusion patches target that SKU.
- **Flag absent** → built-in default `S0904980` (104×159 mm, biggest 5XL roll) / 220 labels.
- The injected count is read from the flag file **at runtime on every poll**, so switching rolls
  needs no restart — only the catalog re-patch (done automatically).

Known rolls (SKU → nominal full-roll count):

| SKU | Roll | Count |
|-----|------|-------|
| S0904980 | 104×159 mm (biggest 5XL) | 220 |
| S0722430 | 54×101 mm | 220 |
| 30387 | Internet Postage, 2¼×10 in (biggest 550) | 100 |
| 30256 | 59×102 mm shipping (biggest 550) | 300 |
| 11351 | 11×54 mm | 1500 |
| 99017 | 12×50 mm | 220 |
| S0722530 | 13×25 mm | 1000 |
| 11354 | 57×32 mm | 1000 |
| S0947420 | 102×59 mm | 1150 |
| S0947410 | 89×28 mm record | 2100 |

## Using it

**Tray app** (`dmo.exe --tray`): D.mo LabelWriter icon in the system tray. Menu:

- **Insert new roll…** — known SKUs at nominal count, or *Custom…* (SKU + count).
- **Reset counter** — bump the current roll back to full nominal.
- Presets: *Biggest roll — 30387*, *Unlimited — 9999 labels*.
- **Authentic (off)** — clear the flag file.
- **Re-apply patch now**, **Add/Remove from Windows startup**, **Block D.MO Connect updates…**
  (hosts-file entries for the two update CDNs; needs admin).

**Command line:**

```
dmo [patch|restore|set <SKU> <count>|insert [SKU]|reset|off|show|list|watch [ms]|autostart [on|off|status]|updates [on|off|status]|--tray]
```

- `patch` — patch all DLLs found under `%TEMP%\.net` (default with no args).
- `restore` — put the pristine `.orig` copies back.
- `watch [ms]` — keep re-patching every N ms (default 500); the tray app runs this in the background.
  D.MO Connect re-extracts its DLLs on start/update, so watch mode is what keeps the patch alive.
- `show` / `list` — status of found DLLs and the flag; known SKUs.

## Which path needs what

Two printer-side approaches live in this repo. Do not mix the tables.

**OpenDMOfw** (this tree: replace the F072 image once the chip is writable) reports a
catalog SKU + count itself (`MainBayStatus = 8`). With the compiled defaults
(`30387` / `S0904980`) D.MO Connect usually accepts the roll **without** this
patch. Use the patch if Connect still shows JOKER/empty, or if you want a SKU
that is not in the catalog (patch A/B/D).

**Bluepill I2C tag emulator** (`free-dmo-stm32` proper, stock F072 left in place):

| Board | Printer side (Bluepill emu) | PC side (this tool) |
|-------|-----------------------------|---------------------|
| Rev E | tag emulation | not needed |
| Rev H | tag emulation + WP blob | not needed |
| Rev I | bluepill emu (v1.1.1) [+ WP blob] | **IL injection is the load-bearing piece** — the printer accepts the tag but conveys no usable SKU, so the empty-SKU fill-in completes the job |
| Rev K | tag emulation + learn step + WP blob | optional (fixes the "JOKER" roll display) |

IL injection alone (without a printer that reports a valid bay/SKU) does not work —
the tag-presence check is enforced in the printer's own MCU firmware, not in D.MO Connect.

## Building from source

- **Requirements:** Windows, .NET SDK (any recent), .NET Framework 4.8 reference assemblies
  (shipped with Windows 10/11).
- **App:** `cd src && dotnet build -c Release` → `bin\Release\dmo.exe` (+ `dnlib.dll`,
  `dmo.exe.config`). The app icon is `dmo.ico` (the D.mo LabelWriter 550 photo).
- **Tests:** `cd test && dotnet run -c Release` (exit 0 = all pass).

Dependencies: [dnlib](https://github.com/0xd4d/dnlib) 3.1.0 (MIT) for the IL patching.

## Tests

`test/` builds a **synthetic assembly** with the same shapes the patcher anchors
on (`LabelWriterRollDetectionPrinterCommunication`, a nested `UpdatePrinterStatus`
state machine with `MoveNext`, `set_SkuNumber` fed by `get_InsertedSKU`, a
`set_LabelsRemaining` call, an `ERollValidity` local) and runs the real patcher
against it — no `DYMO.LabelAPI.dll` needed, so it runs in CI.

The assertions are about **branch wiring**, not just "patching succeeded". Two
label captures used to read back the wrong instruction, which made the flag file
a silent no-op: `File.Exists == true` jumped straight to the unconditional
branch (skipping the parse block, leaving the SKU local `null`), and a
*successful* `int.TryParse` jumped to "load default count", discarding the value
it had just parsed. Both produced a DLL that loaded and ran perfectly — only the
feature was gone, and `dmo show` still reported the flag correctly because it
reads the file itself. Reintroducing either bug now fails the suite.

## Notes / limitations

- Requires D.MO Connect to be installed (it patches the DLLs *it* extracts).
- Pristine copies are kept as `DYMO.LabelAPI.dll.orig` next to each patched DLL; `dmo restore`
  reverts everything.
- The IL injection anchors on dynamic method/property names (`set_SkuNumber`,
  `get_InsertedSKU`, …) rather than hardcoded offsets, so it tracks small D.MO Connect updates;
  a large rewrite of that type would need the anchors revisited.
- "Block D.MO Connect updates" edits `%SystemRoot%\system32\drivers\etc\hosts` (admin needed);
  reversible with `dmo updates off`.
