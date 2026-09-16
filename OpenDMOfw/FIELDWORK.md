# FIELDWORK — OpenDMOfw (what a board owner needs to measure)

**Status.** The firmware is complete and builds clean
(`make` → 550, the target board; `make MODEL=OP104` → 4" geometry for a future
5XL port). Every software-testable layer is done and
verified on a host.

**This document is deliberately as short as it can be.** Everything that DYMO's
own published manuals, the head datasheet or a board photo could answer has been
read and folded into the firmware already (DECISIONS D21) — thermal limits, the
VH rail voltage, the version-reply format, what the paper sensor actually senses,
the per-line time budget. Section 4 lists what is settled so you do not measure
it again. What is left is **seven measurements**, plus a five-minute contribution
in section 1b that needs no screwdriver at all.

**Two routes.** Sections 3–7 are the careful one: measure first, then power.
Section **7b** is the fast one: let the firmware drive pins and watch what
responds, with the 24 V heat rail locked out in firmware while you do it. They
end in the same place. Read **section 7c (risk factors)** either way — it is
short, and it is the part that names what cannot be undone.

**Flashing a factory board:** the stock F072 is reported to be at **RDP Level 2**
(DECISIONS D13). SWD is then off until RDP is lowered (mass-erase). Do not expect `make flash` to work on an unmodified
printer. The default `make` is the 550 build (PID `0x0028`).

---

## 0. Read this first: the three rules

1. **Nothing gets powered until it has been measured.** Every head/motor pin in
   `pins.h` is a guess. Bit-banging a guessed pin into a 24 V head driver is how
   a board dies. Section 3 is continuity checks on an **unpowered** board.
2. **The head connector stays off** until step B4 in section 6. The firmware
   enumerates, answers the host and runs every self-test except the head strobe
   perfectly well with no head attached.
3. **You cannot brick the F072 with this firmware.** It has no flash-write path
   and never touches option bytes. What you *can* brick is the print head, the
   motor driver, or your own ST-Link — hence rules 1 and 2. Full list in
   **section 7c**, including the one that surprises people: flashing this at all
   is a one-way conversion, because the stock firmware is behind RDP2 and cannot
   be backed up first.

**Time budget.** The continuity work (section 3) is one to two hours with a
multimeter. That alone, reported by email, is the single most valuable
contribution — everything after it is calibration that can be done later, by
someone else, from your numbers. Section 4 lists what has already been settled,
so read it before you start measuring things nobody needs.

---

## 1. How to report — email your findings

📧 **opendymofw@secret.fyi**

**You do not need to write any code.** Fill in the report template in section 10
and send it. A pull request updating `src/pins.h` (and `store.c` if the I2C pair
differs) is a welcome bonus, not a requirement. Partial results are welcome:
"I only got the head connector traced" is genuinely useful.

Photos help: a sharp shot of the head flex connector with your probe on a known
pin settles more arguments than a paragraph of prose.

---

## 1b. Contribute in five minutes, without opening anything

Most of what is still guessed about the *protocol* can be settled by anyone who
owns a working LabelWriter 550 / 550 Turbo / 5XL **and a genuine roll**. No
teardown, no multimeter, no soldering — plug it into a PC and run three things.
This is by far the cheapest contribution and it retires real assumptions.

```sh
pip install pyusb
# 1. the full USB descriptor set of a GENUINE printer
lsusb -v -d 0922:                       # Linux/macOS; on Windows use USBTreeView

# 2. the 63-byte consumable record from a real NFC roll
python tools/opsend.py sku              # prints the reply as hex

# 3. the 34-byte version reply
python tools/opsend.py version

# 4. the IEEE-1284 device ID string (printer-class GET_DEVICE_ID)
python -c "import usb.core as u; d=u.find(idVendor=0x0922); r=d.ctrl_transfer(0xA1,0,0,0,1023); print(bytes(r[2:]))"
```

(On Windows `opsend.py` needs a WinUSB binding via Zadig, and that detaches the
printer from the DYMO driver until you undo it. If that is a problem, the
`lsusb`/USBTreeView dump alone is still worth sending.)

What each one closes:

| Capture | Settles |
|---|---|
| Descriptor dump | Our clone's `bcdDevice` and string layout against the real thing (the 550's endpoint order `0x82`/`0x02` is already known from a published dump) |
| `ESC U` hex | That the printer sends the roll tag's record on the wire as we reconstruct it from 37 genuine tag dumps: CRC-32 at bytes 4–7, geometry in 0.1 mm, plus the three trailing date/time bytes the tag does not carry |
| `ESC V` hex | The real hardware/firmware version strings, of which we currently only know the documented *shape* |
| Device ID | The genuine 550/5XL IEEE-1284 string byte for byte — ours follows the LabelWriter 450 family layout, including its `CMD:` key, which has never been checked on a 550 |

These retire the remaining protocol entries in DECISIONS D12. Mail the hex to
**opendymofw@secret.fyi**.

---

## 2. What you need


**Required**

