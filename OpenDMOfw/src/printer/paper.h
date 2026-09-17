/* OpenDMOfw - paper-size table (keyed by the ESC L code from the host driver).
 *
 * The host driver's DOC_SETUP selects a paper size with ESC L + u16 BIG-endian
 * (PROTOCOL.md / DECISIONS D11). The GPD literal "<1B>L<0867>" puts bytes
 * 08 67 on the wire, read as the value 0x0867. The value is a LENGTH, not an
 * opaque id (LW5XX.GPD: page height + 300 for most 550 papers), which is why
 * several papers share one value - the first entry with a value wins, and the
 * sharing papers differ only in width, which the ESC D header carries anyway.
 * 0x7F00 (custom size) and 0xFFFF (continuous) are sentinels handled in
 * protocol.c and never looked up here. The value identifies the stock; the
 * raster geometry itself arrives explicitly in the ESC D header. This table is
 * used for:
 *   - feed math (label pitch = paper height + physical gap)
 *   - the mm dimensions reported in the ESC U consumable record
 * Unknown codes fall back to the model default paper; a code that is not in
 * the table but looks like a plain dot length may be treated as such by the
 * protocol layer.
 *
 * Dimensions are in dots at 300 dpi, taken from the genuine driver GPD files:
 *   5XL : lw4xl.gpd   (D.MO Connect Drivers/DLS)
 *   550 : LW5XX.GPD   (spooler driver store)
 * PageDimensions = PAIR(width_dots, height_dots).
 */
#ifndef OP_PAPER_H
#define OP_PAPER_H
#include <stdint.h>
#include "../model.h"

typedef struct {
    uint16_t code;          /* ESC L u16 big-endian value (wire 08 67 = 0x0867) */
    uint16_t width_dots;    /* page width in dots */
    uint16_t height_dots;   /* page height in dots */
} paper_t;

/* The 550 rows, shared: the 5XL driver inherits them (see below). */
#define PAPERS_550_ROWS \
    { 0x0546,  329, 1050 },   /* Address 30252/30320/99010 (default) */ \
    { 0x05DC,  694, 1200 },   /* Shipping 30256 / NameBadge 30364    */ \
    { 0x05D3,  638, 1191 },   /* Shipping 99014 / NameBadge 99014    */ \
    { 0x041A,  225,  750 },   /* Durable 1933085                     */ \
    { 0x0960,  675, 2100 },   /* PC Postage 3-part 30383             */ \
    { 0x09F6,  694, 2250 },   /* PC Postage 2-part 30384             */ \
    { 0x0465,  638,  825 },   /* Diskette                            */ \
    { 0x0384,  330,  600 },   /* Handing file insert / return address*/ \
    { 0x0396,  338, 1031 },   /* File folder 2-up                    */ \
    { 0x0534,  235, 1031 },   /* File folder                         */ \
    { 0x03EB,  600,  703 },   /* Zipdisk                             */ \
    { 0x0D7A,  694, 3150 },   /* PC Postage EPS 30387 (biggest roll) */ \
    { 0x09F0,  694, 2244 },   /* Large lever arch                    */ \
    { 0x0233,  640,  263 },   /* Jewelry label 2-up                  */ \
    /* remaining LW5XX.GPD papers */ \
    { 0x0542,  422, 1046 }, \
    { 0x080F,  225, 1763 }, \
    { 0x07FC,  260, 1744 }, \
    { 0x04C3,  544,  919 }, \
    { 0x0478,  225,  844 }, \
    { 0x0258,  300,  300 }, \
    { 0x02A3,  675,  375 }, \
    { 0x03AA,  300,  638 }, \
    { 0x0290,  304,  356 }, \
    { 0x035F,  150,  563 }, \
    { 0x02EE,  300,  450 }, \
    { 0x0438,  693,  780 }, \
    { 0x05EB,  731, 1215 }, \
    { 0x0339,  464,  525 },
#if defined(MODEL_OP57)
static const paper_t PAPERS[] = {
    PAPERS_550_ROWS
};
#define PAPER_DEFAULT_CODE  0x0546
#else
/* 5XL-class papers. Source is lw5xl.gpd, which begins `*Include: "lw5xx.gpd"`
 * and then declares its own PaperSize feature with six NEW options plus a
 * restatement of CUSTOMSIZE (only to widen MaxSize and MaxPrintableWidth).
 * Restating one inherited option to change two values, and restating no other,
 * only means anything under GPD merge-with-override semantics - so the real 5XL
 * driver can emit every 550 paper code as well as its own.
 *
 * The transcription used to read lw5xl.gpd alone and carried five rows. Any of
 * the 24 missing codes then missed paper_lookup() and fell back to the default
 * 0x0867 = 1883 dot lines, so selecting a 550 Address label on a 5XL fed
 * 1883 instead of 1050 - about 70 mm of blank stock per label.
 *
 * The 4"-wide rows come FIRST so a code defined by both resolves to the 5XL
 * geometry; the inherited 550 rows follow verbatim. */
static const paper_t PAPERS[] = {
    { 0x0867, 1233, 1883 },   /* Shipping 4x6  (S0904980, default)   */
    { 0xB80B, 1320, 3000 },   /* Shipping 4x10                       */
    { 0xB009, 1204, 2480 },   /* A6                                  */
    { 0x03E2, 1200,  694 },   /* High Capacity Large Shipping        */
    { 0x0275, 1050,  329 },   /* High Capacity Address               */
    PAPERS_550_ROWS
};
#define PAPER_DEFAULT_CODE  0x0867
#endif

/* Exact lookup; NULL when the code is not in the table. */
static const paper_t *paper_find(uint16_t code)
{
    for (unsigned i = 0; i < sizeof(PAPERS) / sizeof(PAPERS[0]); i++)
        if (PAPERS[i].code == code) return &PAPERS[i];
    return 0;
}

/* Lookup with fallback: unknown codes give the model default paper. */
static const paper_t *paper_lookup(uint16_t code)
{
    const paper_t *p = paper_find(code);
    if (p) return p;
    return paper_find(PAPER_DEFAULT_CODE);
}

#endif /* OP_PAPER_H */
