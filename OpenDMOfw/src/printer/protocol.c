/* OpenDMOfw - host wire-protocol parser (byte-driven, resumable state machine).
 *
 * Implements the LabelWriter 550/5XL host protocol exactly as published in the
 * official "LabelWriter 550 Series Printers Technical Reference Manual" (the
 * "tech ref") and as confirmed by the decompiled stock PC-side software
 * (D.MO Connect / port monitor):
 *
 *   ESC s <JobID u32>    start of print job (job ID is echoed in status)
 *   ESC L <len u16 BE>   label length (dots), used for feed math; 0x7F00 =
 *                         custom size and 0xFFFF = continuous are sentinels
 *   ESC h / ESC i        text / graphics output mode
 *   ESC T <speed>        1B 54 (the tech ref's "1B 74" is a typo: 0x74 is 't'),
 *                         0x10 normal, 0x20 high speed
 *   ESC n <idx u16>      set label index (echoed in status)
 *   ESC D BPP Align W H  start of label print data: W = number of lines,
 *                         H = number of dots; then W * roundup(H*BPP/8) bytes
 *   ESC G                feed to print head (short form feed, between labels)
 *   ESC E                feed to tear position (long form feed)
 *   ESC Q                end of print job (releases the lock)
 *   ESC A <lock>         request status -> 32-byte struct on bulk-IN
 *   ESC C <duty>         print density, 0-200 % (0 = printing disabled)
 *   ESC c / d / e / g    zero-argument density presets: Light 75 %,
 *                         Medium 87.5 %, Normal 100 % (the default), Dark 112.5 %
 *   ESC y / ESC z        400-series step-resolution commands, no argument,
 *                         accepted and ignored (this family is 300x300 only)
 *   ESC U                get SKU info -> 63-byte consumable record
 *   ESC V                get version -> 34-byte reply
 *   ESC $                restore factory settings (config back to defaults).
 *                         0x24 is the byte in the tech ref and the one the host
 *                         tool sends; 0x2A ('*') is accepted as an alias because
 *                         earlier revisions of this firmware only had that one.
 *   ESC o <count u8>     set label count (one argument byte, tech ref p.20)
 *   ESC q <roll>         select roll/tray, ASCII '0'-'3' (Twin Turbo only);
 *                         accepted, ignored
 *   ESC ESC ...          a run of bare ESC bytes collapses to one pending ESC
 *   ESC W len dir objid  control-command framing; payload consumed, ignored
 *   ESC M <8 bytes>      media-type descriptor (mtDefault = 8 zero bytes);
 *                         the driver always sends it, so consume + ignore
 *   ESC @                restart print engine -> full pipeline reset here
 *
 * Backdoor commands (never sent by the stock host, kept for configuration and
 * driver-less bring-up via tools/opsend.py):
 *   GS C len lo hi sku.. 1D 43 .. set roll config (SKU + count) in EEPROM
 *   GS D sub [arg]         1D 44 ..  self-test / diagnostic (see diagnose()):
 *                                0x01 <n> strobe head n lines, 0x02 <n> step motor,
 *                                0x03 EEPROM self-test, 0x04 diagnostic snapshot.
 *                                Replies are 'D'-prefixed so they can't be mistaken
 *                                for a 32-byte status struct.
 *
 * The parser is fed byte-by-byte and keeps its position: if the ring buffer
 * runs dry in the middle of a raster block it resumes on the next
 * protocol_task() call without losing the job. The USB IRQ only fills the
 * ring (protocol_feed); head/motor run here in main-loop context.
 */
#include "protocol.h"
#include "head.h"
#include "motor.h"
#include "thermal.h"
#include "paper.h"
#include "../config/store.h"
#include "../usb/usb_core.h"
#include "../usb/usb_printer.h"
#include "../pins.h"
#include "../system.h"

#define RING_SZ 2048u                 /* power of two */
static uint8_t  s_ring[RING_SZ];
static volatile uint16_t s_head, s_tail;
static int s_rx_paused;

static uint16_t ring_used(void){ return (uint16_t)((s_head - s_tail) & (RING_SZ-1)); }
static uint16_t ring_free(void){ return RING_SZ - 1 - ring_used(); }

/* ---- parser state (persistent across protocol_task calls) --------------- */
typedef enum {
    S_CMD,          /* waiting for a command start byte */
    S_AFTER_ESC,    /* saw 0x1B */
    S_ARG1,         /* one argument byte  (s_arg1 = which command) */
    S_ARG2,         /* two argument bytes: LE for ESC n, BE for ESC L */
    S_ARG4,         /* four argument bytes, u32 LE (s_arg1 = which command) */
    S_ESC_D,        /* ESC D: BPP, Align, W(4), H(4) then raster */
    S_RASTER,       /* consuming raster lines for one label */
    S_ESC_W,        /* ESC W: 4 header bytes then len payload bytes */
    S_ESC_F,        /* ESC f: sub-command byte then one argument byte */
    S_SKIP,         /* consume s_w_payload raw bytes (ESC M media type) */
    S_AFTER_GS,     /* saw 0x1D (backdoor config commands) */
    S_GSC_HDR,      /* GS C: 3 header bytes (len, cntLo, cntHi) */
    S_GSC_SKU,      /* GS C: len SKU bytes */
    S_DIAG_SUB,     /* GS D: one subcommand byte */
    S_DIAG_ARG      /* GS D: one argument byte (strobe/step count) */
} pstate_t;

static pstate_t s_state;
static uint8_t  s_arg1, s_arg2, s_arg4[4];
static uint8_t  s_diag_sub;        /* GS D subcommand */
static uint8_t  s_diag_args[3];    /* GS D argument bytes */
static uint8_t  s_diag_argn, s_diag_argi;
static uint8_t  s_hdr[10];            /* ESC D header (10 B) / ESC W header (4 B) */
static uint8_t  s_hcnt;
static uint16_t s_bpl;               /* raster bytes per line ON THE WIRE */
static uint16_t s_use;               /* bytes of that line the head can print */
static uint16_t s_lines_left;        /* lines still to print for this label */
static uint16_t s_line_rx;           /* bytes received for the current line */
static uint8_t  s_line[HEAD_BYTES];
static uint16_t s_xoff;              /* left padding (bytes) to center narrow rasters */
static uint16_t s_w_payload;         /* ESC W payload bytes still to consume */
static uint8_t  s_sku_len, s_sku_i;