- A genuine **LabelWriter 550**, opened, mainboard exposed. **Not a 5XL or 550
  Turbo:** those boards carry an **STM32F407VET6** (read on several boards,
  DECISIONS D25) and this image does not run on them. Read the MCU marking
  anyway: it should be `STM32F072C8U6` or `STM32F072CBU6` (both work — the
  image is linked for 64 KB). A board photo with a legible marking is a
  welcome report on its own.
- A **multimeter** with a continuity buzzer and DC volts. Fine-tipped probes or
  a pair of sewing needles — LQFP48 pads are 0.5 mm apart.
- This repo checked out, so you can read `src/pins.h` while measuring.
- Fine-tipped probes really do matter here: the MCU is a leadless QFN, so you
  will be probing the 22 Ω series resistors rather than the chip (7c).

**Strongly recommended**

- A **logic analyzer** (an 8-channel clone is enough) or a scope. Sections 6–8
  are guesswork without one.
- An **F072 you are allowed to program**. Cheapest path: desolder the stock
  chip and fit a blank **STM32F072CB** (a few dollars — new chips ship RDP
  Level 0 with SWD enabled). **Match the footprint**: the Rev K board appears to
  be the leadless **UFQFPN48** (`...CBU6`), not LQFP48 (`...CBT6`), and a QFN
  swap wants hot air rather than an iron. A bare F072 dev board or a printer
  whose RDP was already lowered also works. See section 5.

**Optional**

- USB protocol analyzer (or Wireshark + usbmon on Linux) for section 5.
- A roll of any label stock, including third-party or blank die-cut.

---

## 3. The one thing that matters most: GPIO routing

The head/motor/sensor signals are plain GPIOs; the firmware bit-bangs them. The
assumed routing (`src/pins.h`) is below. Pad numbers come from the LQFP48 map in
`PINMAP.md` — orient the chip by its corner dot, and sanity-check that pad 7 is
NRST before trusting your numbering.

| Signal | Assumed pin | Pad | What to measure |
|--------|-------------|-----|-----------------|
| Head CLK   | PA5 | 15 | Continuity from pad 15 → which head-flex pin? |
| Head DI1   | PA6 | 16 | same |
| Head DI2   | PA7 | 17 | same |
| Head LAT   | PA4 | 14 | same (Low = THROUGH) |
| Head STB1  | PB0 | 18 | same (active-low heat strobe, half 1) |
| Head STB2  | PB1 | 19 | same (half 2) |
| Head STB3/4 | PB2 / PB3 | 20 / 39 | only if the head has more than 2 heat lines |
| Motor A1/STEP | PB4 | 40 | which motor-driver input does pad 40 reach? (motor = LEILI 35BY412-339, 2-phase bipolar, so expect two H-bridges) |
| Motor A2/DIR  | PB5 | 41 | same |
| Motor B1   | PB6 | 42 | same |
| Motor B2   | PB7 | 43 | same |
| Thermistor | PA1 | 11 | is the NTC divider on pad 11? (ADC_IN1) |
| Paper sensor | PA0 | 10 | is the paper sensor on pad 10? digital or analog? |
| Head VH enable | PA8 | 29 | which pad gates the 24 V P-MOS / load switch? polarity? |
| Status LED | PA2 | 12 | follow the LED (PC6/PC7 are not bonded on LQFP48) |
| Button | PA3 | 13 | follow the feed/power button |
| I2C SCL    | PB8 | 45 | confirm PB8 vs PB6 (SCL); trace EEPROM SCL |
| I2C SDA    | PB9 | 46 | confirm PB9 vs PB7 (SDA); trace EEPROM SDA |

**Method.** Board unpowered. Continuity mode between each F072 pad and the pins
of the **head flex connector**, the **motor-driver IC**, and the **EEPROM**. The
head side is already constrained by the ROHM pin order (CLK, DI1, DI2, LAT,
STB1/2, VH, VDD, GND, TM), so matching "which F072 pad reaches which flex pin"
gives you the full map.

**Work the other way round where it is easier.** Buzzing 48 pads against 10 flex
pins is 480 checks. Start from the *destination*: put one probe on head-flex pin
1 and sweep the F072 pads — most signals land on PA4–PA7 / PB0–PB3 or nowhere.

**Series resistors — expect about 22 Ω.** A Rev K board photo shows banks of SMD
"220" (= 22 Ω) parts around the MCU on exactly these lines, with some "102"
(1 kΩ) pulls mixed in. If a pad reads tens of ohms to a flex pin rather than a
dead short, that **is** the connection — note the resistance instead of calling
it "no connection".

> ⚠ **If the EEPROM lands on PB6/PB7, stop and read the conflict note in
> `PINMAP.md`.** PB6/PB7 are the default motor B1/B2 phases. Both cannot be true;
> the motor phases have to move (PB10–PB15, PA9/PA10/PA15 are free). Do not power
> anything until that is resolved.

---

## 4. What is ALREADY established (don't redo these)

Not everything that *could* be measured still needs to be. This section is the
short list of things that are already pinned down, so you can skip them.

- **MCU = STM32F072CB**, 48 pins, 128 K flash / 16 K RAM. On the Rev K board it
  looks like the **UFQFPN48** part (`...CBU6`) rather than LQFP48 (`...CBT6`).
  That does not change the pin map — ST's Table 13 uses one shared
  `LQFP48/UFQFPN48` column — but it does change how you probe: see 7c.
