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
> **What belongs on this list.** Only what genuinely has to be measured on an
> opened board with instruments. Two things that used to sit here do not:
>
> * **Anything a genuine printer can answer over a USB cable** — the IEEE-1284
>   device ID, the `ESC V` reply, the 32-byte status struct in its various
>   states, the roll record, the real die-cut gap, the density ladder. Those
>   moved to [`LIVETEST.md`](LIVETEST.md): a printer, a cable and a capture tool,
>   no screwdriver. One of them (the four optional 1284 keys) was *proved* in
>   cycle 5 to be obtainable from nothing else.
> * **Anything the firmware can compute from a reading it already takes** — the
>   thermistor divider is now solved by `tools/calib_thermistor.py` from raw ADC
>   codes and a thermometer, with no meter on the board.
>
> What is left below is the bench visit, and nothing more.

it again. What is left is **nine measurements** — seven, plus the strobe
polarity (6b), which DECISIONS D28 put back on the list when it withdrew the
datasheet reading. Plus a five-minute contribution
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

# PROPOSED replacement for FIELDWORK.md section 3 ("The one thing that matters most: GPIO routing")

Drop-in text for lines 157-232 of FIELDWORK.md. Everything below is read out of the
LabelWriter 450 / 450 Turbo application image
(`LW450_LW450T_app_0x2000_20480.bin`, load 0x2000, 20480 bytes). Addresses in brackets are
addresses in that image. Two rows are corroborated from a Rev E 550 board photo, marked as such.

---

## 3. The one thing that matters most: GPIO routing

**Start from the 450, not from a blank pad map.** A LabelWriter 450-generation mainboard fitted
into a 550 prints correctly (owner report), so the 450 firmware is a working driver for *this*
mechanism. Its MCU is an NXP LPC11Uxx, not an STM32, so its **pin numbers tell you nothing** —
but its **signal set, polarities, idle levels and sequence do**, and those are what a 550 board
also has to route. You are not looking for "which pad is PA5"; you are looking for *this* list
of nets, on *these* connectors, each with a known shape on a scope.

### 3.1 The complete signal set the genuine firmware drives

Every GPIO the 450 application configures, in the order its own init routine configures them
[0x4bac-0x4c76]: sixteen outputs then four inputs. "Idle" is the level the firmware leaves the
pin at [reset-state routine 0x4c78, stop routine 0x5dac].

