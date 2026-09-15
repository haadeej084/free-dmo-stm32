/* OpenDMO-FW - model selection (width-dependent parameters in one place).
 *
 * One codebase, two head widths. The default is the 104 mm (4") class; the
 * 57 mm variant is a build option. Select with a build define:
 *   (none)        -> OP104 : 101 mm head, 300 dpi, 1248 dots, 2 strobe segments
 *   -DMODEL_OP57  -> OP57  : 57 mm head, 300 dpi, 672 dots, 2 strobe segments
 *
 * Head dimensions per the official "LabelWriter 550 Series Printers Technical
 * Reference Manual" (LW550_TECHREF.txt): the 57 mm head uses 672 individually
 * addressable dots (84 bytes/line), the 101 mm head uses 1248 dots
 * (156 bytes/line), both at 300 dpi.
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

/* Vendor ID shared by both models (the genuine vendor's USB VID). */
#define MODEL_VID 0x0922

#if defined(MODEL_OP57)
  /* 57 mm head, presents as the 550-class printer. */
  #define MODEL_NAME            "OP57"
  #define MODEL_PID             0x0028
  #define MODEL_USB_PRODUCT     "LabelWriter 550"
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
  #define MODEL_FW_VERSION      "FWAP01.02.2112"
#else /* default: OP104 (4", 300 dpi - shipping-label class) */
  /* 101 mm head, presents as the 5XL-class printer. */
  #define MODEL_NAME            "OP104"
  #define MODEL_PID             0x002A
  #define MODEL_USB_PRODUCT     "LabelWriter 5XL"
  /* MFG+MDL must yield USBPRINT\DYMOLabelWriter_5XLB920; the CID field
   * reproduces the genuine compatible ID 1284_CID_DYMOLabelWriter_5XLB. */
  #define MODEL_IEEE_ID         "MFG:DYMO;MDL:LabelWriter 5XL;CID:DYMOLabelWriter_5XLB;CLS:PRINTER;DES:DYMO LabelWriter 5XL;"
  #define MODEL_HEAD_DOTS       1248     /* 101 mm @ 300 dpi (official spec) */
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
  #define MODEL_FW_VERSION      "FWAP01.02.2112"
#endif

/* Derived values used by the rest of the firmware. */
#define HEAD_DOTS            MODEL_HEAD_DOTS
#define HEAD_BYTES           ((MODEL_HEAD_DOTS + 7) / 8)   /* OP104=156, OP57=84 */
#define HEAD_STROBE_SEGMENTS MODEL_STROBE_SEGMENTS

#endif /* OP_MODEL_H */