- **Complete physical pin map** (pad → GPIO), extracted from datasheet Table 13 —
  see `PINMAP.md`. Use it to find each pad.
- **Head interface** = ROHM KF3002-family module: built-in shift registers + latch
  + heat drivers; signals CLK, DI1/DI2, LAT (High=HOLD/Low=THROUGH), STB1/STB2
  (active-low), VH (24 V), VDD (3.3 V), TM (built-in NTC 30 kΩ B=3950). No MISO.
  Sourced from the head datasheet — only the *board routing* is unknown.
- **STB polarity = active-low.** From the KF3002 timing chart. You do not need a
  separate experiment for this; one glance at the scope trace you are taking
  anyway in step B confirms it.
- **Strobe segments = 2** on both heads (two shift-register halves, 2×624 and
  2×336). From the family architecture. Only revisit if a print shows a seam at
  the halfway point or the flex carries more than two STB lines.
- **DI1/DI2 topology** is not a separate measurement either: it falls out of the
  routing table. Two MCU pads reaching DI1 and DI2 ⇒ parallel (what the firmware
  assumes). Only one MCU pad, with the head's DO1 strapped to DI2 ⇒ daisy-chained.
- **EEPROM part**: Rev H/I/K = BL24C128A (16 KB, 2-byte addressing, 64 B page) @
  0x50, Rev E = AT24C01D/02D (1-byte, 8 B page). `store.c` detects which at boot,
  so you do not need to identify it — only whether WP is tied high, and `GS D 0x03`
  answers that functionally.
- **Feed contract**: one raster line = 1/300 inch = 0.08467 mm, so 300 lines must
  advance exactly 25.4 mm. That is the acceptance test for measurement 2.
- **NTC curve and the limits themselves**: 30 kΩ at 25 °C, B = 3950, and DYMO's
  own policy of halting at 70 °C and resuming at 56 °C (LW450 manual p.7). Only
  the board's divider resistor is unknown, and two readings pick it from a table.
- **VH = 24 V**: from the 550 manual's adapter table (p.9) — 24 V at 1.75 A (550),
  2.5 A (Turbo), 3.75 A (5XL). Not an inference from teardown photos any more.
- **Series resistors are 22 Ω**: visible as SMD "220" in banks around the MCU on a
  Rev K board photo. Expect tens of ohms, not a dead short, on head lines.
- **There is a 12 MHz crystal** (Y1, "AXC12.00-115") beside the MCU. The firmware
  does not need it (HSI48 + CRS), but `-DOPENDMO_CLOCK_HSE12=1` will use it.
- **Graphics mode does not change the feed step** on the 550 series: its
  barcode/graphics mode is 300×300 dpi (550 p.8). The 450 halved the step; this
  family does not, so `ESC h`/`ESC i` correctly change nothing.
- **I2C = AF2**, and I2C1 exists only on PB6/PB7 or PB8/PB9 (datasheet Table 14).
- **SWD flash points:** SWDIO = PA13 (pad 34), SWCLK = PA14 (pad 37); GND = any VSS
  pad (23/35/47); 3.3 V = a VDD pad (24/48). Boots from flash by default (BOOT0
  pad 44 Low).
- **USB identity** (VID `0x0922`, PID `0x002A`/`0x0028`, IEEE-1284 device ID) and the
  **command set + status-struct layout** — sourced from the tech ref and a live
  capture of a genuine 550.

> **What D.MO Connect actually checks** (decompiled, DECISIONS D24): it never
> sends `ESC U`, so the record's CRC and geometry cannot upset it. Roll state
> comes from the `ESC A` status only — bay status byte 10 (8 = OK, 10 =
> counterfeit), the 12-byte SKU and the label count — and the SKU must be in
> Connect's catalog for the install's region, or the roll shows as empty. The
> `ESC V` version strings remain our own values (D12). See measurement 7.

### What the silicon already rules out

You are not searching 48 pads. The package and the peripheral mapping eliminate
most of them before you touch a probe:

| Fact | Consequence |
|------|-------------|
| 11 pads are power/ground/NRST/BOOT0 (1, 7, 8, 9, 23, 24, 35, 36, 44, 47, 48) | not signals |
| USB DM/DP are fixed at PA11/PA12 (pads 32/33) | not available |
| SWD is PA13/PA14 (pads 34/37) | not available (and you need them to flash) |
| **ADC inputs exist only on PA0–PA7 (IN0–IN7), PB0 (IN8), PB1 (IN9)** | the head thermistor **must** be one of those 10 pads — and so must the paper sensor if it turns out to be analog |
| I2C1 is AF2 and exists only on PB6/PB7 or PB8/PB9 | the EEPROM is on one of those two pairs, full stop |
| Port C is bonded only as PC13/PC14/PC15 on LQFP48, and those are limited-drive, low-frequency pins | they cannot carry head CLK/DI/STB; an LED or a button is plausible |

That leaves **33 candidate GPIOs**, of which roughly 17–19 are in use. For the
thermistor specifically the search is 10 pads, and for I2C it is 4.

