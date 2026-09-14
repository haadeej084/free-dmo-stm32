# pc-patch — DYMO roll patch (PC side)

PC-side companion to [free-dmo](https://github.com/js1000/free-dmo): makes **DYMO Connect** accept
**any** roll in a LabelWriter 550 / 5XL. Where free-dmo handles the printer side (defeating the
roll-tag DRM in firmware), this tool handles the PC side — DYMO Connect's own roll validation in
`DYMO.LabelAPI.dll`.

## How DYMO Connect validates rolls (and what gets patched)

DYMO Connect extracts its DLLs to `%TEMP%\.net\<hash>\` on every start and update. The roll check
lives in `DYMO.LabelAPI.dll`, type `LabelWriterRollDetectionPrinterCommunication`. This tool
applies four patches to that DLL (all idempotent — always rebuilt from a pristine `.orig` copy):

| # | Patch | What it does |
|---|-------|--------------|
| A | Catalog | In the embedded SKU catalog, set `Region="Global"` on the active SKU (length-preserving byte edit), keeping its real paper name. |
| B | IL inject | In `UpdatePrinterStatus…::MoveNext`, right after `set_SkuNumber`: if the printer reports an **empty** SKU, force `SkuNumber` / `LabelsRemaining` / `eRollValidity = Valid` from a local flag file. This is what lets a printer that "accepts" the emulated tag but conveys no usable SKU still pass on the PC side. |
| C | ValidateResult | Flip the non-null `return false` sites to `true`, so a printer reporting Error/Counterfeit state still passes validation. |
| D | ExcludedPapers | Blank the active SKU's real paper name in the per-driver exclusion lists, so the paper resolves (otherwise e.g. S0904980 would not resolve → "Leeg"/empty). |

## Roll selection — the flag file

`%LOCALAPPDATA%\dymo_roll.flag`, one line: `<SKU> <count>` (e.g. `30387 100`).

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

**Installer (recommended):** `installer/Output/dymo-roll-patch-setup.exe` — per-user install
(no admin needed), Inno Setup. Options: *Start automatically when I log in* (HKCU `Run` key,
checked by default) and *Launch now*. Uninstall removes the flag file and the Run entry.

**Tray app** (`dymo.exe --tray`): rocket icon in the system tray. Menu:

- **Insert new roll…** — known SKUs at nominal count, or *Custom…* (SKU + count).
- **Reset counter** — bump the current roll back to full nominal.
- Presets: *Biggest roll — 30387*, *Unlimited — 9999 labels*.
- **Authentic (off)** — clear the flag file.
- **Re-apply patch now**, **Add/Remove from Windows startup**, **Block DYMO Connect updates…**
  (hosts-file entries for the two update CDNs; needs admin).

**Command line:**

```
dymo [patch|restore|set <SKU> <count>|insert [SKU]|reset|off|show|list|watch [ms]|autostart [on|off|status]|updates [on|off|status]|--tray]
```

- `patch` — patch all DLLs found under `%TEMP%\.net` (default with no args).
- `restore` — put the pristine `.orig` copies back.
- `watch [ms]` — keep re-patching every N ms (default 500); the tray app runs this in the background.
  DYMO Connect re-extracts its DLLs on start/update, so watch mode is what keeps the patch alive.
- `show` / `list` — status of found DLLs and the flag; known SKUs.

## Which board needs what

| Board | Printer side (free-dmo) | PC side (this tool) |
|-------|--------------------------|---------------------|
| Rev E | free-dmo emu | not needed |
| Rev H | free-dmo emu + WP blob | not needed |
| Rev I | bluepill emu (v1.1.1) [+ WP blob] | **IL injection is the load-bearing piece** — the printer accepts the tag but conveys no usable SKU, so the empty-SKU fill-in completes the job |
| Rev K | free-dmo emu + learn step + WP blob | optional (fixes the "JOKER" roll display) |

Note: IL injection alone (without the printer-side emulation) does not work — the tag-presence
check is enforced in the printer's own MCU firmware, not in DYMO Connect.

## Building from source

- **Requirements:** Windows, .NET SDK (any recent), .NET Framework 4.8 reference assemblies
  (shipped with Windows 10/11), Inno Setup 6 (installer only).
- **App:** `cd src && dotnet build -c Release` → `bin\Release\dymo.exe` (+ `dnlib.dll`,
  `dymo.exe.config`). Copy those three files to `release\`.
- **Installer:** `ISCC installer\setup.iss` → `installer\Output\dymo-roll-patch-setup.exe`.

Dependencies: [dnlib](https://github.com/0xd4d/dnlib) 3.1.0 (MIT) for the IL patching.

## Notes / limitations

- Requires DYMO Connect to be installed (it patches the DLLs *it* extracts).
- Pristine copies are kept as `DYMO.LabelAPI.dll.orig` next to each patched DLL; `dymo restore`
  reverts everything.
- The IL injection anchors on dynamic method/property names (`set_SkuNumber`,
  `get_InsertedSKU`, …) rather than hardcoded offsets, so it tracks small DYMO Connect updates;
  a large rewrite of that type would need the anchors revisited.
- "Block DYMO Connect updates" edits `%SystemRoot%\system32\drivers\etc\hosts` (admin needed);
  reversible with `dymo updates off`.