/* Job context for the status struct. */
static uint32_t s_job_id;            /* from ESC s (host-assigned) */
static uint16_t s_label_index;       /* from ESC n (host-assigned) */
static int      s_job_active;        /* 1 from ESC s until ESC Q */
static uint8_t  s_density_pct;       /* last ESC C duty, 0-200, reported in status */
static const paper_t *s_paper;       /* current stock, from ESC L (feed + ESC U) */
static uint16_t s_len_override;      /* ESC L value treated as a raw dot length */
static int      s_len_from_raster;   /* continuous / custom size: pitch = raster height */
static uint16_t s_raster_dots;       /* height (dots) of the current raster block */

/* Feed math: die-cut rolls have a small physical gap between labels. */
#define LABEL_GAP_DOTS   20          /* ~1.7 mm at 300 dpi */
#define TEAR_EXTRA_DOTS  15          /* tear bar sits past the next print position */
/* Hard ceiling on a single feed. The paper table carries continuous/banner
 * stock with a nominal height of 32000 dots; without this, a short label on
 * that stock would make ESC G spool out 32000 lines = 2.7 metres. The largest
 * real die-cut pitch in either table is 3150 dots (PC Postage 30387, 10").
 * Continuous stock has no inter-label pitch to honour anyway. */
#define MAX_FEED_DOTS    4000        /* ~34 cm */

void protocol_init(void)
{
    s_head = s_tail = 0; s_rx_paused = 0;
    s_state = S_CMD;
    s_job_active = 0; s_label_index = 0; s_job_id = 0;
    s_density_pct = 100;
    s_paper = paper_lookup(PAPER_DEFAULT_CODE);
    s_len_override = 0; s_raster_dots = 0; s_len_from_raster = 0;
}

void protocol_reset(void)
{
    s_head = s_tail = 0;
    s_state = S_CMD; s_hcnt = 0; s_lines_left = 0; s_line_rx = 0;
    s_job_active = 0; s_label_index = 0;
    s_paper = paper_lookup(PAPER_DEFAULT_CODE);
    s_len_override = 0; s_raster_dots = 0; s_len_from_raster = 0;
    /* Dropping the ring also drops the reason bulk-OUT was throttled. The
     * un-pause in ring_getc() only fires when a byte is actually read, so an
     * empty ring would leave the endpoint NAKing forever after a SOFT_RESET
     * that arrived while the ring was backed up. Re-arm it here. */
    s_rx_paused = 0;
    if (usb_is_configured()) usb_ep_rx_ready(EP_DATA);
    head_reset();
}

void protocol_feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (ring_free() == 0) break;
        s_ring[s_head] = data[i];
        s_head = (s_head + 1) & (RING_SZ - 1);
    }
    if (ring_free() < EP_MAXPKT) {
        s_rx_paused = 1;
    } else {
        s_rx_paused = 0;
        usb_ep_rx_ready(EP_DATA);
    }
}

static int ring_getc(void)
{
    if (s_head == s_tail) return -1;
    int c = s_ring[s_tail];
    s_tail = (s_tail + 1) & (RING_SZ - 1);
    if (s_rx_paused && ring_free() >= EP_MAXPKT) {
        s_rx_paused = 0;
        usb_ep_rx_ready(EP_DATA);
    }
    return c;
}

/* ---- helpers ------------------------------------------------------------- */
static void set_density(uint8_t pct)
{
    s_density_pct = (pct <= 200) ? pct : 200;
    /* head density 1..16 ~= 12.5..200 % of the reference duty cycle */
    uint8_t d = (s_density_pct == 0) ? 0
             : (uint8_t)(((uint32_t)s_density_pct * 8u + 50u) / 100u);
    if (d > 16) d = 16;
    /* Only ESC C 0 means "disable printing" (tech ref p.16). 1-6 % must still
     * print, however faintly - rounding them down to 0 would silently turn the
     * heat off on a host that just wanted a very light label. */
    if (d == 0 && s_density_pct != 0) d = 1;
    head_set_density(d);
}

/* One completed raster block = one label. The counter decrements and persists
 * to EEPROM so it survives a power cycle (like the genuine roll counter, but
 * plain configuration - no tag). When it runs out it wraps to a fresh roll so
 * the host never sees an empty one. */
static void label_printed(void)
{
    op_config_t *cfg = store_get_mut();
    if (cfg->label_count > 1) {
        cfg->label_count--;
    } else {
        cfg->label_count = MODEL_DEFAULT_COUNT;   /* fresh roll */
    }
    store_save();
    s_label_index++;
}

/* `bpl` is the width the HOST will actually put on the wire; it must never be
 * clamped, or the surplus bytes of an over-wide line get parsed as the next
 * line and the whole raster desynchronises. Clamp only what we hand to the
 * head (s_use); the remainder of each line is consumed and discarded.
 * The driver caps the printable width at the head width (GPD MaxPrintableWidth:
 * 1248 for 5XL, 672 for 550), so s_use == s_bpl in normal operation. A narrower
 * raster is centered on the head (matches PrintableOrigin geometry). */
static void begin_raster(uint16_t lines, uint16_t dots, uint8_t bpp)
{
    s_raster_dots = dots;
    s_bpl = (uint16_t)(((uint32_t)dots * bpp + 7u) / 8u);
    s_use = (s_bpl > HEAD_BYTES) ? (uint16_t)HEAD_BYTES : s_bpl;
    s_lines_left = lines; s_line_rx = 0;
    s_xoff = (uint16_t)((HEAD_BYTES - s_use) / 2);
    if (s_bpl == 0 || lines == 0) { s_state = S_CMD; return; }
    s_job_active = 1;
    s_state = S_RASTER;
}