**Why no photo or datasheet can finish the job:** D.mo's silkscreen carries only
reference designators (`U1`, `C4`, `JP2`…), never signal names; the FCC circuit
diagram is confidential; and the stock MCU is RDP2, so its flash cannot be read
back and disassembled. Continuity probing is the only remaining route. That is
the honest reason this document exists.

---

## 5. Bring-up step A — silicon and USB, no head, no motor

Goal: prove the chip runs your image and the host sees a printer. Nothing can be
damaged in this step.

1. **Establish you may program the chip — and that it is the right chip.**
   `st-info --probe` must report an **STM32F07x** (chip ID `0x448`); anything
   else means this image does not fit the board. Connect SWD and try to read the IDCODE
   (`st-info --probe`, or OpenOCD `targets`). On a stock printer this is
   expected to fail if the part really is at RDP2, which disables SWD entirely —
   not a wiring fault. **If it does return an IDCODE**, the part is at RDP0/1,
   not RDP2: report that, it settles an open question. A second check that
   needs no probe: hold BOOT0 (pad 44) high with USB attached and look for a
   DFU device `0483:df11` (AN2606: the F072 bootloader offers USB DFU); at RDP2
   nothing appears. Continue on an F072 you are allowed to program.
2. **Flash** with the head connector and motor disconnected: `make flash`.
3. **Enumeration.** Plug USB into a PC. Expect `0922:0028`. On Linux: `lsusb`.
   Report the exact VID:PID line.
4. **Device ID.** The printer class returns the IEEE-1284 string via
   GET_DEVICE_ID; on Windows the hardware ID derives from its MFG+MDL.
   Report whether Windows binds D.mo's own driver package without a prompt. If
   it does not, read the OS-generated hardware ID out of
   `%windir%\inf\setupapi.dev.log` and compare it with
   `USBPRINT\DYMOLabelWriter_550C80D` / `...5XLB920` — Microsoft names that log as
   the way to retrieve it. Also run `lsusb -v` (Linux) and report the endpoint
   list: it should show `0x82` IN before `0x02` OUT, as on a genuine 550.
5. **The LED tells you the state** (from `main.c`): solid = configured and ready;
   1 Hz blink = enumerated but not configured; 5 Hz = head over temperature;
   double-blink every ~1.2 s = paper out. If the LED pin is wrong you will see
   nothing — that is a routing issue (section 3), not an enumeration failure.
6. **Talk to it without a driver:** `python tools/opsend.py status`. You should
   get `bay: 8`, the compiled default SKU and count. Report the raw dict.

**If enumeration fails**, the suspects in order are: the 48 MHz clock (HSI48 +
CRS, `system.c`), USB D+/D− (PA11/PA12 are fixed, so this is a board/cable
issue), and the PMA/EPnR handling (DECISIONS D8 items 1, 2 and 6). A USB analyzer
capture of the failed enumeration is the single most useful thing to send.

---

## 6. Bring-up step B — the physical layer via `GS D`, still safe

The firmware ships a diagnostic backdoor specifically for this (PROTOCOL.md
"GS D"). All of it works with **no vendor driver** and, except where noted, with
**no head power**.

```sh
python tools/opsend.py diag 5          # build id - put this in your report
python tools/opsend.py diag 4          # snapshot: model, thermistor, GPIOs, config
python tools/opsend.py diag 3          # EEPROM write/read self-test
python tools/opsend.py diag 2 30       # step the feed motor 30 dot lines
python tools/opsend.py diag 1 8        # fire the head for 8 all-on lines
```

Recommended order and what each one proves:

| Step | Command | Head power | Proves | Report |
|------|---------|-----------|--------|--------|
| B1 | `diag 4` | off | ADC reads the thermistor; paper/button GPIOs read something | the whole snapshot dict |
| B2 | `diag 3` | off | the I2C EEPROM path works, and WP is not asserted | `eeprom_match` true/false |
| B3 | `diag 2 30` | off | the motor pins reach the driver and the paper moves | did it move? which direction? how far? |
| B4 | `diag 1 8` | **on** | the head fires | `lines_fired`, and whether a mark appears on the stock |

**`diag 1` is thermally gated.** It runs every dot at maximum dwell, so the
firmware refuses to fire while the head is over its limit and reports how many
lines *actually* fired. `lines_fired` < the number you asked for means the
thermal gate stopped it — check `thermal_ok` in the same reply and measurement 3
below before assuming the head is dead.

**Scope the head lines during `diag 1`** if you have an analyzer: CLK burst,
then one LAT pulse, then one wide STB pulse per half. That single capture
confirms the STB polarity, the DI topology and the strobe count in one go
(section 4).

**No host at all?** The firmware implements the genuine button behaviour, so the
bench needs nothing but power and paper:

| Gesture | Action |
|---|---|
| Short press | Form feed to the tear position |
| Hold ~10 s | Print the built-in test pattern (border + diagonals); press again to stop |

That is the same gesture the real printer uses (550 p.8), and it exercises the
head, the feed and the thermal gate end to end without a PC in the loop.

---

## 7. Bring-up step C — calibration with paper

