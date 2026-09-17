# Live print tests — what a genuine LabelWriter answers over a USB cable

**Nothing here needs the printer opened.** No screwdriver, no meter, no risk to
the head. A genuine 550 on a USB cable, its own software, and a capture tool
settle several questions this project has otherwise been unable to close — one
of which was *proved* unobtainable from any other source.

That is why these are **not** on `FIELDWORK.md`. Fieldwork is what must be
measured on an opened board with instruments. This is a printer doing its job
while we listen.

Work top to bottom; each step is independent, so a step that fails costs only
itself.

---

## TO DO — tick these off during the session

Each line says what to bring back. Anything you cannot get, leave unticked and
say so; a step that fails costs only itself.

- [ ] **0. Capture running** before the printer is plugged in.
      → `550_enum.pcapng`
- [ ] **1. Device ID** — `python3 tools/probe_genuine.py --out 550_devid.txt`
      → the IEEE-1284 string verbatim *(closes a Hardware-only item)*
- [ ] **2. ESC V** — same run
      → the version reply, byte for byte
- [ ] **3. Status struct, six states** — `ESC A` once per state, each labelled
  - [ ] 3a idle, roll fitted, cover closed
  - [ ] 3b **out of paper**
  - [ ] 3c **cover open**
  - [ ] 3d mid-job (poll during a long print)
  - [ ] 3e immediately after the last label ejects
  - [ ] 3f third-party roll, if one is to hand *(also the DRM question)*
      → six 32-byte dumps, each labelled with its state
- [ ] **4. ESC U** — the roll record
      → the reply **plus the SKU on the box** (e.g. `S0722370` / `30252`)
- [ ] **5. Print two labels, measure with a ruler**
      → (a) printed length of one label, (b) gap between the two prints, in mm
      → and the roll SKU
- [ ] **6. Density ladder** — same label at each darkness setting, strip kept in order
      → photo under even light, marked; or captures if the setting is not exposed
- [ ] **7. Full job capture** — one ordinary address label via DYMO Connect
      → `550_print_address.pcapng`

**Bring back:** the `.pcapng` files, `550_devid.txt`, the six status dumps, the
two measurements in mm, the roll SKU, and the density strip. Drop them anywhere
and say where — they get replayed through the real parser and the answers land
in `model.h`, `PROTOCOL.md` and the paper table.

---

## 0. Before anything: start the capture

Everything below is worth more as a capture than as a screenshot, because the
capture can be replayed through our parser afterwards (`tools/capture_replay/`).