/* Advance the paper to the next label position. The stock pitch comes from the
 * ESC L paper code (height in the GPD table) or a raw dot length; plus the
 * physical inter-label gap. ASSUMPTION (verify on hardware): the die-cut gap
 * and tear-bar offset are fixed dot counts, not read from the roll. */
static void feed_next_label(int to_tear)
{
    uint16_t pitch = s_len_from_raster ? s_raster_dots
                   : s_len_override     ? s_len_override
                   : (s_paper ? s_paper->height_dots : s_raster_dots);
    uint32_t dots = LABEL_GAP_DOTS;
    if (pitch > s_raster_dots) dots += (uint32_t)(pitch - s_raster_dots);
    if (to_tear) dots += TEAR_EXTRA_DOTS;
    if (dots > MAX_FEED_DOTS) dots = MAX_FEED_DOTS;
    motor_step_lines((uint16_t)dots);
}

/* ---- 32-byte status struct (layout per tech ref p.13-16) -------- */
static void send_status(void)
{
    const op_config_t *c = store_get();
    uint8_t r[32];
    for (int i = 0; i < 32; i++) r[i] = 0;

    r[0] = s_job_active ? 1 : 0;                 /* PrintStatus: Printing/Idle */
    r[1] = (uint8_t)(s_job_id & 0xFF);           /* PrintJobID u32 LE */
    r[2] = (uint8_t)((s_job_id >> 8) & 0xFF);
    r[3] = (uint8_t)((s_job_id >> 16) & 0xFF);
    r[4] = (uint8_t)((s_job_id >> 24) & 0xFF);
    r[5] = (uint8_t)(s_label_index & 0xFF);      /* LabelIndex u16 LE */
    r[6] = (uint8_t)((s_label_index >> 8) & 0xFF);
    r[7] = 0;                                    /* Reserved */
    r[8] = 0;                                    /* PrintHeadStatus: ok */
    r[9] = s_density_pct;                        /* PrintDensity % (0-200) */
    r[10] = usbp_paper_present() ? 8 : 2;        /* MainBayStatus: ok / no media */
    for (int i = 0; i < 12; i++) {               /* SKU info, NUL-padded */
        if (i < OP_SKU_MAX && c->sku[i]) r[11 + i] = (uint8_t)c->sku[i];
    }
    /* bytes 23..26 ErrorID = 0 */
    r[27] = (uint8_t)(c->label_count & 0xFF);    /* LabelCount u16 LE */
    r[28] = (uint8_t)((c->label_count >> 8) & 0xFF);
    r[29] = 0x01;                                /* EPS status: present */
    r[30] = 0x01;                                /* PrintHeadVoltage: ok */
    r[31] = 0xFF;                                /* Reserved (default -1) */
    usbp_send_reply(r, sizeof(r));
}

/* ---- ESC U: 63-byte consumable record (tech ref p.16-19) --------
 *
 * This layout is no longer read off the manual alone. The repository root of
 * this very project (free-dmo-stm32, Src/main.c) embeds 37 dumps of GENUINE
 * DYMO 550-series roll tags, and the consumable record sits at tag offset 12
 * in every one of them. Checking our fields against all 37 settled several
 * things the manual gets wrong or leaves ambiguous:
 *
 *   - Byte 3 is a CONSTANT 0x3C (60) in 37/37, regardless of SKU length. It is
 *     the payload length: an 8-byte header plus 60 payload bytes, and the next
 *     record's magic begins at exactly offset 68 on every tag. It is not the
 *     SKU length, which is what we used to send.
 *   - Bytes 4-7 are a 32-bit CRC, not a CRC16 with two reserved bytes. It is
 *     plain CRC-32/ISO-HDLC (zlib) over bytes 0..59 with 4..7 zeroed, stored
 *     little-endian: 37/37. The manual's "Byte 7...Byte 4 | b15...b0 | CRC" row
 *     is right about the span and wrong about the width.
 *   - Every geometry field is in units of 0.1 mm, not mm. SKU 30256 carries
 *     1016 x 587 = 101.6 x 58.7 mm, i.e. exactly 4" x 2.3125". We used to
 *     report whole mm, so the host saw every dimension ten times too small.
 *   - Bytes 52-53 are the total media length in 2 mm units, not mm. The
 *     continuous roll 30270 carries 45720, and 45720 x 2 mm = 91440 mm = 300 ft
 *     exactly - a length no u16 could hold in mm.
 *   - Bytes 44-47 (printable-area offsets) and 60-62 are zero in 37/37; byte 56
 *     (counter strategy) is 0x01 in 37/37, not the 0x00 the manual describes;
 *     bytes 54-55 (counter margin) carry count/10 in 36/37.
 *
 * Constants below are taken from the genuine record for our own default SKU
 * S0904980 where the field is per-roll rather than universal.
 */

/* CRC-32/ISO-HDLC (zlib): poly 0x04C11DB7 reflected, init 0xFFFFFFFF, xorout
 * 0xFFFFFFFF. Bit-at-a-time; 60 bytes once per ESC U is not worth a table. */
static uint32_t crc32_iso(const uint8_t *d, uint16_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return ~c;
}

/* Dots -> TENTHS of a millimetre at the model DPI, rounded to nearest.
 * 254 tenths per inch. */
static uint16_t dots_to_tenth_mm(uint16_t dots)
{
    return (uint16_t)(((uint32_t)dots * 254u + MODEL_DPI / 2u) / MODEL_DPI);
}

/* Inter-label gap, in tenths. Genuine rolls carry 42-118 depending on the
 * stock (mode 42); 57 is the value on our default S0904980. Per-roll in
 * reality, so this is a default rather than a constant. */
#define LABEL_GAP_TENTH_MM 42