Only once B1–B4 pass. Start with a **short dwell and low density** and raise it
stepwise while watching head temperature (`diag 4` reports the raw ADC).

```sh
python tools/opsend.py density 60        # start low
python tools/opsend.py testpattern       # border + diagonals: width and alignment
python tools/opsend.py density 100
python tools/opsend.py image label.png
```

The test pattern is designed to expose exactly the failures you are hunting: a
border that is cut off on one side means the dot offset is wrong, a mirrored
right half means measurement 6, and diagonals that come out as stairsteps of
uneven height mean measurement 2.

---

## 7b. Fast route — use the firmware as the probe

The route above is written for someone who wants a *measured* map and does not
want to redo work. If you would rather find the map by driving pins and watching
what happens, this section is for you. It is not reckless — it is the same job
done by search instead of by survey, with the one genuinely destructive step
fenced off in firmware rather than in prose.

**Why it is safe to just try things.** Almost every wrong guess here fails
harmlessly:

| Wrong guess | What actually happens |
|---|---|
| Wrong motor pin | 3.3 V logic into a logic input. Nothing moves. |
| Wrong I2C pair, LED, button, sensor pin | Nothing. |
| Wrong CLK / DI / LAT on the head | You clock nonsense into a shift register. Harmless **as long as VH is off**. |
| Wrong strobe pin **with VH on** | Heat elements destroyed in seconds. Not recoverable. |

So the danger is not "wrong pin". It is "wrong pin **and** 24 V behind it". Split
those two and the caution collapses to a single rule.

**The firmware enforces that rule.** Build the exploration image:

```sh
make clean && make CFLAGS_EXTRA=-DOPENDMO_SAFE_BRINGUP=1     # or edit store.c
```

`OP_FLAG_VH_INHIBIT` then starts **set**, and `head.c` refuses to enable the heat
rail for any command whatsoever. It is a persisted config bit, not a habit:
`opsend.py vh off` sets it, `opsend.py vh on` clears it, and `diag 4` / `diag 6`
report its state. You clear it once, deliberately, at step 5 below.

### The loop

1. **Enumerate first, on the real board.** Flash, plug in USB, check for
   `0922:002a` / `0922:0028` and run `opsend.py status`. Ten minutes, and the
   whole USB stack and protocol layer are validated — the largest block of
   unknowns, with no measurement at all.

2. **Find the sensors by scanning, not tracing.**
   ```sh
   python tools/opsend.py diag 6                 # baseline
   # warm the print head gently (hairdryer, low, ~20 s)
   python tools/opsend.py diag 6                 # diff the ADC columns
   ```
   The channel that moved is the thermistor. Repeat with a label blocking the
   top-of-form sensor to find the photocell. The scan covers every ADC-capable
   pin on the part (PA0–PA7, PB0, PB1), so the answer is in the table by
   construction.

3. **Find the motor by driving it.** `opsend.py diag 2 30`. If nothing moves,
   permute: other pin group, other drive mode, or poke single pins with
   `diag 7 <port> <pin> <n>` while watching the driver IC's outputs. Failures
   here cost nothing.

4. **Get the head's logic right with the rail still locked.** `diag 1 8` shifts,
   latches and strobes exactly as a real print would, but with VH inhibited
   nothing can heat. Scope CLK / DI1 / DI2 / LAT / STB: you want a burst of
   `HEAD_DOTS/2` clocks, one latch pulse, then one strobe pulse per half.
   Iterate `pins.h` until that picture is right.

5. **Only now unlock the rail.** Before you do, confirm on the scope that
   **every** strobe line idles **high**. Then `opsend.py vh on`, and go straight
   to `diag 1 1` — a single line — with paper in the path. If a mark appears,
   you are done with the dangerous part.

6. **Calibrate** as in section 7.

### What you give up

A map you *inferred*, not one you *measured*. You will know PB4 makes the motor
turn without knowing whether there is a series resistor in the way or which
driver input it reaches. Fine for your own board; the measured map in section 3
is the better thing to publish. Both routes end at the same place.

And neither route gets around **RDP2** — you still need an F072 you are allowed
to program.

---

## 7c. Risk factors — read before you open anything

Most of this project fails softly. These are the parts that do not.

### Irreversible

- **Flashing this firmware converts the printer permanently.** The stock DYMO
  firmware is behind RDP Level 2 and **cannot be read out first**. Lowering RDP
  mass-erases it. There is no backup, no restore, and no image to put back. You
  are not experimenting with a printer, you are converting one. Use a spare, or
  a replacement F072, or accept that outcome before you start.
- **The print head.** A strobe held low with 24 V on the rail destroys elements
  in seconds and a 550-series head is not a part you casually re-order. This is
  the single reason for the VH interlock and the 70 °C limit. Do not disable
  either "just to see".
- **Firing the head with no paper under it.** The elements then rub directly on
  the rubber platen — DYMO's own 450 manual mentions that friction — and you
  wear both. Always have stock in the path before `diag 1` or a test print.
- **Raising RDP.** This firmware never writes option bytes, and neither should
  you. RDP2 is a one-way door.

### Damaging, but replaceable