| # | Signal | Dir | Active | Idle | Evidence |
|---|---|---|---|---|---|
| 1 | **Head DATA** | out | data | — | 84 bytes/line = 672 dots, over hardware SPI at PCLK/4 = 12 MHz, CPOL=0 CPHA=0. **One data line for the whole line — no second half** [SSP1 init 0x5852/0x5876, pin mux 0x58d2] |
| 2 | **Head CLK** | out | rising | low | Same port as DATA. Data is sampled on the **rising** edge, shown twice: SSP CPOL=0/CPHA=0, and the run-length path bit-bangs it as SET-then-CLR while holding the data line [0x5634-0x5640] |
| 3 | **Head LATCH** | out | **low** | high | One minimum-width low pulse immediately after the line is shifted, before the strobe [0x2906-0x2912] |
| 4 | **Head STROBE** | out | **low** | high | **One strobe for all 672 dots.** Asserted after the pulse width is computed; released by a timer match [assert 0x299c; width written to CT16B0 MR0 at 0x29a0-0x29b4; release 0x29cc] |
| 5 | **Head thermistor** | ADC | — | — | Burst-converted continuously. Head temperature, higher count = colder [ADC CR = 0x000100E0 at 0x5d72; read at 0x2a2a] |
| 6 | **VH rail sense** | ADC | — | — | 24 V at the head. Suspend below 676 counts, resume at 751, motor slow-down at 770 [0x29f2-0x2a28] |
| 7 | **Label-gap sensor** | ADC | — | — | **Analog, not a digital pin.** Software Schmitt trigger at 294 / 320 counts [0x2ec0-0x2ed4] |
| 8 | **Motor STEP** | out | pulse | low | One pulse per step-timer match, on every match [low->high 0x210c-0x2118, back low 0x216c]; the timer is re-armed with the step period on each match [0x62fa-0x6302] |
| 9 | **Motion gate (DIR or ENABLE)** | out | level | high | A level set per motion state — high in the forward states, low in three others [high 0x24d6, low 0x25fc]. The firmware **reads it back** and refuses to run the line engine while it is low [0x2e92-0x2e9c]. Which of DIR / ENABLE it is, is not established; that it is a *level*, and that printing is gated on it, is |
| 10 | **Start-torque / boost line** | out | **low** | high | Driven low on entry to every motion state, and **automatically released to high 128 step interrupts later** by the step ISR [0x62d4-0x62f8] — about 0.9 mm of travel at 3600 steps/inch. Too short to be a plain enable; consistent with a current-boost or decay-mode input to the driver. Not established |
| 11 | **An active-low /RESET** | out | **low** | high | Pulsed low then high exactly once, at boot [0x6354-0x636a]. Most consistent with the external driver IC's reset. Not established |
| 12 | **Mode strap A** | out | — | **high** | Written once, never again [0x4d18] |
| 13 | **Mode strap B** | out | — | **high** | Written once, never again [0x4d20] |
| 14 | **Mode strap C** | out | — | **low** | Written once [0x4d28, 0x5e3a] |
| 15 | **Engine power gate** | out | high = on | — | High at engine reset [0x4d08] and at print start [0x5e22]; low at print stop [0x5dc2], **after** the strobe is released and the motor lines are safed. This is the 450's only candidate for a head-VH or driver-supply gate, and the stop ordering is the ordering you would use for one. Not established |
| 16 | **Unidentified pulse line** | out | **low** | high | A short low pulse in exactly three places: between the head latch and the strobe [0x2916]; immediately before the **VH-rail** ADC read [0x29e6]; and in the last few steps of a feed [0x215c]. Best guess is a fault-latch reset or an external watchdog kick. It is **not** a head data/clock/latch/strobe line — all four of those are accounted for above |
| 17 | **Static enable** | out | — | **low** | Driven low once at boot, never again [0x637c] |
| 18 | **Digital sensor A** | in | — | — | Software-debounced (100-tick counter). Checked at the top of every line; a 1 aborts the job [0x2c76, 0x2ea6, 0x5996] |
| 19 | **Digital sensor B** | in | — | — | Same debounce path, second channel [0x2ca8] |
| 20 | **Button** | in | rising | — | Edge interrupt on pin-interrupt channel 0, plus a 40-tick software debounce [0x4f76-0x4f86, 0x593c, 0x59fc] |
| 21 | **Second edge input** | in | rising | — | Edge interrupt on pin-interrupt channel 1, polled at [0x5a36] |
| 22-24 | **LED group (3 lines)** | out | see note | — | Blinked by a toggle routine [0x5ad0]: "on" drives two lines low and makes the third an output-low; "off" drives one high and returns the third to an **input** (tri-state). **This is the one block that does not transfer** — the 450 has one button and one indicator, the 550 has its own button/LED board (SW1-SW3, D1-D8) |

That is **seventeen nets outside the front panel** (rows 1-21 minus the three-line LED group).
It is the shopping list.

### 3.2 The motor interface is STEP/DIR to a driver IC, not four phases

Nothing in the image ever writes four pins as a phase table. The only direct GPIO port-register
writes in the whole image are the two that re-mux the SPI pins [0x5902] and the run-length head
clock [0x5628-0x563e]; every other pin movement goes through a one-pin-at-a-time HAL, so the
inventory above is exhaustive over the image rather than a sample. What the MCU emits is **one
STEP pulse per timer match** plus levels and straps.

Step timing, for scale: the step timer's prescaler is 17 on a 48 MHz core, i.e. a **375 ns
tick** [0x34d0-0x34d2], and the raster step period is 255 ticks = **95.6 us per STEP pulse**.
(The other two timers: a second 375 ns timer carries the strobe width, and a 1 us timer does
housekeeping on a 250 ms match [0x34d4-0x34d6, 0x37ca-0x37d2].)

**The 550 board agrees.** On the Rev E photo, **J9 is a 4-pin connector** — two coil pairs,
matching the 2-phase bipolar LEILI 35BY412-339 already on record — it sits next to **U2**, and
**two 0.68 ohm "R680" sense resistors** flank it. A sense-resistor pair beside a driver package
is a current-regulated bipolar driver, which is exactly the architecture the 450 firmware
implies.