static void send_sku_record(void)
{
    const op_config_t *c = store_get();
    const paper_t *p = s_paper ? s_paper : paper_lookup(PAPER_DEFAULT_CODE);
    uint8_t r[63];
    for (int i = 0; i < 63; i++) r[i] = 0;

    /* 0.1 mm values from the configured paper's dot dimensions at MODEL_DPI */
    uint16_t w_tmm = dots_to_tenth_mm(p->width_dots);
    uint16_t h_tmm = dots_to_tenth_mm(p->height_dots);

    r[0] = 0xB6; r[1] = 0xCA;                    /* magic 0xCAB6 LE */
    r[2] = 0;                                    /* version */
    r[3] = 0x3C;                                 /* payload length: 60 (37/37) */
    /* bytes 4-7 hold the CRC, computed last */
    for (int i = 0; i < 12; i++)                 /* SKU, NUL-padded */
        if (i < OP_SKU_MAX && c->sku[i]) r[8 + i] = (uint8_t)c->sku[i];
    r[20] = 0x00;                                /* brand: DYMO */
    r[21] = 0xFF;                                /* region: global */
    /* Material: the manual's 0x00-0x07 enum does not describe real tags, which
     * use 0x02/0x04/0x06/0x08 and 0x20/0x23/0x24/0x25/0x26. 0x03 ("paper")
     * appears on none of the 37. 0x04 is what our default SKU S0904980 carries. */
    r[22] = 0x04;
    r[23] = 0x01;                                /* label type: die-cut (33/37) */
    r[24] = 0x01;                                /* label color: white */
    r[25] = 0x00;                                /* content color: black */
    r[26] = 0x00;                                /* marker type 0 */
    uint16_t pitch_tmm = (uint16_t)(h_tmm + LABEL_GAP_TENTH_MM);
    r[28] = (uint8_t)(pitch_tmm & 0xFF); r[29] = (uint8_t)(pitch_tmm >> 8);
    r[30] = 30; r[31] = 0;                       /* marker1 width 3.0 mm (35/37) */
    r[32] = 38; r[33] = 0;                       /* marker1 to label start; per-roll, 38 = S0904980 */
    /* marker2 unused (type 0) */
    r[38] = 16; r[39] = 0;                       /* vertical offset 1.6 mm (23/37) */
    r[40] = (uint8_t)(h_tmm & 0xFF); r[41] = (uint8_t)(h_tmm >> 8);   /* label length */
    r[42] = (uint8_t)(w_tmm & 0xFF); r[43] = (uint8_t)(w_tmm >> 8);   /* label width  */
    /* bytes 44-47 printable-area offsets: zero on 37/37 genuine rolls */
    uint16_t liner_tmm = dots_to_tenth_mm(HEAD_DOTS);
    r[48] = (uint8_t)(liner_tmm & 0xFF);         /* liner is a little wider than */
    r[49] = (uint8_t)(liner_tmm >> 8);           /* the head; head width is our best guess */
    /* Bytes 50-51 are the roll's TOTAL label count and 52-53 the roll's total
     * media length, not what is left on it - the remaining count is the status
     * struct's job (bytes 27-28). Reporting the remaining count here would make
     * the "roll" appear to shrink as it is used. */
    uint16_t total_count = MODEL_DEFAULT_COUNT;
    if (c->label_count > total_count) total_count = c->label_count;
    r[50] = (uint8_t)(total_count & 0xFF);
    r[51] = (uint8_t)(total_count >> 8);
    uint32_t total_2mm = ((uint32_t)pitch_tmm * total_count) / 20u;   /* 2 mm units */
    if (total_2mm > 0xFFFF) total_2mm = 0xFFFF;
    r[52] = (uint8_t)(total_2mm & 0xFF); r[53] = (uint8_t)(total_2mm >> 8);
    uint16_t margin = (uint16_t)(total_count / 10u);   /* 36/37 genuine rolls */
    r[54] = (uint8_t)(margin & 0xFF); r[55] = (uint8_t)(margin >> 8);
    r[56] = 0x01;                                /* counter strategy (37/37) */
    /* bytes 57-62 zero: genuine tags carry nothing past byte 59, and the
     * manual's "production date/time" rows have no counterpart in real data */

    uint32_t crc = crc32_iso(r, 60);             /* bytes 0..59, 4..7 already zero */
    r[4] = (uint8_t)(crc & 0xFF);
    r[5] = (uint8_t)((crc >> 8) & 0xFF);
    r[6] = (uint8_t)((crc >> 16) & 0xFF);
    r[7] = (uint8_t)((crc >> 24) & 0xFF);
    usbp_send_reply(r, sizeof(r));
}

/* ---- ESC V: 34-byte version reply (tech ref p.20) --------------- */
static void send_version(void)
{
    uint8_t r[34];
    for (int i = 0; i < 34; i++) r[i] = 0;
    /* Two 16-char fields, zero-padded (tech ref p.20). The FW field's internal
     * structure is fixed by the manual - "FWAP"/"FWBL", major, minor, MMYY,
     * four chars each - see MODEL_FW_VERSION_COMMON in model.h. */
    static const char hw[] = MODEL_HW_VERSION;
    static const char fw[] = MODEL_FW_VERSION;
    for (int i = 0; i < 16 && i < (int)sizeof(hw) - 1; i++) r[i]      = (uint8_t)hw[i];
    for (int i = 0; i < 16 && i < (int)sizeof(fw) - 1; i++) r[16 + i] = (uint8_t)fw[i];
    r[32] = (uint8_t)(MODEL_PID & 0xFF);         /* USB PID LE */
    r[33] = (uint8_t)(MODEL_PID >> 8);
    usbp_send_reply(r, sizeof(r));
}

