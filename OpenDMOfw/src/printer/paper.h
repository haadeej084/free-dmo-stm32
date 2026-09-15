/* OpenDMOfw - paper-size table (keyed by the ESC L code from the host driver).
 *
 * The host driver's DOC_SETUP selects a paper size with ESC L + u16 LE
 * (PROTOCOL.md / DECISIONS D11). The GPD literal "<1B>L<0867>" is the numeric
 * code 0x0867; on the wire that is bytes 67 08. The table stores that u16
 * value. The code identifies the stock; the
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
    uint16_t code;          /* ESC L u16 LE value (e.g. 0x0867) */
    uint16_t width_dots;    /* page width in dots */
    uint16_t height_dots;   /* page height in dots */
} paper_t;

/* Raster line width in bytes for a given dot width. */
#define PAPER_LINE_BYTES(wd)  (((wd) + 7) / 8)

#if defined(MODEL_OP57)
/* 550-class papers (from LW5XX.GPD). */
static const paper_t PAPERS[] = {
    { 0x0546,  329, 1050 },   /* Address 30252/30320/99010 (default) */
    { 0x05DC,  694, 1200 },   /* Shipping 30256 / NameBadge 30364    */
    { 0x05D3,  638, 1191 },   /* Shipping 99014 / NameBadge 99014    */
    { 0x041A,  225,  750 },   /* Durable 1933085                     */
    { 0x0960,  675, 2100 },   /* PC Postage 3-part 30383             */
    { 0x09F6,  694, 2250 },   /* PC Postage 2-part 30384             */
    { 0x0465,  638,  825 },   /* Diskette                            */
    { 0x0384,  330,  600 },   /* Handing file insert / return address*/
    { 0x0396,  338, 1031 },   /* File folder 2-up                    */
    { 0x0534,  235, 1031 },   /* File folder                         */
    { 0x03EB,  600,  703 },   /* Zipdisk                             */
    { 0x0D7A,  694, 3150 },   /* PC Postage EPS 30387 (biggest roll) */
    { 0x09F0,  694, 2244 },   /* Large lever arch                    */
    { 0x0233,  640,  263 },   /* Jewelry label 2-up                  */
    { 0xFFFF,  638, 32000 },  /* Banner (continuous)                 */
};
#define PAPER_DEFAULT_CODE  0x0546
#else
/* 5XL-class papers (from lw4xl.gpd). */
static const paper_t PAPERS[] = {
    { 0x0867, 1233, 1883 },   /* Shipping 4x6  (S0904980, default)   */
    { 0x7F00, 1320, 3000 },   /* Shipping 4x10                       */
    { 0x03E2, 1200,  694 },   /* Large shipping horizontal           */
    { 0x0275, 1050,  329 },   /* Address horizontal                  */
    { 0xFFFF, 1282, 32000 },  /* Banner 4 (continuous)               */
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