- **Windows:** [USBPcap](https://desowin.org/usbpcap/) + Wireshark. Pick the root
  hub the printer is on, start capturing, *then* plug the printer in so the
  enumeration is recorded too.
- **Linux:** `modprobe usbmon`, then `tshark -i usbmonN -w dymo550.pcapng`.

Save every capture with a name that says what was happening:
`550_enum.pcapng`, `550_print_address.pcapng`, `550_paper_out.pcapng`.

> The project already has `dymo450.pcapng` (a genuine **450**) and two Ethernet
> captures of a LabelWriter Wireless. **A 550 USB capture is the single most
> valuable artifact this project does not have.**

---

## 1. The IEEE-1284 device ID — closes a Hardware-only item

**Why it matters more than it looks.** `src/model.h` declares
`MFG:DYMO;CMD: ;MDL:LabelWriter 550;CLASS:PRINTER;DESCRIPTION:DYMO LabelWriter 550;`
and four of those keys are *guesses*. Cycle 5 proved they cannot be recovered any
other way: the Windows USBPRINT hardware-ID checksum covers only `MFG` and `MDL`,
so no INF, catalog or `setupapi.dev.log` can ever constrain `CMD`, `CLASS`,
`DESCRIPTION` or `SERN`. `lprint`, the one open-source table that would have them,
has the whole 550 family commented out.

If Windows binds DYMO's driver to our device but the string is wrong in a way
that matters, that is a printer which enumerates and never prints.

**How:**
```
python3 tools/probe_genuine.py --out 550_devid.txt
```
It asks the printer directly and is **read-only by construction** — the send path
refuses any byte sequence not on its own safe list, so it cannot print, feed,
write configuration or touch the update path.

If a vendor driver owns the interface and the tool cannot bind, the capture from
step 0 has the same answer: the device ID is fetched during enumeration.

**Deliverable:** the string verbatim. Paste it into the session and it goes into
`model.h` and `DECISIONS`.

---

## 2. `ESC V` — the version reply we currently guess

Same tool, same run. Our `send_version()` invents a format. Capture what a
genuine unit actually replies, byte for byte.

---

## 3. The 32-byte status struct — thirty bytes nobody has re-checked

`PROTOCOL.md` documents all 32 bytes, but only a handful have been verified
against a genuine unit; the rest were transcribed once from the tech reference.
Cycle 5 flagged exactly this. Byte 8 and byte 21 were settled by reading DYMO's
own Linux driver — the other thirty were not.

**How:** run `probe_genuine.py` (it sends `ESC A`) once per state, and note which
state each reading came from:

| # | State | What to do |
|---|-------|-----------|
| 3a | idle, roll fitted, cover closed | nothing |
| 3b | **out of paper** | run the roll out, or lift the roll off the sensor |
| 3c | **cover open** | open the lid |
| 3d | mid-job | start a long print, poll while it runs |
| 3e | just after a job | poll immediately after the last label ejects |
| 3f | **third-party roll** | if you have one — this is also the DRM question |

**Deliverable:** six 32-byte dumps, labelled. Diffing them tells us which byte
carries which condition, which is the only way to get it right.

---

## 4. `ESC U` — the roll record for the roll actually fitted

Our reconstruction of the 60-byte consumable record was built from 37 genuine
roll-tag dumps and is CRC-verified, but it has never been checked against what a
printer *reports* for a roll in front of it.

One field in particular: the firmware currently uses a single die-cut gap of 42
tenths of a mm for every stock, because that is the fleet mode across those 37
tags. The genuine record for the fitted roll gives its real value. For the 5XL's
own default SKU the difference is 15 tenths — small, and it is exactly the kind
of thing that puts print in the wrong place.

**Deliverable:** the `ESC U` reply plus **which roll is fitted** (the SKU on the
box, e.g. `S0722370` / `30252`).

---

## 5. Print two labels and measure them

The cheapest physical answer in the whole project, and it needs no instruments
beyond a ruler or calipers.

**How:** print **two** identical address labels from DYMO's own software. Then
measure, on the strip that comes out:

- **a)** the printed length of one label;
- **b)** the gap between the end of the first print and the start of the second.

**Deliverable:** those two numbers in millimetres, and the roll SKU.

**What it settles:** (b) is the real die-cut gap for that stock, directly. It
either confirms the 42 tenths the firmware now uses or corrects it, and it is the
ground truth the per-paper gap table would be built on.

---

## 6. The density ladder — print the same label four times

DYMO's presets are `ESC c` 75 %, `ESC d` 87.5 %, `ESC e` 100 %, `ESC g` 112.5 %,
and our firmware implements that ladder — but nothing has ever compared the
*result*.

**How:** if the software exposes a darkness or quality setting, print the same
label at each setting and keep the strip in order, marked. If it does not,
capture a job at each setting and we read the command out of the capture.

**Deliverable:** the strip, photographed under even light, in order. Plus the
captures if the settings are not exposed.

**Caveat, stated plainly:** this calibrates how our density mapping *looks*, not
the head's energy. `Po` still needs a bench (FIELDWORK 8).

---

## 7. A full job capture — the command sequence in order

Print one ordinary address label with DYMO Connect while capturing.

**Deliverable:** `550_print_address.pcapng`. We replay it through the real parser
with `tools/capture_replay/`. That is how cycle 2 found the feed-axis defect —
the entire 138-scenario test suite passed with and without it, and only genuine
traffic caught it.

---

## What this session does **not** answer

Say so plainly so the next bench visit is planned around the right things. These
still need the board open, and they are all that is left on `FIELDWORK.md` as
true measurements:

- **GPIO routing** — board-specific; no genuine printer can tell us how *our*
  MCU is wired to the mechanism.
- **VH enable pin and polarity, and strobe polarity** — must be done together,
  before the head sees 24 V from anything but a current-limited supply.
- **`Po` at the fitted head** — the one number that unblocks the energy model.