**Consequence for `src/pins.h`:** `MOTOR_DRIVE` should be the STEP/DIR variant, not the 4-phase
fallback, and the map is short of a **/RESET** line and the **mode straps**. Trace only STEP,
DIR and ENABLE and the driver may sit in reset with the wrong microstep setting, and the motor
will not turn at all.

### 3.3 What to look for, per connector

**J6, the head flex.** Six logic nets and only six: DATA, CLK, LATCH, STROBE, plus VH and the
thermistor return. If the flex carries the head's DI1/DI2 and STB1/STB2 separately, expect them
**paralleled on the board** — the genuine firmware drives exactly one data line and exactly one
strobe. LATCH and STROBE both idle **high** and pulse **low**; that is the polarity to expect,
and confirming it is measurement 6b, which must happen before any 24 V test.

**J9, the motor.** Four pins = the two coil pairs. Nothing on J9 reaches the MCU directly; it
reaches U2. Trace the **MCU side of U2** instead, through the series-resistor bank between them:
one pulse line (STEP), one or two levels (DIR / ENABLE), one /RESET, and two or three straps
tied high or low.

**The sensors.** Three analog and two or three digital, not one digital "paper sensor":

* the head thermistor (analog),
* the VH rail divider (analog) — the genuine firmware suspends printing on it,
* the label-gap photocell (**analog**, with a software Schmitt trigger),
* two debounced digital inputs the genuine firmware aborts a job on,
* the button (edge-interrupt capable).

If you find one digital paper sensor and no analog gap channel, look again: the genuine firmware
cannot find a die-cut gap without the analog one.

**No I2C.** The 450 application never enables the I2C peripheral and never touches its
registers — it has no config EEPROM. So the 450 says nothing about `PIN_I2C_SCL`/`PIN_I2C_SDA`.
That pair stays a pure 550-board question, and `store.c`'s boot ladder self-reports it anyway.

**The VH gate.** The 450 has no pin that is unambiguously a head-power switch. Signal 15 is the
only candidate: it goes high when the engine is armed and low at stop, after the strobe is
released. That is weak support, not confirmation. Treat `PIN_HEAD_VH` and its polarity as a
550-board measurement (measurement 5), bundled with measurement 6b — D31's fault handler is only
safe if at least one of the two polarities is right.

### 3.4 The honest part: what the 450 cannot tell you

The 450's MCU is an NXP LPC11Uxx. Its pin *numbers* do not transfer, and no amount of
disassembly changes that. Everything in `src/pins.h` that names a PA/PB pin is still an
assumption read off the STM32F072 datasheet and is still worth nothing until somebody buzzes it
out. What the 450 gives you is the list above: which nets exist, which way they point, what they
idle at, and what fires when. That turns "sweep 48 pads against everything" into "find these
seventeen, and you already know what each one should look like on a scope".

### 3.5 Do this FIRST: eight of the nineteen need no meter

*(keep the existing text — `tools/discover_pins.py`, `GS D 0x06` diffing for the inputs,
`GS D 0x07` for the outputs, `store.c`'s boot ladder for the I2C pair — with one correction.
With the STEP/DIR reading above, "the four motor phases announce themselves by twitching" is
wrong: a single `GS D 0x07` toggle of STEP is one microstep, which you will not see or hear.
Toggle the motion gate or the boost line instead and listen for holding torque appearing, or
pulse the candidate STEP pin a few hundred times.)*

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
  (polarity per-variant, ASSUMED active-low — measurement 6b, not established),
  VH (24 V), VDD (3.3 V), TM (built-in NTC 30 kΩ B=3950). No MISO.
  Sourced from the head datasheet — only the *board routing* is unknown.