- **Motor driver.** A wrong phase pattern can leave two phases of an H-bridge on
  at once. On 24 V that is a shoot-through. Keep `diag 2` bursts short until you
  have confirmed the drive mode, and touch the driver IC to check it is not
  getting hot.
- **The head flex connector.** ZIF connectors on these mechanisms are rated for
  a handful of insertions. Plan your probing so you open it as few times as
  possible.
- **ESD.** A bare mainboard and an exposed head flex are both static-sensitive,
  and the head's drivers sit right behind the connector. Strap up, or at least
  keep one hand on a grounded surface.

### Easy to underestimate

- **The QFN package.** The MCU on the Rev K board appears to be **UFQFPN48**
  (`STM32F072CBU6`), not the LQFP48 (`...CBT6`) the docs originally assumed. The
  pin *numbering* is identical — ST's Table 13 uses a single shared
  `LQFP48/UFQFPN48` column, so the pad map in `PINMAP.md` stands — but there are
  **no protruding leads to probe**. Pads are 0.5 mm pitch and flush with the
  body, and a probe tip bridges two of them easily. A slipped probe across two
  powered pins can take the MCU with it.
  **Do not probe the MCU directly.** Use the **22 Ω series resistors** instead:
  every head and motor line goes through one, each is an accessible 0402/0603
  pad, and electrically it *is* the MCU pin. Trace from there.
- **Heat you can touch.** A head at its 70 °C limit will mark direct-thermal
  paper from a fingertip and is unpleasant to touch. It also stays hot after the
  rail is cut.
- **24 V is not a "safe low voltage" for the board.** It will not hurt you, but
  a shorted probe on that rail will happily vaporise a trace.
- **Mains.** The brick is a sealed 24 V supply. Do not open it, and do not probe
  on the primary side of anything.
- **The EEPROM's original contents.** On first boot this firmware writes its
  config into the EEPROM, and on a 1-byte (Rev E) part it also disturbs the
  first few bytes. If the stock contents might ever matter to you, dump the
  EEPROM before the first boot.
- **Unattended operation.** Do not leave a board powered with the rail unlocked
  and no one watching, especially during calibration.

### Reassuringly harmless

You cannot brick the F072 with this image: it has no flash-write path and never
touches option bytes. Wrong pins on the motor, the sensors, the LED, the button
or the I2C bus do nothing but fail to work. And with `OP_FLAG_VH_INHIBIT` set,
no command sequence in the protocol can heat the head at all.

---

## 8. Measurements, in the order they pay off

Seven items. Everything else has been resolved in section 4 — this is the
irreducible list that genuinely needs the board in front of you.

### 1. GPIO routing — *the whole job*
Section 3. Nothing below can be interpreted before this is done.

### 2. Motor µsteps per line — *the motor is now identified; the gearing is not*
The motor is a **LEILI 35BY412-339**: two-phase bipolar PM stepper, 4 leads,
~35 mm can, ~6.5 Ω/phase (the marking "35BY412-339 6.5Ω" is legible in the FCC
photos of both the 550 and the 5XL), 7.5° per step = 48 steps/rev. One full
step per line is the estimate that fits the rated speed (DECISIONS D24), and it
is what `MOTOR_STEPS_PER_LINE` holds; the measurement below confirms or
corrects it in one feed. That confirms the 4-phase drive mode the firmware
defaults to — a 4-lead bipolar motor is two H-bridges on IN1–IN4. What remains
is the drive train between motor and platen.
Assumed a 24 V-capable driver (MP6500-class chopper or a discrete bridge),
driven **IN1–IN4 directly** (`MOTOR_DRIVE_4PHASE`), `MOTOR_STEPS_PER_LINE = 1`.

> ⚠ **Measure the winding before the first `diag 2`.** Put an ohmmeter across
> each phase pair of the motor lead. `k_phase[]` is two-phase-on full-step, so
> both windings carry current for the whole feed. At a few ohms on 24 V behind a
> plain bridge that is several amps — a thermal failure within seconds, not a
> shoot-through. A chopper driver limits it; a high-resistance winding needs no
> limiting. Report the reading and the driver marking, keep bursts short until
> both are known, and feel the motor and driver IC after each burst.
**How:** identify the IC first (report the marking). Then scope the phase pins
during `diag 2 300` and measure how far the paper actually moved.
One raster line must equal 1/300 inch = **0.08467 mm**, so:

```
MOTOR_STEPS_PER_LINE = steps_issued x 0.08467 mm / measured_mm_moved
```

Feed 300 lines (`diag 2` takes a byte, so run it a few times or use
`opsend.py feed`), measure the travel with calipers, and round to the nearest
sensible integer. 300 lines must advance exactly **25.4 mm**.
**Patch:** `MOTOR_STEPS_PER_LINE` and, if the motor stalls or sings,
`MOTOR_STEP_US` in `motor.c`.

### 3. Thermistor divider — *two readings, then read the numbers off a table*
The head's built-in NTC is **30 kOhm at 25 °C, B = 3950** (ROHM datasheet), and
the temperatures that matter are DYMO's own, not invented: the LabelWriter 450
manual states the engine "halt[s] printing if the print head temperature exceeds
**70 °C**" and resumes "when the print head cools to **56 °C**". `thermal.c`
implements exactly that, latched, with hysteresis. The only unknown left is the
board's divider.