/* ---- GS D: self-test / diagnostic backdoor (never sent by the stock host) --
 * Subcommands (GS D <sub> [arg]):
 *   0x01 <n>  strobe the head for n all-on lines at max density (verify head drive);
 *             thermally gated, the reply says how many lines actually fired
 *   0x02 <n>  step the feed motor n dot-lines (verify feed)
 *   0x03      EEPROM self-test -> reply match status
 *   0x05      firmware build id
 *   0x06      scan: every ADC channel + every port's input levels
 *   0x07 p n c  toggle port p pin n, c times (USB/SWD refused)
 *   0x08 <0|1>  clear/set the VH interlock, persisted
 *   0x04      diagnostic snapshot -> thermistor, GPIOs, config, model
 * Every reply starts with 'D' so the host can tell it apart from a status struct.
 * These exist so the physical layer (head/motor/EEPROM/thermistor) can be verified
 * on hardware without a full print job - see DECISIONS.md D8 / D15. */
/* How many argument bytes each subcommand takes. */
static uint8_t diag_argcount(uint8_t sub)
{
    switch (sub) {
    case 0x01: case 0x02: case 0x08: return 1;
    case 0x07: return 3;
    default:   return 0;
    }
}

static void diagnose(uint8_t sub)
{
    uint8_t arg = s_diag_args[0];
    const op_config_t *c = store_get();
    /* 24 B is the snapshot; the rest is headroom for the build-id reply. Every
     * GS D reply stays well inside one 64-byte bulk packet. */
    uint8_t r[2 + OP_BUILD_ID_MAX];
    for (unsigned i = 0; i < sizeof(r); i++) r[i] = 0;
    r[0] = 'D';                       /* marker: distinguishes from a status struct */
    r[1] = sub;                       /* echo the subcommand (uniform across cases) */

    switch (sub) {
    case 0x01: {                              /* strobe head n all-on lines */
        uint8_t line[HEAD_BYTES];
        uint8_t fired = 0;
        for (int i = 0; i < HEAD_BYTES; i++) line[i] = 0xFF;
        head_set_density(16);                 /* max dwell so it is unmistakable */
        for (uint8_t i = 0; i < arg; i++) {
            /* DECISIONS D7: every line is gated on thermal_ok(). This path runs
             * all dots on at maximum dwell, so unlike the print path it does not
             * fall through after the wait - it stops and reports how far it got. */
            for (int g = 0; g < 100 && !thermal_ok(); g++) { delay_ms(10); wdt_kick(); }
            if (!thermal_ok()) break;
            head_print_line(line, HEAD_BYTES);
            fired++;
            wdt_kick();
        }
        set_density(s_density_pct);           /* restore the configured base level */
        r[2] = fired;                         /* lines actually fired (<= arg) */
        r[3] = thermal_ok() ? 1 : 0;
        usbp_send_reply(r, 4);
        break; }
    case 0x02:                                /* step feed motor n dot-lines */
        motor_step_lines(arg);
        r[2] = arg;
        usbp_send_reply(r, 3);
        break;
    case 0x03:                                /* EEPROM write/read self-test */
        r[2] = store_selftest() ? 1 : 0;
        usbp_send_reply(r, 3);
        break;
    case 0x06: {                              /* scan every findable input */
        /* Exploration aid: one reply with all ten ADC channels and the input
         * level of every pin on ports A/B/C. Warm the head and diff two scans
         * to find the thermistor; block the sensor and diff to find the
         * photocell. Turns two of the seven fieldwork measurements from
         * "trace it" into "read the table".
         * The rail is dropped first: sampling means briefly floating pins,
         * including the strobes, and a floating strobe with 24 V behind it is
         * the one mistake that costs a print head. */
        head_vh_off();
        uint16_t adc[10];
        thermal_scan_adc(adc);
        for (int i = 0; i < 10; i++) {
            r[2 + i*2] = (uint8_t)(adc[i] >> 8);
            r[3 + i*2] = (uint8_t)(adc[i] & 0xFF);
        }
        for (int prt = 0; prt < 3; prt++) {
            uint16_t idr = sys_port_idr((uint8_t)prt);
            r[22 + prt*2] = (uint8_t)(idr & 0xFF);
            r[23 + prt*2] = (uint8_t)(idr >> 8);
        }
        r[28] = (uint8_t)((head_vh_is_on() ? 1u : 0u) |
                          ((c->flags & OP_FLAG_VH_INHIBIT) ? 2u : 0u));
        usbp_send_reply(r, 29);
        break; }
    case 0x07:                                /* toggle an arbitrary pin */
        /* Find a signal by driving a candidate and watching what moves. USB
         * and SWD pins are refused - toggling those ends the session instead
         * of answering the question. */
        r[2] = sys_pin_toggle(s_diag_args[0], s_diag_args[1], s_diag_args[2]) ? 1 : 0;
        usbp_send_reply(r, 3);
        break;
    case 0x08: {                              /* set/clear the VH interlock */
        op_config_t *m = store_get_mut();
        if (arg) m->flags |= OP_FLAG_VH_INHIBIT;
        else     m->flags &= (uint8_t)~OP_FLAG_VH_INHIBIT;
        if (m->flags & OP_FLAG_VH_INHIBIT) head_vh_off();
        store_save();
        r[2] = m->flags;
        usbp_send_reply(r, 3);
        break; }
    case 0x05: {                              /* firmware build id (ASCII) */
        const char *b = OPENDMO_BUILD;
        uint8_t n = 0;
        while (n < OP_BUILD_ID_MAX && b[n]) { r[2 + n] = (uint8_t)b[n]; n++; }
        usbp_send_reply(r, (uint16_t)(2 + n));
        break; }
    case 0x04:                                /* diagnostic snapshot */
    default: {
        uint16_t traw = thermal_read_raw();
        r[2] = (uint8_t)(MODEL_PID & 0xFF);   /* model id (PID low byte) */
        r[3] = (uint8_t)(traw >> 8); r[4] = (uint8_t)(traw & 0xFF);      /* thermistor raw (BE) */
        r[5] = thermal_ok() ? 1 : 0;
        r[6] = (gpio_get(PIN_PAPER_SENSE) == PAPER_PRESENT_LEVEL) ? 1 : 0;
        if (gpio_get(PIN_BUTTON) == BUTTON_PRESSED_LEVEL) r[6] |= 2;
        r[7] = s_density_pct;
        r[8] = c->flags;
        for (int i = 0; i < 12; i++) if (i < OP_SKU_MAX && c->sku[i]) r[9 + i] = (uint8_t)c->sku[i];
        r[21] = (uint8_t)(c->label_count & 0xFF); r[22] = (uint8_t)(c->label_count >> 8);
        r[23] = HEAD_BYTES;
        usbp_send_reply(r, 24);
        break; }
    }
}

