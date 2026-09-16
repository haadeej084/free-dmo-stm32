/* OpenDMOfw - model selection (width-dependent parameters in one place).
 *
 * One codebase, two head widths. The default is the 4" (5XL) class; the
 * 57 mm variant is a build option. Select with a build define:
 *   (none)        -> OP104 : 1248 dots, 300 dpi, 2 strobe segments (5XL class)
 *   -DMODEL_OP57  -> OP57  :  672 dots, 300 dpi, 2 strobe segments (550 class)
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

#if defined(MODEL_OP57)
  /* 57 mm head, presents as the 550-class printer. */
  #define MODEL_NAME            "OP57"
  #define MODEL_PID             0x0028
  /* The genuine 550 reports the product string WITH the vendor prefix:
   * a published lsusb of 0922:0028 shows "DYMO LabelWriter 550". */
  #define MODEL_USB_PRODUCT     "DYMO LabelWriter 550"
  /* IEEE-1284 device ID. MFG+MDL must yield the driver-model match ID
   * USBPRINT\DYMOLabelWriter_550C80D (NameModel(20) + OS checksum); the CID
   * field reproduces the genuine compatible ID 1284_CID_DYMOLabelWriter_550B. */
  #define MODEL_IEEE_ID         "MFG:DYMO;MDL:LabelWriter 550;CID:DYMOLabelWriter_550B;CLS:PRINTER;DES:DYMO LabelWriter 550;"
  #define MODEL_HEAD_DOTS       672      /* 57 mm @ 300 dpi (official spec) */
  #define MODEL_DPI             300
  /* Two shift-register halves (2x336 dots), each with its own heat strobe —
   * ROHM KF3002 architecture (sibling part KF3002-GL50A datasheet). Fired
   * sequentially to split peak current. Verify half count on the board. */
  #define MODEL_STROBE_SEGMENTS 2
  #define MODEL_DEFAULT_SKU     "30387"  /* Internet Postage, biggest 550 roll */
  #define MODEL_DEFAULT_COUNT   100
  /* ESC V version strings (16 chars each, zero-padded). Format per tech ref p.20;
   * the exact values are an assumption (DECISIONS D12) — kept consistent with the
   * model's PID/MDL so a 550 reports a 550 hardware string. */
  #define MODEL_HW_VERSION      "LW550-REV.K"
  #define MODEL_FW_VERSION      MODEL_FW_VERSION_COMMON
#else /* default: OP104 (4", 300 dpi - shipping-label class) */
  /* 1248-dot / 105.7 mm head, presents as the 5XL-class printer. */
  #define MODEL_NAME            "OP104"
  #define MODEL_PID             0x002A
  #define MODEL_USB_PRODUCT     "DYMO LabelWriter 5XL"  /* vendor prefix, as on the 550 */
  /* MFG+MDL must yield USBPRINT\DYMOLabelWriter_5XLB920; the CID field
   * reproduces the genuine compatible ID 1284_CID_DYMOLabelWriter_5XLB. */
  #define MODEL_IEEE_ID         "MFG:DYMO;MDL:LabelWriter 5XL;CID:DYMOLabelWriter_5XLB;CLS:PRINTER;DES:DYMO LabelWriter 5XL;"
  #define MODEL_HEAD_DOTS       1248     /* 105.7 mm @ 300 dpi (official spec) */
  #define MODEL_DPI             300
  /* Two heat halves (2x624 dots, STB1/STB2 per the KF3002 architecture), fired
   * sequentially to split peak current. If the board shows 4 heat lines on the
   * wide head, raise this to 4 (spare strobe pins are already in pins.h). */
  #define MODEL_STROBE_SEGMENTS 2
  #define MODEL_DEFAULT_SKU     "S0904980" /* 104x159 mm, biggest 5XL roll */
  #define MODEL_DEFAULT_COUNT   220
  /* ESC V version strings (16 chars each, zero-padded). Format per tech ref p.20;
   * the exact values are an assumption (DECISIONS D12) — kept consistent with the
   * model's PID/MDL so a 5XL reports a 5XL hardware string. */
  #define MODEL_HW_VERSION      "LW5XL-REV.K"
  #define MODEL_FW_VERSION      MODEL_FW_VERSION_COMMON
#endif

/* Derived values used by the rest of the firmware. */
#define HEAD_DOTS            MODEL_HEAD_DOTS
#define HEAD_BYTES           ((MODEL_HEAD_DOTS + 7) / 8)   /* OP104=156, OP57=84 */
#define HEAD_STROBE_SEGMENTS MODEL_STROBE_SEGMENTS

#endif /* OP_MODEL_H */