`R(T) = 30000 × exp(3950 × (1/T − 1/298.15))`, T in kelvin:

| °C | 0 | 10 | 20 | 25 | 30 | 40 | 50 | 56 | 60 | 70 | 80 |
|----|---|----|----|----|----|----|----|----|----|----|----|
| kΩ | 100.9 | 60.5 | 37.6 | **30.0** | 24.1 | 15.9 | 10.8 | **8.6** | 7.5 | **5.3** | 3.8 |

**Step 1 — two readings.** `diag 4` at room temperature, then again after warming
the head gently (hairdryer on low, ~20 s). Note the room temperature.

**Step 2 — direction.** Raw went **up** when warm ⇒ `THERMAL_HOTTER_IS_HIGHER 1`
(NTC to VDD, `R_p` to GND). Raw went **down** ⇒ set it to `0`.

**Step 3 — pick the column your 25 °C reading matches and copy the two
thresholds.** 12-bit ADC, rounded. Enter the pull-down numbers either way —
`thermal.c` inverts the reading itself when the topology is pull-up.

*Pull-down (`THERMAL_HOTTER_IS_HIGHER 1`)*

| | R_p = 10 k | 20 k | 30 k | 47 k | 100 k |
|---|---|---|---|---|---|
| raw @ 25 °C (match this) | 1024 | **1638** | 2047 | 2500 | 3150 |
| **`THERMAL_COLD_RAW`** (25 °C) | 1024 | **1638** | 2047 | 2500 | 3150 |
| **`THERMAL_RESUME_RAW`** (56 °C) | 2200 | **2862** | 3181 | 3461 | 3770 |
| **`THERMAL_LIMIT_RAW`** (70 °C) | 2680 | **3240** | 3482 | 3681 | 3890 |

*Pull-up (`THERMAL_HOTTER_IS_HIGHER 0`) — for matching your 25 °C reading only*

| | R_p = 10 k | 20 k | 30 k | 47 k | 100 k |
|---|---|---|---|---|---|
| raw @ 25 °C | 3071 | 2457 | 2048 | 1595 | 945 |

The shipped defaults are the **20 k pull-down** column (bold). If your reading
matches a different column, change three numbers in `thermal.c` and you are done.

**Report:** room temperature and both raw readings. That alone lets someone else
finish this without the board.

### 4. Top-of-form photocell — *what it is, is known; the wiring is not*
Not a plain "paper present" switch. The 550 manual (p.7) says: "An infrared LED
photocell detects the top-of-form sense hole that is located between labels. The
absolute positions of the label and the tear bar are calculated based upon the
reading of an infrared LED photocell sensor." So it is an **emitter + detector
pair** reading the gap hole, and the genuine firmware counts motor steps between
holes to track position.

Two consequences for us:
- The detector may be **analog**, not a logic level. If so it belongs on an ADC
  pin (PA0–PA7, PB0, PB1 are the only candidates) with a threshold, not on a
  GPIO read.
- The **emitter may need driving**. `pins.h` has no pin for it. If the LED is not
  simply tied to 3V3 through a resistor, find the pin that gates it and add it.

**How:** `diag 4` with and without stock in the path, and again with a label gap
over the sensor; watch `paper_present`. If it never changes, measure the pin
voltage in each state.
**Patch:** `PIN_PAPER_SENSE` / `PAPER_PRESENT_LEVEL` in `pins.h`, or move it to
the ADC. By default this does **not** gate printing (`OP_FLAG_PAPER_FORCE`,
DECISIONS D14) — it only drives the LED — so a wrong result here cannot stop you
printing.

### 5. VH enable pin and polarity — *unknown, and strobes do nothing without it*
**Check the external pull first.** The MCU's pins are floating inputs during and
after reset, so only a board-side resistor can hold the load-switch gate off
while the MCU is unpowered, in reset, or being flashed. If there is no such
pull, that is a finding worth reporting on its own — it means the rail's state
at power-on is undefined.
Assumed PA8, active-low P-MOS gate.
**How:** find the 24 V load switch near the head connector, trace its gate to an
F072 pad. Confirm polarity by measuring VH at the head connector while the
firmware holds the pin low.
**Patch:** `PIN_HEAD_VH` / `HEAD_VH_ON_LEVEL` in `pins.h`.
**Symptom if wrong:** everything looks right on the logic lines and the head
simply never marks the paper.

### 6. Half-2 dot order — *assumption, easiest to spot in print*
`head.c` sends dot `i` to DI1 and dot `half + i` to DI2 on the same clock. Some
two-half heads shift the second bank in the opposite direction. The split itself
is assumed too: `MODEL_DI1_DOTS` / `MODEL_DI2_DOTS` in `model.h` say 336 + 336
(550) and 624 + 624 (5XL). **Report the head's part marking** (the 550 head bar
reads `3C56-9638`; the 5XL one is unknown) — if its datasheet gives a different
register split, change those two numbers; `head.c` handles unequal halves.
**How:** print `opsend.py testpattern`. If the right half of the pattern is
mirrored, reverse the DI2 index in `head_print_line()`.

