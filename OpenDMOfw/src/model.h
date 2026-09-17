/* OpenDMOfw - model selection (width-dependent parameters in one place).
 *
 * One codebase, two head widths. Select with a build define:
 *   -DMODEL_OP57 (default) -> OP57  :  672 dots, 300 dpi (LabelWriter 550)
 *   -DMODEL_OP104          -> OP104 : 1248 dots, 300 dpi (4" / 5XL geometry)
 *
 * WHICH BOARD. The plain LabelWriter 550 carries an STM32F072 (48-pin), the
 * part this firmware is written for. The 5XL and the 550 Turbo carry an
 * STM32F407VET6 (Cortex-M4, 100-pin, Ethernet via a KSZ8081 PHY) - read on
 * several boards (DECISIONS D25). So OP57 is the image with a real target;
 * OP104 keeps the 1248-dot geometry, 5XL USB identity and paper table ready
 * for an F407 port, and runs on an F072 only as a test build.
 *
 * Head dimensions per the official "LabelWriter 550 Series Printers Technical
 * Reference Manual": the 57 mm head uses 672 individually addressable dots
 * (84 bytes/line), the 4" head uses 1248 dots (156 bytes/line), both at 300 dpi.
 *
 * NOTE ON THE WIDTH IN MM. Only the DOT COUNT is a spec value; the physical
 * width follows from it. 1248 dots at 300 dpi = 105.7 mm (ROHM catalog SF2024
 * lists the matching head as 105.706 mm), 672 dots = 56.9 mm. The name "OP104"
 * refers to the 104 mm printable label width of the biggest 5XL roll, NOT to
 * the head. Do not re-derive these as 101 or 104 mm - earlier revisions of the
 * docs did, and the ESC U geometry was ~1.6 % short as a result.
 *
 * Both heads are ROHM KF3002-family modules with TWO shift-register halves,
 * each with its own heat strobe (STB1/STB2) — so both models fire 2 segments
 * sequentially to split peak current (ROHM KF3002-GL50A datasheet; head ID in
 * PINMAP.md / DECISIONS D16).
 *
 * Each model presents itself to the host as the corresponding genuine label
 * printer (VID/PID, USB strings, IEEE-1284 device ID) so that the stock
 * PC-side label software binds its own driver package to it. Only the head
 * geometry, strobe segments and USB identity differ; the USB core, protocol
 * parser, motor, thermal and config code are shared.
 *
 * Adding a new format = add a block here; nothing else needs to change.
 */
#ifndef OP_MODEL_H
#define OP_MODEL_H

/* Vendor ID shared by both models (the genuine vendor's USB VID, confirmed in
 * the 550 tech ref p.10 together with the per-model PIDs below). */
#define MODEL_VID 0x0922

/* ESC V firmware-version string. The tech ref p.20 fixes the STRUCTURE of this
 * 16-char field exactly: bytes 0-3 "FWAP" (application) or "FWBL" (bootloader),
 * 4-7 major release, 8-11 minor release, 12-15 release date as MMYY. Four
 * four-character groups, no separators - so the old "FWAP01.02.2112" was not a
 * well-formed reply, dots and all. The VALUES are still ours to choose
 * (DECISIONS D12); the host treats them as informational. */
#define MODEL_FW_VERSION_COMMON  "FWAP000100010921"

/* Build identifier, stamped by the Makefile / build.sh from `git describe`.
 * Reported verbatim by GS D 0x05 so a fieldwork report says which image was
 * measured. "dev" = built outside a git checkout. */
#ifndef OPENDMO_BUILD
#define OPENDMO_BUILD "dev"
#endif
/* Wire cap for that string. 2 + 48 keeps the reply inside one 64-byte bulk
 * packet with room to spare; long enough for "v1.1.1-14-gfda76012-dirty". */
#define OP_BUILD_ID_MAX 48