static void factory_reset(void)
{
    op_config_t *cfg = store_get_mut();
    const char *d = MODEL_DEFAULT_SKU;
    uint8_t i = 0;
    for (; i < OP_SKU_MAX - 1 && d[i]; i++) cfg->sku[i] = d[i];
    cfg->sku[i] = 0;
    cfg->label_count = MODEL_DEFAULT_COUNT;
    cfg->density = 8;
    cfg->flags = OP_FLAG_PAPER_FORCE;
    store_save();
    set_density(100);
}

static void emit_line(void)
{
    /* Center the raster on the head (narrow label, wide head); a raster wider
     * than the head is left-aligned and clipped at the buffer edge. */
    uint8_t tmp[HEAD_BYTES];
    for (uint16_t i = 0; i < HEAD_BYTES; i++)
        tmp[i] = (i >= s_xoff && (i - s_xoff) < s_use) ? s_line[i - s_xoff] : 0;
    for (uint16_t i = 0; i < HEAD_BYTES; i++) s_line[i] = tmp[i];
    wdt_kick();
    /* Bounded cool-down wait (max ~1 s), keep feeding the watchdog. After that
     * print anyway: the dwell is already thermally reduced, so this never hangs. */
    for (int i = 0; i < 100 && !thermal_ok(); i++) { delay_ms(10); wdt_kick(); }
    head_print_line(s_line, HEAD_BYTES);
    /* Credit the strobe time against the step's settle delay: the paper may
     * not move during the heat pulse, but the pulse is dead time the step
     * would otherwise wait out a second time. */
    motor_step_line_after(head_last_strobe_us());
    wdt_kick();
}

/* ---- host-independent button actions (see main.c) ----------------------- */
void protocol_form_feed(void)
{
    if (s_job_active) return;          /* never fight a running host job */
    feed_next_label(1);
}

/* Built-in canned test pattern. The genuine printer has one too - 550 tech ref
 * p.8, "a repeating series of test patterns" on a long button press - and it is
 * the only way to prove the head and the feed on a bench with no host at all.
 * Border + diagonals: the border shows head width and both edges, the diagonals
 * show dropouts and feed regularity. */
#define SELFTEST_LINES 400u            /* ~34 mm at 300 dpi */

void protocol_self_test(void)
{
    if (s_job_active) return;
    uint8_t line[HEAD_BYTES];
    int released = 0;

    for (uint16_t y = 0; y < SELFTEST_LINES; y++) {
        for (uint16_t b = 0; b < HEAD_BYTES; b++) line[b] = 0;
        for (uint16_t x = 0; x < HEAD_DOTS; x++) {
            int edge = (x < 8) || (x >= HEAD_DOTS - 8) ||
                       (y < 8) || (y >= SELFTEST_LINES - 8);
            int diag = ((x + y) % 32u) < 2u;
            if (edge || diag) line[x >> 3] |= (uint8_t)(0x80u >> (x & 7u));
        }
        for (int g = 0; g < 100 && !thermal_ok(); g++) { delay_ms(10); wdt_kick(); }
        if (!thermal_ok()) break;                    /* D7: never strobe over the limit */
        head_print_line(line, HEAD_BYTES);
        motor_step_line_after(head_last_strobe_us());
        wdt_kick();
        /* A second press stops it, as on the genuine printer. The button is
         * still held when we start, so wait for a release first. */
        if (gpio_get(PIN_BUTTON) != BUTTON_PRESSED_LEVEL) released = 1;
        else if (released) break;
    }
    feed_next_label(1);                              /* present it at the tear bar */
}