- **STB polarity is NOT established — do not skip it.** This entry used to say
  "active-low, from the KF3002 timing chart, no separate experiment needed".
  Re-reading that chart withdrew the claim (DECISIONS D28): the published
  KF3002 charts draw the strobe idling LOW and pulsing HIGH, and other variants
  of the same family name the pin `/STB1`. It is **measurement 6b**, it is
  paired with **measurement 5** (the VH gate polarity), and both must be done
  before the head sees 24 V from anything but a current-limited supply.

  The old advice was worse than merely wrong. It sent the reader to confirm
  polarity from the scope trace taken during `diag 1` — and that capture is
  taken in bring-up step B4, the one row of the table whose "Head power" column
  reads **on**. So it proposed confirming the polarity only *after* the event
  the check exists to precede. If both this polarity and the VH gate polarity
  are inverted, the fault handler added in D31 switches the rail on into a
  firing head; that pair is the single case where the safety handler becomes
  the hazard.
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
   This is the only time you need SWD: every later image can go in over USB
   with `python tools/opsend.py dfu` followed by `dfu-util` (BUILD.md, "Updating
   over USB"). That matters for the fast route in 7b, where you rebuild often.
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
corrects it in one feed.

> **This one may be settled without a bench.** D30 reads DYMO's own LabelWriter
> 450 firmware as driving **12 motor steps per dot line — 3600 steps/inch** — on
> a mechanism a 450 mainboard drives correctly when fitted to a 550, while D24
> says 300. They cannot both be full steps (twelve would be 18750 rpm), so the
> 450's twelve are microsteps and our `1` is right **only if it microsteps
> exactly 12:1**. Counting the entries in the table the 450's CT32B0 step ISR
> indexes answers it from the image already in `scratchpad`. Do that before
> spending bench time here; if it comes back four-entries-cycled-three-times,
> this constant is wrong by a factor of four and every label is the wrong
> length. **That sentence used to claim the 450 count "confirms the 4-phase drive mode".
It says the opposite.** The 450 mainboard drives this mechanism with
**STEP / DIR / ENABLE to a driver IC** — three pins, one pulse per step, no
phase table anywhere in its image — and the microstep indexer is inside that
driver. The old justification ("a 4-lead bipolar motor is two H-bridges on
IN1–IN4") is a non-sequitur: *every* bipolar stepper is two H-bridges; the
question is whether the MCU sequences them or a driver IC does.

> ### Do this FIRST, and it costs nothing
> **Read the marking on U2 and its mode straps.** The 550 board has its own
> driver (a Rev K report notes a different U2), so the 450 proves the interface
> for the 450's board and not ours — but the part number on U2 decides the whole
> 4-phase-versus-STEP/DIR question in one look, before any feed test. On the 450
> the strap signature is visible in the image: two pins driven high and one low
> once at boot and never touched again.
>
> If U2 is a STEP/DIR driver, `MOTOR_DRIVE` must become `MOTOR_DRIVE_STEPDIR`
> and the microstep ratio comes off its datasheet rather than off a feed test. What remains
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

### 3. Thermistor divider — *no meter: two ADC readings and a thermometer*
> **The meter is gone from this one.** `tools/calib_thermistor.py` solves the
> divider from the raw ADC code alone. The NTC curve is known (30 kOhm at 25 °C,
> B = 3950), so ONE reading at a known temperature is one equation in one
> unknown and pins `R_p`; a SECOND reading with the head warmer settles the
> topology, because the wrong pull direction makes `R_p` move by a large factor
> while the right one holds. Run:
>
> ```
> opsend.py diag 4                      # thermistor_raw, printer cold
> ...print a few lines to warm the head, then diag 4 again...
> python3 tools/calib_thermistor.py --code 1638 --temp 21 --code2 2100 --temp2 34
> ```
>
> It prints the three `THERMAL_*_RAW` defines and `THERMAL_HOTTER_IS_HIGHER`
> ready to paste. What you still need is a **thermometer and a cold printer** -
> the arithmetic is only as good as the temperature you type, and the curve moves
> about 4 %/K near 25 °C. The prose below is kept as the fallback for a board
> where the ADC reads nothing at all.


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

> **Do this together with measurement 6b, in one sitting, before the first 24 V
> test.** They are no longer two independent unknowns. The fault handler
> (DECISIONS D31) drives PA8 to `!HEAD_VH_ON_LEVEL` on every fault, including a
> fault before `SystemInit()`, where reset had left the pin a floating input.
> If the gate polarity is inverted, that handler switches the rail **on** in a
> window that used to be safe by default. Its only mitigation is that the heat
> strobes are already at `!MODEL_STB_ACTIVE_LEVEL` by then, so the head draws
> nothing — which assumes the strobe polarity is right. Either assumption alone
> being wrong is survivable; **both wrong at once is the one case where the
> safety handler becomes the hazard.** So confirm both, and report both, before
> the head sees 24 V from anything other than a current-limited bench supply.

### 5b. How many DATA pins and how many STROBE pins does the flex bring out?
*Two continuity checks, and between them they decide whether the firmware can
print at all.*

**Data pins.** DECISIONS D36: the vendor's own firmware feeds this head **672
clocks on ONE data line**, and its image contains no second head data pin. The
firmware now defaults to that (`MODEL_HEAD_SHIFT_LINES 1`). Confirm it: does the
flex bring out one data pin or two?
* **One** → leave the default; `HEAD_DI2_DOTS` is meaningless.
* **Two** → set `MODEL_HEAD_SHIFT_LINES 2`, and measurement 6 (which half enters
  first) becomes live. Be careful here: if the two are a daisy chain (DO1 → DI2)
  rather than two independent inputs, driving DI2 from the MCU is a bus conflict
  against the head's own output, and the answer is still 1.

**Strobe pins.** `MODEL_STROBE_SEGMENTS` is 2, and the genuine firmware fires
**one strobe for all 672 dots** — it never drives a second. D30 keeps the split
for a supply reason that still stands (the 550 ships a 42 W brick against the
450's 60 W, and we have a load switch the 450 does not), but the split depends
on a second strobe net **existing on the flex**. If there is only one:
`HEAD_STROBE_SEGMENTS` must become 1 **and the energy ceiling has to be
re-derived for a whole-line strobe**. Finding that out after flashing rather than
before is the difference between a printer that works and one that does not.

### 6. Half-2 dot order — *assumption, easiest to spot in print*
`head.c` sends dot `i` to DI1 and dot `half + i` to DI2 on the same clock. Some
two-half heads shift the second bank in the opposite direction. The split itself
is assumed too: `MODEL_DI1_DOTS` / `MODEL_DI2_DOTS` in `model.h` say 336 + 336
(550) and 624 + 624 (5XL). **Report the head's part marking** (the 550 head bar
reads `3C56-9638`; the 5XL one is unknown) — if its datasheet gives a different
register split, change those two numbers; `head.c` handles unequal halves.
**How:** print `opsend.py testpattern`. If the right half of the pattern is
mirrored, reverse the DI2 index in `head_print_line()`.

### 6b. Strobe polarity — *do this before the first 24 V test*
`model.h`'s `MODEL_STB_ACTIVE_LEVEL` says a LOW level fires the heat drivers.
That is an assumption: the published KF3002 timing charts draw the strobe
idling low and pulsing high, and other variants of the same family name the pin
`/STB1` (DECISIONS D28). If it is wrong, the head fires continuously the moment
the 24 V rail comes up.
**How:** keep `OP_FLAG_VH_INHIBIT` set (`opsend.py vh off`). Power VH from a
bench supply current-limited to ~100 mA instead of the brick. Assert each
strobe pin in turn with `opsend.py diag 7 <port> <pin> 1` — no, that command
refuses the strobes on purpose; use `diag 1 1` (one head line) once per
candidate polarity instead, with the head connected and the current meter
watched. The polarity that draws essentially no current with the strobe idle,
and a brief current pulse only while printing, is the right one.
**If in doubt, leave it as is and report the measurement** — this is exactly
the kind of thing that is cheap to measure and expensive to guess.
**Patch:** `MODEL_STB_ACTIVE_LEVEL` in `src/model.h`, one line.
**Pair this with measurement 5** — see the note there. The fault-safe handler's
guarantee rests on these two polarities together, not on either one alone.

### 8. Po at the fitted head — *the one number that unlocks the energy model*
**This was missing from this list**, although `DECISIONS.md` D30 closes by naming
it as the single measurement that unblocks everything about the head's energy:
`HEAD_BASE_DWELL_US` (270) and `HEAD_MAX_DWELL_US` (410) are both derived from an
**assumed** Po of 0.43 W/dot and Rave of 1250 Ω, taken from published KF3002
siblings — and the head actually fitted (`3C56-9638`) is in no public ROHM
catalogue. No amount of further disassembly or research moves those constants;
this does.

**Why it matters more than it looks.** At the assumed Po, today's 337.5 µs sits
at 0.906 of ROHM's maximum-energy envelope and the 450-transposed 405 µs at
0.991 — but if the real Po is 17 % higher, today is still at 0.99 while the
ported value is at **1.19**. Today's number absorbs a 20 % error in an unmeasured
constant. That is the whole reason D30 declined to port the genuine dwell.

**How — and a correction, because the obvious method does not work.** An
earlier draft of this entry asked for a four-wire resistance across one dot.
**You cannot measure a single dot externally.** In a KF3002-class head the
heating elements sit between the common VH rail and the outputs of drivers that
are *inside the head module*; the only pins the flex brings out are VH, VDD,
GND, CLK, DI1, DI2, LAT, STB and TM. There is no per-dot terminal to probe, and
with the drivers off there is no path at all.

So measure the current instead, with a known number of dots energised:

1. **Put a shunt in the VH return** — a few tens of milliohms, non-inductive —
   and watch it on a scope. A DMM will not do: the strobe is a ~340 µs pulse.
2. **Print a line with a known dot count.** Our own firmware controls the raster
   exactly, so print all-black (`HEAD_DOTS` dots, but note the halves fire
   *sequentially*, so the current you see is one half at a time) and then a line
   with a small known count for a cross-check.
3. **Read VH at the head connector during that pulse**, on the same scope. The
   rail is an unregulated brick and it sags under the pulse; the loaded figure
   is the one that matters, not 24 V nominal.

Then `I_dot = I_measured / dots_energised`, `Po = V_loaded × I_dot`, and
`HEAD_MAX_DWELL_US = 0.177 mJ / Po`.

> **This reading now settles two things at once.** D35 found that ROHM's newest
> head of identical geometry and the same 24 V rail is **850 Ω**, against 1250 Ω
> on the four others — and nothing public says which grade DYMO specified. A dot
> draws **19.2 mA through 1250 Ω** and **28.2 mA through 850 Ω**, a 47 %
> difference this measurement separates easily. If it comes back near 28 mA, the
> firmware's present dwell is **above** the head's energy ceiling rather than
> comfortably under it, and `HEAD_MAX_DWELL_US` has to come down to about
> 260 µs. That is the single most consequential outcome of the whole bench visit.

**Without a scope** there is still a usable approximation: print continuously at
a known coverage, measure the *average* VH current with a DMM, and divide by the
duty cycle (strobe time ÷ line period, both of which `GS D 0x04` and the line
rate give you). Cruder, but it brackets Po, and bracketing it is already better
than the analogue the firmware assumes today.

**Report both raw numbers**, not the computed Po — the 0.177 mJ comes off a
curve that itself depends on line time (see below), and a later reader needs to
redo that arithmetic rather than inherit it.

> **Carry this forward, because it is counter-intuitive and easy to get backwards:**
> ROHM's rated pulse energy is **not a flat constant**. It rises with scanning
> line time, so a **faster** printer has a **lower** ceiling. `head.c` carries a
> `_Static_assert` that refuses to build if `MODEL_LINE_PERIOD_US` leaves the
> band the current ceiling was derived for.

**Two cheaper experiments that inform it**, in descending order of value:
- **Scope the 450 board's strobe line while it drives the 550 mechanism.** This
  is the one experiment the owner's mainboard-swap report makes possible, and it
  gives the genuine dwell at the genuine rail directly.
- **Total head current on a full-width black line.** Near 12 A means the head
  does no hardware dot grouping; a fraction of it means every energy figure in
  D30 divides by the group count.

**Patch:** `HEAD_MAX_DWELL_US` in `src/printer/head.c`, and then `D30`'s deferred
port becomes a decision rather than a guess.

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
| **Label feeds and ejects BLANK, LED blinking fast** | the heat rail is locked out: `OP_FLAG_VH_INHIBIT` is set. Either you set it (`opsend.py vh off`), or the config record failed its checksum and the firmware locked it out for you — see **DECISIONS D32** | `diag 4` / `diag 6` report the flag; `opsend.py vh on` clears it. If it comes back after every power cycle, the EEPROM is not storing: `diag 3`, and check `GS D 0x08`'s **persisted** byte |
| `diag 4` raw is 0 or 4095, and status byte 8 reads 2 | the head thermistor is open or shorted — or simply not fitted, which is the normal state of a bring-up board. The firmware treats this as a THIRD state, not as a cold head (**DECISIONS D33**): minimum dwell, self test refused, host told. Print is light but the printer works | measurement 3; `thermal.c` `THERMAL_OPEN_RAW` / `THERMAL_SHORT_RAW` |
| LED blinks fast with a healthy head and paper | same two states as above — the LED's 5 Hz pattern means "cannot put a dot on a label", which now includes the heat lockout and not only over-temperature | `main.c` `led_update()` |
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