#if defined(MODEL_OP57) && defined(MODEL_OP104)
#error "define MODEL_OP57 or MODEL_OP104, not both"
#endif
#if !defined(MODEL_OP104) && !defined(MODEL_OP57)
#define MODEL_OP57                 /* default: the LabelWriter 550 */
#endif

#if defined(MODEL_OP57)
  /* 57 mm head, presents as the 550-class printer. */
  #define MODEL_NAME            "OP57"
  #define MODEL_PID             0x0028
  /* The genuine 550 reports the product string WITH the vendor prefix:
   * a published lsusb of 0922:0028 shows "DYMO LabelWriter 550". */
  #define MODEL_USB_PRODUCT     "DYMO LabelWriter 550"
  /* IEEE-1284 device ID. Windows derives the hardware ID
   * USBPRINT\DYMOLabelWriter_550C80D from MFG+MDL only (NameModel(20) + OS
   * checksum), so those two are the load-bearing fields. Key names and order
   * follow the published device ID of the genuine LabelWriter 450 family
   * (MFG/CMD/MDL/CLASS/DESCRIPTION); the 550's own string is unverified. No
   * CID field: the previous one had no source. */
  #define MODEL_IEEE_ID         "MFG:DYMO;CMD: ;MDL:LabelWriter 550;CLASS:PRINTER;DESCRIPTION:DYMO LabelWriter 550;"
  #define MODEL_HEAD_DOTS       672      /* 57 mm @ 300 dpi (official spec) */
  #define MODEL_DPI             300
  /* Two shift-register halves (2x336 dots), each with its own heat strobe —
   * ROHM KF3002 architecture (sibling part KF3002-GL50A datasheet). Fired
   * sequentially to split peak current. Verify half count on the board. */
  #define MODEL_STROBE_SEGMENTS 2
  /* Dots clocked in on DI1 and DI2. ASSUMED 336 + 336: the head part number,
   * and so its register split, is not confirmed (FIELDWORK section 3). A
   * KF3002 variant with an UNEQUAL split exists - the GD31A puts dots 1-384 on
   * DI1 and 385-640 on DI2, with four strobes of 256/128/128/128 - so this is
   * a real possibility, not a formality. head.c handles unequal halves. */
  #define MODEL_DI1_DOTS        336
  #define MODEL_DI2_DOTS        336
  #define MODEL_DEFAULT_SKU     "30387"  /* Internet Postage, biggest 550 roll */
  #define MODEL_DEFAULT_COUNT   100
  /* ESC V version strings (16 chars each, zero-padded). Format per tech ref p.20;
   * the exact values are an assumption (DECISIONS D12) — kept consistent with the
   * model's PID/MDL so a 550 reports a 550 hardware string. */
  #define MODEL_HW_VERSION      "LW550-REV.K"
  #define MODEL_FW_VERSION      MODEL_FW_VERSION_COMMON
#else /* OP104 (4", 300 dpi - shipping-label class). NOT the default: model.h
       * selects MODEL_OP57 above when neither is defined, because OP57 is the
       * LabelWriter 550, the only model with a board this firmware runs on. */
  /* 1248-dot / 105.7 mm head, presents as the 5XL-class printer. */
  #define MODEL_NAME            "OP104"
  #define MODEL_PID             0x002A
  #define MODEL_USB_PRODUCT     "DYMO LabelWriter 5XL"  /* vendor prefix, as on the 550 */
  /* MFG+MDL must yield USBPRINT\DYMOLabelWriter_5XLB920; same key layout as
   * the 550 block above. */
  #define MODEL_IEEE_ID         "MFG:DYMO;CMD: ;MDL:LabelWriter 5XL;CLASS:PRINTER;DESCRIPTION:DYMO LabelWriter 5XL;"
  #define MODEL_HEAD_DOTS       1248     /* 105.7 mm @ 300 dpi (official spec) */
  #define MODEL_DPI             300
  /* Two heat halves (2x624 dots, STB1/STB2 per the KF3002 architecture), fired
   * sequentially to split peak current. If the board shows 4 heat lines on the
   * wide head, raise this to 4 (spare strobe pins are already in pins.h). */
  #define MODEL_STROBE_SEGMENTS 2
  /* ASSUMED 624 + 624, as for the 550 block. */
  #define MODEL_DI1_DOTS        624
  #define MODEL_DI2_DOTS        624
  /* 104x159 mm, biggest 5XL roll (EU part number). DYMO's en-US driver names
   * the same media "1744907 4 in x 6 in" (LW5XX.DLL string 420, lw5xl.gpd
   * Shipping4x6, ESC L 0x0867), but that is a localized UI label - the GPD
   * driver never reads the ESC U SKU. S0904980 is the name DYMO Connect's
   * catalog resolves (pc-patch patches A and D), so it stays. */
  #define MODEL_DEFAULT_SKU     "S0904980"
  #define MODEL_DEFAULT_COUNT   220
  /* ESC V version strings (16 chars each, zero-padded). Format per tech ref p.20;
   * the exact values are an assumption (DECISIONS D12) — kept consistent with the
   * model's PID/MDL so a 5XL reports a 5XL hardware string. */
  #define MODEL_HW_VERSION      "LW5XL-REV.K"
  #define MODEL_FW_VERSION      MODEL_FW_VERSION_COMMON