/* Process as many bytes as are available; resume exactly where we stopped. */
void protocol_task(void)
{
    /* By default the roll state is pure config (OP_FLAG_PAPER_FORCE), so any
     * physical roll reports "present" to the host. Only when that flag is cleared
     * do we track the real paper sensor for the status byte. The LED still reads
     * the sensor directly (main.c) either way. */
    if (!(store_get()->flags & OP_FLAG_PAPER_FORCE))
        usbp_set_paper_present(gpio_get(PIN_PAPER_SENSE) == PAPER_PRESENT_LEVEL);

    int ci;
    while ((ci = ring_getc()) >= 0) {
        uint8_t c = (uint8_t)ci;
        switch (s_state) {
        case S_CMD:
            if (c == 0x1B)       s_state = S_AFTER_ESC;
            else if (c == 0x1D)  s_state = S_AFTER_GS;
            /* anything else (padding, stray bytes): ignore */
            break;

        case S_AFTER_ESC:
            switch (c) {
            /* A run of bare ESC bytes collapses to one pending escape. The
             * 400/450-generation CUPS driver opens every document with 100-156
             * of them as flush padding; without this case each extra ESC took
             * the unknown-command path and ate the following byte, so a run
             * whose length is not a multiple of 3 swallowed the next real
             * command - catastrophic if that was an ESC D. No DYMO command is
             * ESC ESC. */
            case 0x1B: break;
            /* ESC y / ESC z: zero-argument step-resolution commands of the
             * 400 series (300x300 / 203x300). The 550 family is 300x300 in
             * every mode, so accept them with no effect - but they must not
             * reach the unknown-command path, which would eat the next byte. */
            case 'y':
            case 'z': s_state = S_CMD; break;
            case 's': s_arg1 = 's'; s_arg2 = 0; s_state = S_ARG4; break; /* job + ID */
            case 'L': s_arg1 = 'L'; s_arg2 = 0; s_state = S_ARG2; break; /* paper code */
            case 'n': s_arg1 = 'n'; s_arg2 = 0; s_state = S_ARG2; break; /* label index */
            /* ESC o is ONE argument byte. The 550 tech ref p.20 shows
             * "Byte 0 1 2 / 'ESC' 'o' Count" - three bytes total - exactly as it
             * shows two-byte tables for ESC n and ESC L. Some notes on the
             * decompiled driver claim u16. Reading one byte is safe under both
             * readings: a u16's high byte is 0x00, which S_CMD ignores as a
             * stray. Reading two when the host sent one would swallow the next
             * command's ESC and wreck the rest of the job. Use GS C for counts
             * above 255. */
            case 'o': s_arg1 = 'o'; s_state = S_ARG1; break;    /* set count   */
            case 'f': s_hcnt = 0; s_state = S_ESC_F; break;     /* skip n lines */
            case 'A': s_arg1 = 'A'; s_state = S_ARG1; break;    /* status + lock */
            case 'C': s_arg1 = 'C'; s_state = S_ARG1; break;    /* density       */
            case 'T': s_arg1 = 'T'; s_state = S_ARG1; break;    /* speed         */
            case 'q': s_arg1 = 'q'; s_state = S_ARG1; break;    /* roll select: ASCII '0'-'3', Twin Turbo only */
            /* The zero-argument print-density family (LW450 tech ref p.19,
             * and emitted by the CUPS driver's SetPrintDensity for the PPD's
             * Light/Medium/Normal/Dark choices). ESC d used to be our feed
             * backdoor, which collided head-on with a genuine opcode: a host
             * sending ESC d then ESC L would have had the 0x1B eaten as a feed
             * count. The feed lives on GS D 0x02 and ESC f 1 n instead. */
            case 'c': set_density(75);  s_state = S_CMD; break;  /* Light   75 %   */
            case 'd': set_density(88);  s_state = S_CMD; break;  /* Medium  87.5 % */
            case 'g': set_density(113); s_state = S_CMD; break;  /* Dark   112.5 % */
            case 'D': s_hcnt = 0; s_state = S_ESC_D; break;     /* raster header */
            case 'W': s_hcnt = 0; s_state = S_ESC_W; break;     /* control cmd   */
            case 'M': s_w_payload = 8; s_state = S_SKIP; break; /* media type +8B */
            case 'h': s_state = S_CMD; break;                   /* text mode     */
            case 'i': s_state = S_CMD; break;                   /* graphics mode */
            case 'G': feed_next_label(0); s_state = S_CMD; break;  /* short feed  */
            case 'E': feed_next_label(1); s_state = S_CMD; break;  /* tear feed   */
            case 'Q':                                 /* end of job / unlock     */
                s_job_active = 0; s_label_index = 0;
                s_raster_dots = 0;   /* no printed height carries into the next job */
                s_state = S_CMD; break;
            case 'e': set_density(100); s_state = S_CMD; break;  /* Normal 100 % */
            case 'U': send_sku_record(); s_state = S_CMD; break;
            case 'V': send_version();    s_state = S_CMD; break;
            case '$':                                 /* 0x24, per the tech ref */
            case '*': factory_reset();   s_state = S_CMD; break;  /* 0x2A alias */
            case '@': protocol_reset();  s_state = S_CMD; return; /* pipeline reset */
            default:  s_arg1 = '?'; s_state = S_ARG1; break;     /* unknown: skip 1 arg */
            }
            break;

        case S_ARG1:
            switch (s_arg1) {
            case 'A': send_status(); break;
            case 'C': set_density(c); break;
            case 'o': store_get_mut()->label_count = c; store_save(); break;
            /* 'T' speed, 'q' tray, '?': accept and ignore */
            }
            s_state = S_CMD;
            break;

        case S_ARG2:
            if (s_arg2 == 0) { s_arg2++; s_arg4[0] = c; break; }
            s_arg4[1] = c;
            {
                /* ESC n is u16 LE; ESC L is u16 BIG-endian. Three independent
                 * sources: DYMO's own CUPS driver (SendLabelLength writes
                 * (v>>8) then v&0xff, with a unit test pinning ESC L 12 34 for
                 * 0x1234), Microsoft's GPD rule that <1B>L<0867> puts bytes
                 * 08 67 on the wire in that order, and our own paper table -
                 * read big-endian, 12 of 14 LW5XX.GPD entries are exactly
                 * height_dots + 300, while byte-swapped they are noise. */
                uint16_t v_le = (uint16_t)(s_arg4[0] | (c << 8));
                uint16_t v_be = (uint16_t)(((uint16_t)s_arg4[0] << 8) | c);
                switch (s_arg1) {
                case 'L': {
                    /* ESC L carries a label LENGTH in dots, not a paper id: in
                     * the stock LW5XX.GPD 53 of 56 sized papers emit
                     * page_height + 300, and nine different papers share
                     * 0x0546. Two values are sentinels there:
                     *   0x7F00  CUSTOMSIZE - a user-defined size range with no
                     *           fixed dimensions (MaxSize 750/1350 x 32000);
                     *   0xFFFF  continuous stock (the driver sends 7F 00 then
                     *           FF FF when ContinuousMode is on).
                     * For both, the only real length is the raster that follows
                     * in ESC D, so the feed must not add a pitch. Without these
                     * branches 0x7F00 passed the raw-length test as 32512 dots
                     * and every label fed the full 4000-dot clamp. */
                    s_len_from_raster = 0;
                    if (v_be == 0x7F00u || v_be == 0xFFFFu) {
                        s_len_override = 0;
                        s_len_from_raster = 1;
                        break;
                    }
                    const paper_t *p = paper_find(v_be);
                    if (p) { s_paper = p; s_len_override = 0; }
                    /* 0 is not something the Windows driver ever sends; keep it
                     * as a defensive "clear any override". */
                    else if (v_be == 0) s_len_override = 0;
                    /* Unknown value: take it as a raw length. DYMO's CUPS driver
                     * sends the page height itself here (no +300 slack), and
                     * that is the host most likely to send a code we do not
                     * know, so it is used unmodified. */
                    else if (v_be >= 50 && v_be <= 32767) s_len_override = v_be;
                    break; }
                case 'n': s_label_index = v_le; break;
                }
            }
            s_state = S_CMD;
            break;

        case S_ARG4:
            s_arg4[s_arg2++] = c;
            if (s_arg2 >= 4) {
                if (s_arg1 == 's') {
                    s_job_id = (uint32_t)s_arg4[0] | ((uint32_t)s_arg4[1] << 8) |
                               ((uint32_t)s_arg4[2] << 16) | ((uint32_t)s_arg4[3] << 24);
                    s_job_active = 1;
                    s_label_index = 0;
                    /* A feed before this job's first ESC D must advance a full
                     * pitch, not the height of the previous job's last label. */
                    s_raster_dots = 0;
                }
                s_state = S_CMD;
            }
            break;

        case S_ESC_D:
            s_hdr[s_hcnt++] = c;                 /* BPP, Align, W(4), H(4) */
            if (s_hcnt >= 10) {
                uint8_t  bpp   = s_hdr[0] ? s_hdr[0] : 1;
                uint32_t w     = s_hdr[2] | ((uint32_t)s_hdr[3] << 8) |
                                 ((uint32_t)s_hdr[4] << 16) | ((uint32_t)s_hdr[5] << 24);
                uint32_t h     = s_hdr[6] | ((uint32_t)s_hdr[7] << 8) |
                                 ((uint32_t)s_hdr[8] << 16) | ((uint32_t)s_hdr[9] << 24);
                uint32_t bpl   = (h > 0xFFFFu) ? 0x10000u
                                               : ((h * bpp + 7u) / 8u);
                if (w > 0xFFFFu || bpl > 0xFFFFu) {
                    /* Header out of range: we cannot know where this raster
                     * block ends, so consuming it would desynchronise the
                     * stream. Drop the block and resync on the next ESC. */
                    s_lines_left = 0; s_line_rx = 0;
                    s_state = S_CMD;
                } else {
                    begin_raster((uint16_t)w, (uint16_t)h, bpp);
                }
            }
            break;

        case S_RASTER:
            /* Bytes past the head width are consumed but not printed, so an
             * over-wide line cannot shift the rest of the raster. */
            if (s_line_rx < s_use) s_line[s_line_rx] = c;
            s_line_rx++;
            if (s_line_rx >= s_bpl) {              /* line complete on the wire */
                emit_line();
                s_line_rx = 0;
                if (--s_lines_left == 0) {         /* label done */
                    label_printed();
                    s_state = S_CMD;
                }
            }
            break;

        case S_ESC_W:
            if (s_hcnt < 4) {                       /* len dir obj(2) */
                s_hdr[s_hcnt++] = c;
                if (s_hcnt == 4) {                 /* header complete */
                    s_w_payload = s_hdr[0];        /* payload bytes to skip */
                    if (s_w_payload > 250) s_w_payload = 250;
                    s_state = (s_w_payload ? S_SKIP : S_CMD);
                }
            }
            break;

        case S_ESC_F:
            /* ESC f 1 n - "Skip n Lines", documented in the LabelWriter 450
             * Series tech ref. The 550 manual drops it, but the command costs
             * three bytes to support and gives a host a genuine way to feed
             * without the GS backdoor. Any other sub-command is consumed. */
            s_hdr[s_hcnt++] = c;
            if (s_hcnt >= 2) {
                if (s_hdr[0] == 1) motor_step_lines(s_hdr[1]);
                s_state = S_CMD;
            }
            break;

        case S_SKIP:
            if (--s_w_payload == 0) s_state = S_CMD;   /* media-type bytes done */
            break;

        case S_AFTER_GS:
            if (c == 'C')       { s_hcnt = 0; s_state = S_GSC_HDR; }   /* set roll config */
            else if (c == 'D')  { s_diag_sub = 0; s_state = S_DIAG_SUB; } /* self-test */
            else                s_state = S_CMD;
            break;

        case S_GSC_HDR:
            s_hdr[s_hcnt++] = c;                   /* len, cntLo, cntHi */
            if (s_hcnt >= 3) {
                op_config_t *cfg = store_get_mut();
                cfg->label_count = (uint16_t)(s_hdr[1] | (s_hdr[2] << 8));
                s_sku_len = s_hdr[0]; s_sku_i = 0;
                if (s_sku_len == 0) { cfg->sku[0] = 0; store_save(); s_state = S_CMD; }
                else s_state = S_GSC_SKU;
            }
            break;

        case S_GSC_SKU: {
            op_config_t *cfg = store_get_mut();
            if (s_sku_i < OP_SKU_MAX - 1) cfg->sku[s_sku_i] = (char)c;
            s_sku_i++;
            if (s_sku_i >= s_sku_len) {
                uint8_t z = s_sku_len < OP_SKU_MAX-1 ? s_sku_len : OP_SKU_MAX-1;
                cfg->sku[z] = 0;
                store_save();
                s_state = S_CMD;
            }
            break;
        }

        case S_DIAG_SUB:
            s_diag_sub = c;
            s_diag_args[0] = s_diag_args[1] = s_diag_args[2] = 0;
            s_diag_argn = diag_argcount(c);
            s_diag_argi = 0;
            if (s_diag_argn) s_state = S_DIAG_ARG;
            else { diagnose(c); s_state = S_CMD; }
            break;

        case S_DIAG_ARG:
            s_diag_args[s_diag_argi++] = c;
            if (s_diag_argi >= s_diag_argn) {
                diagnose(s_diag_sub);
                s_state = S_CMD;
            }
            break;
        }
    }
}