### 7. Host acceptance — *the actual goal*
With a plausible SKU configured, does **D.MO Connect** show a valid roll and
print end to end?
**How:** `opsend.py config --count 220 --sku S0904980` (5XL) or `--sku 30387`
(550), then drive it from D.MO Connect.
**If the roll shows as empty/JOKER:** Connect decides that from the status
struct and its own catalog, not from `ESC U` (DECISIONS D24). Check, in order:
`opsend.py status` shows `bay: 8` and the SKU you configured; the SKU exists in
Connect's catalog for your region (an EU install hides US-only SKUs as
"empty"); then try the `pc-patch/` tool, which fixes the catalog side.
**Report:** what Connect displayed, before and after the patch.

---

## 9. Troubleshooting map

| Symptom | Most likely cause | Where to look |
|---------|-------------------|---------------|
| No USB device at all | clock / D+ pull-up | `system.c` SystemInit, `usb_init()` BCDR |
| Enumerates, Windows won't bind D.mo's driver | IEEE-1284 MFG/MDL string | `model.h` `MODEL_IEEE_ID`; hardware ID in `setupapi.dev.log` |
| `diag 4` thermistor raw is 0 or 4095 | wrong ADC pin, or NTC not on PA1 | measurement 3 |
| `diag 3` always false | I2C pair wrong, or WP tied high | section 3 (I2C pair), section 4 (EEPROM) |
| `diag 2` does nothing | motor pins or drive mode wrong | measurement 2, `MOTOR_DRIVE` |
| Motor buzzes, doesn't turn | phase order or `MOTOR_STEP_US` too short | `k_phase[]`, `motor.c` |
| `diag 1` fires 0 lines | thermal gate — the head reads as over-limit | measurement 3 |
| Head logic looks right, nothing prints | VH never enabled | measurement 5 |
| Print is stretched or squashed vertically | µsteps per line | measurement 2 |
| Right half of the image mirrored | DI2 dot order | measurement 6 |
| Print too light / too dark | dwell and density | `HEAD_BASE_DWELL_US`, `opsend.py density` |
| Label starts in the wrong place | die-cut gap / tear offset | `LABEL_GAP_DOTS`, `TEAR_EXTRA_DOTS` in `protocol.c` |
| Connect shows "empty"/JOKER roll | host-side validation | measurement 7, `pc-patch/` |

---

## 10. Report template

Copy, fill in what you measured, leave the rest as `?`, and mail it to
**opendymofw@secret.fyi**. If you only did section 1b, just send that block —
it is still worth having.

```
NO-TEARDOWN CAPTURES (section 1b - a working printer + a genuine roll)
  lsusb -v / USBTreeView dump of the genuine printer:
  opsend.py sku      (63-byte hex):
  opsend.py version  (34-byte hex):

BOARD
  Model (550 / 5XL / Turbo):
  Board revision (silkscreen, e.g. Rev K):
  MCU marking:
  EEPROM marking + WP tie (VCC/GND/blob):
  Motor driver IC marking:

GPIO ROUTING (pad number, or "none", or ohms if via a series resistor)
  Head CLK:            Head DI1:           Head DI2:
  Head LAT:            Head STB1:          Head STB2:
  Extra STB lines (how many in total):
  Motor pins (which pads reach which driver inputs):
  Thermistor:          Paper sensor:       VH enable:
  Status LED:          Button:
  I2C SCL:             I2C SDA:

MEASUREMENTS (section 8)
  2 Motor: drive mode, steps issued, mm moved, -> MOTOR_STEPS_PER_LINE:
  3 Thermistor: room temp __ °C -> raw __ ; warmed -> raw __
      (that is enough - the R_p column and both thresholds follow from the table)
  4 Top-of-form photocell: digital or analog? levels with / without stock;
      does the IR emitter need a drive pin?
  5 VH: voltage measured, enable pad, polarity:
  6 Half-2 dot order (mirrored in test print? yes/no):
  7 D.MO Connect: roll shown as ____ ; printed? ____ ; pc-patch needed? ____

CONFIRMATIONS (free, while you are already scoping - section 4)
  STB pulses low to fire?            yes / no / not scoped
  DI1 and DI2 both driven?           yes / no / not scoped
  Number of STB lines on the flex:
  diag 3 result (eeprom_match):

RAW OUTPUT (paste)
  lsusb line:
  opsend.py diag 5:     <- build id: says which image these numbers came from
  opsend.py status:
  opsend.py diag 4:

NOTES / PHOTOS
```

---

## 11. What not to do

- Do not raise the chip to RDP level 2 from this firmware. It never writes option
  bytes, and you should not either — it is a one-way door.
- Do not connect head power before sections 3–6 pass. A strobe held low on a
  24 V head is a burnt element.
- Do not assume a pad has no connection because the meter shows tens of ohms —
  see the series-resistor note in section 3.
- Do not skip the `pins.h` edit after measuring. A correct measurement written
  only in an email still leaves the next person guessing.