#endif

/* Strobe polarity: which level FIRES the heat drivers.
 *
 * ASSUMED, and genuinely unknown for our head. This was previously recorded as
 * "confirmed active-low from the ROHM KF3002 timing chart"; re-reading the
 * chart withdraws that. In KF3002-GL50A and -GD31A Fig.2 the STROBE trace
 * idles LOW and pulses HIGH (DRIVER OUT idles high and pulses low), and inside
 * the same figure /LATCH carries a drawn overbar while STROBE does not. But
 * KF3002-GM50A, KF3004-GM50A and KD3004-DC72A spell the pin "/STB1" in text -
 * so ROHM do mark it when a variant is active-low, and at least one KF3002
 * variant is. Polarity is per-variant, and the GK11C has no public datasheet.
 *
 * CORROBORATED since: the genuine LabelWriter 450 firmware drives its head
 * strobe ACTIVE LOW (see DECISIONS D30), and a 450 mainboard is reported to
 * print correctly on a 550 mechanism - so the head this firmware talks to
 * accepts an active-low strobe. That is evidence from working hardware rather
 * than from a datasheet drawing, and it is why the assumption stays Low.
 *
 * Getting this wrong means the head fires continuously the moment VH comes up,
 * which is why OP_FLAG_VH_INHIBIT exists and why FIELDWORK has a
 * current-limited polarity check before the first 24 V test. One constant, one
 * place to flip. */
/* Nominal time for one dot line, feed included. DYMO rate the 550 at 62
 * labels/min and the 5XL at 53 on a 4-line address label (1050 dot lines), so
 * 0.92 ms and 1.08 ms per line; 800 us is the working figure both models are
 * built around. It lives here rather than in motor.c because head.c needs it
 * too: the head's energy ceiling is derived from ROHM's maximum-energy
 * envelope, which is a FUNCTION OF LINE TIME, not a constant. Move this and
 * HEAD_MAX_DWELL_US has to be re-derived - head.c has a static assert that
 * says so at build time. */
#define MODEL_LINE_PERIOD_US   800

#define MODEL_STB_ACTIVE_LEVEL 0     /* 0 = Low fires (current assumption) */

/* Derived values used by the rest of the firmware. */
#define HEAD_DOTS            MODEL_HEAD_DOTS
#define HEAD_BYTES           ((MODEL_HEAD_DOTS + 7) / 8)   /* OP104=156, OP57=84 */
#define HEAD_STROBE_SEGMENTS MODEL_STROBE_SEGMENTS
#define HEAD_DI1_DOTS        MODEL_DI1_DOTS   /* first dots of the line -> DI1 */
#define HEAD_DI2_DOTS        MODEL_DI2_DOTS   /* remaining dots         -> DI2 */

#endif /* OP_MODEL_H */
