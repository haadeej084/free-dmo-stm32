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
| D | ExcludedPapers | Blank the active SKU's real paper name in the per-driver exclusion lists, so the paper resolves (otherwise e.g. S0904980 would not resolve → "Leeg"/empty). |

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

## Which board needs what

| Board | Printer side (OpenDMOfw) | PC side (this tool) |
|-------|--------------------------|---------------------|
| Rev E | tag emulation | not needed |
| Rev H | tag emulation + WP blob | not needed |
| Rev I | bluepill emu (v1.1.1) [+ WP blob] | **IL injection is the load-bearing piece** — the printer accepts the tag but conveys no usable SKU, so the empty-SKU fill-in completes the job |
| Rev K | tag emulation + learn step + WP blob | optional (fixes the "JOKER" roll display) |

Note: IL injection alone (without the printer-side emulation) does not work — the tag-presence
check is enforced in the printer's own MCU firmware, not in D.MO Connect.

## Building from source

- **Requirements:** Windows, .NET SDK (any recent), .NET Framework 4.8 reference assemblies
  (shipped with Windows 10/11).
- **App:** `cd src && dotnet build -c Release` → `bin\Release\dmo.exe` (+ `dnlib.dll`,
  `dmo.exe.config`). The app icon is `dmo.ico` (the D.mo LabelWriter 550 photo).

Dependencies: [dnlib](https://github.com/0xd4d/dnlib) 3.1.0 (MIT) for the IL patching.

## Notes / limitations

- Requires D.MO Connect to be installed (it patches the DLLs *it* extracts).
- Pristine copies are kept as `DYMO.LabelAPI.dll.orig` next to each patched DLL; `dmo restore`
  reverts everything.
- The IL injection anchors on dynamic method/property names (`set_SkuNumber`,
  `get_InsertedSKU`, …) rather than hardcoded offsets, so it tracks small D.MO Connect updates;
  a large rewrite of that type would need the anchors revisited.
- "Block D.MO Connect updates" edits `%SystemRoot%\system32\drivers\etc\hosts` (admin needed);
  reversible with `dmo updates off`.
