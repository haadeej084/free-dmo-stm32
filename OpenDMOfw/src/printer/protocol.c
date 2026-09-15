/* OpenDMO-FW - host wire-protocol parser (byte-driven, resumable state machine).
 *
 * Implements the LabelWriter 550/5XL host protocol exactly as published in the
 * official "LabelWriter 550 Series Printers Technical Reference Manual" (the
 * "tech ref") and as confirmed by the decompiled stock PC-side software
 * (DYMO Connect / port monitor):
 *
 *   ESC s <JobID u32>    start of print job (job ID is echoed in status)
 *   ESC L <len u16>      set maximum label length (dots), used for feed math
 *   ESC h / ESC i        text / graphics output mode
 *   ESC T <speed>        0x10 normal, 0x20 high speed
 *   ESC n <idx u16>      set label index (echoed in status)
 *   ESC D BPP Align W H  start of label print data: W = number of lines,
 *                         H = number of dots; then W * roundup(H*BPP/8) bytes
 *   ESC G                feed to print head (short form feed, between labels)
 *   ESC E                feed to tear position (long form feed)
 *   ESC Q                end of print job (releases the lock)
 *   ESC A <lock>         request status -> 32-byte struct on bulk-IN
 *   ESC C <duty>         print density, 0-200 % (0 = printing disabled)
 *   ESC e                reset density to default (100 %)
 *   ESC U                get SKU info -> 63-byte consumable record
 *   ESC V                get version -> 34-byte reply
 *   ESC *                restore factory settings (config back to defaults)
 *   ESC o <count u16>    set label count
 *   ESC q <tray>         select output tray (accepted, ignored)
 *   ESC W len dir objid  control-command framing; payload consumed, ignored
 *   ESC M <8 bytes>      media-type descriptor (mtDefault = 8 zero bytes);
 *                         the driver always sends it, so consume + ignore
 *   ESC @                restart print engine -> full pipeline reset here
 *
 * Backdoor commands (never sent by the stock host, kept for configuration and
 * driver-less bring-up via tools/opsend.py):
 *   GS C len lo hi sku.. 1D 43 .. set roll config (SKU + count) in EEPROM
 *   ESC d n                1B 64 xx  feed n dot lines
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
    S_ARG2,         /* two argument bytes, u16 LE (s_arg1 = which command) */
    S_ARG4,         /* four argument bytes, u32 LE (s_arg1 = which command) */
    S_ESC_D,        /* ESC D: BPP, Align, W(4), H(4) then raster */
    S_RASTER,       /* consuming raster lines for one label */
    S_ESC_W,        /* ESC W: 5 header bytes then len payload bytes */
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
static uint8_t  s_hdr[10];            /* ESC D header (10 B) / ESC W header (4 B) */
static uint8_t  s_hcnt;
static uint16_t s_bpl;               /* raster bytes per line */
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
static uint16_t s_raster_dots;       /* height (dots) of the current raster block */
static uint16_t s_last_lines;        /* printed height (dots) of the last label */

/* Feed math: die-cut rolls have a small physical gap between labels. */
#define LABEL_GAP_DOTS   20          /* ~1.7 mm at 300 dpi */
#define TEAR_EXTRA_DOTS  15          /* tear bar sits past the next print position */

void protocol_init(void)
{
    s_head = s_tail = 0; s_rx_paused = 0;
    s_state = S_CMD;
    s_job_active = 0; s_label_index = 0; s_job_id = 0;
    s_density_pct = 100;
    s_paper = paper_lookup(PAPER_DEFAULT_CODE);
    s_len_override = 0; s_raster_dots = 0; s_last_lines = 0;
}

void protocol_reset(void)
{
    s_head = s_tail = 0;
    s_state = S_CMD; s_hcnt = 0; s_lines_left = 0; s_line_rx = 0;
    s_job_active = 0; s_label_index = 0;
    s_paper = paper_lookup(PAPER_DEFAULT_CODE);
    s_len_override = 0; s_raster_dots = 0; s_last_lines = 0;
    head_reset();
}

void protocol_feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (ring_free() == 0) break;
        s_ring[s_head] = data[i];
        s_head = (s_head + 1) & (RING_SZ - 1);
    }
    if (ring_free() < EP_MAXPKT) s_rx_paused = 1;
    else                          usb_ep_rx_ready(EP_DATA);
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

static void begin_raster(uint16_t lines, uint16_t dots, uint8_t bpp)
{
    s_raster_dots = dots;
    uint16_t bpl = (uint16_t)(((uint32_t)dots * bpp + 7u) / 8u);
    /* The driver caps the printable width at the head width (GPD
     * MaxPrintableWidth: 1248 for 5XL, 672 for 550), so bpl never exceeds
     * HEAD_BYTES; the clamp is defensive only. A narrower raster is centered
     * on the head (matches PrintableOrigin geometry). */
    if (bpl > HEAD_BYTES) bpl = HEAD_BYTES;
    s_bpl = bpl; s_lines_left = lines; s_line_rx = 0;
    s_xoff = (bpl < HEAD_BYTES) ? (HEAD_BYTES - bpl) / 2 : 0;
    if (bpl == 0 || lines == 0) { s_state = S_CMD; return; }
    s_job_active = 1;
    s_state = S_RASTER;
}

/* Advance the paper to the next label position. The stock pitch comes from the
 * ESC L paper code (height in the GPD table) or a raw dot length; plus the
 * physical inter-label gap. ASSUMPTION (verify on hardware): the die-cut gap
 * and tear-bar offset are fixed dot counts, not read from the roll. */
static void feed_next_label(int to_tear)
{
    uint16_t pitch = s_len_override ? s_len_override
                    : (s_paper ? s_paper->height_dots : s_raster_dots);
    uint16_t dots = LABEL_GAP_DOTS;
    if (pitch > s_raster_dots) dots += (uint16_t)(pitch - s_raster_dots);
    if (to_tear) dots += TEAR_EXTRA_DOTS;
    motor_step_lines(dots);
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

/* ---- ESC U: 63-byte consumable record (tech ref p.16-19) -------- */
static uint16_t crc16_ccitt(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void send_sku_record(void)
{
    const op_config_t *c = store_get();
    const paper_t *p = s_paper ? s_paper : paper_lookup(PAPER_DEFAULT_CODE);
    uint8_t r[63];
    for (int i = 0; i < 63; i++) r[i] = 0;

    /* mm values from the default paper's dot dimensions at MODEL_DPI */
    uint16_t w_mm = (uint16_t)(p->width_dots  * 25u / MODEL_DPI);
    uint16_t h_mm = (uint16_t)(p->height_dots * 25u / MODEL_DPI);

    r[0] = 0xB6; r[1] = 0xCA;                    /* magic 0xCAB6 LE */
    r[2] = 0;                                    /* version */
    uint8_t slen = 0;
    while (slen < 12 && c->sku[slen]) slen++;
    r[3] = slen;                                 /* length */
    /* bytes 6..7 undocumented: reserved 0 */
    for (int i = 0; i < 12; i++)                 /* SKU, NUL-padded */
        if (i < OP_SKU_MAX && c->sku[i]) r[8 + i] = (uint8_t)c->sku[i];
    r[20] = 0x00;                                /* brand: DYMO */
    r[21] = 0xFF;                                /* region: global */
    r[22] = 0x03;                                /* material: paper */
    r[23] = 0x01;                                /* label type: die */
    r[24] = 0x01;                                /* label color: white */
    r[25] = 0x00;                                /* content color: black */
    r[26] = 0x00;                                /* marker type 0 */
    uint16_t pitch_mm = (uint16_t)(h_mm + 3);    /* label length + gap */
    r[28] = (uint8_t)(pitch_mm & 0xFF); r[29] = (uint8_t)(pitch_mm >> 8);
    r[30] = 2; r[31] = 0;                        /* marker1 width 2 mm */
    r[32] = 2; r[33] = 0;                        /* marker1 to label start 2 mm */
    /* marker2 unused (type 0) */
    r[38] = 1; r[39] = 0;                        /* vertical offset 1 mm */
    r[40] = (uint8_t)(h_mm & 0xFF); r[41] = (uint8_t)(h_mm >> 8);   /* label length mm */
    r[42] = (uint8_t)(w_mm & 0xFF); r[43] = (uint8_t)(w_mm >> 8);   /* label width mm */
    r[44] = 2; r[45] = 0;                        /* printable area h-offset 2 mm */
    r[46] = 2; r[47] = 0;                        /* printable area v-offset 2 mm */
    r[48] = (uint16_t)(HEAD_DOTS * 25u / MODEL_DPI) & 0xFF;         /* liner width mm */
    r[49] = ((uint16_t)(HEAD_DOTS * 25u / MODEL_DPI) >> 8) & 0xFF;
    r[50] = (uint8_t)(c->label_count & 0xFF);    /* total label count */
    r[51] = (uint8_t)(c->label_count >> 8);
    uint32_t total_mm = (uint32_t)pitch_mm * c->label_count;
    if (total_mm > 0xFFFF) total_mm = 0xFFFF;
    r[52] = (uint8_t)(total_mm & 0xFF); r[53] = (uint8_t)(total_mm >> 8);
    /* counter margin = 0 */
    r[56] = 0x00;                                /* counter strategy: count up from 0 */
    r[60] = 15; r[61] = 26;                      /* production date DDYY (15-26) */
    r[62] = 0x12;                                /* production time HHMM (low byte) */

    uint16_t crc = crc16_ccitt(&r[8], 55);       /* ASSUMPTION: CRC over payload */
    r[4] = (uint8_t)(crc & 0xFF); r[5] = (uint8_t)(crc >> 8);
    usbp_send_reply(r, sizeof(r));
}

/* ---- ESC V: 34-byte version reply (tech ref p.20) --------------- */
static void send_version(void)
{
    uint8_t r[34];
    for (int i = 0; i < 34; i++) r[i] = 0;
    /* 16-char fields, zero-padded (tech ref p.20). Per-model values from
     * model.h; the exact strings are an assumption (DECISIONS D12). */
    uint8_t hw[16] = MODEL_HW_VERSION;
    uint8_t fw[16] = MODEL_FW_VERSION;
    for (int i = 0; i < 16; i++) { r[i]     = hw[i];
                                   r[16 + i] = fw[i]; }
    r[32] = (uint8_t)(MODEL_PID & 0xFF);         /* USB PID LE */
    r[33] = (uint8_t)(MODEL_PID >> 8);
    usbp_send_reply(r, sizeof(r));
}

/* ---- GS D: self-test / diagnostic backdoor (never sent by the stock host) --
 * Subcommands (GS D <sub> [arg]):
 *   0x01 <n>  strobe the head for n all-on lines at max density (verify head drive)
 *   0x02 <n>  step the feed motor n dot-lines (verify feed)
 *   0x03      EEPROM self-test -> reply match status
 *   0x04      diagnostic snapshot -> thermistor, GPIOs, config, model
 * Every reply starts with 'D' so the host can tell it apart from a status struct.
 * These exist so the physical layer (head/motor/EEPROM/thermistor) can be verified
 * on hardware without a full print job - see DECISIONS.md D8 / PROGRESS.md R2-R6. */
static void diagnose(uint8_t sub, uint8_t arg)
{
    const op_config_t *c = store_get();
    uint8_t r[24];
    for (int i = 0; i < 24; i++) r[i] = 0;
    r[0] = 'D';                       /* marker: distinguishes from a status struct */
    r[1] = sub;                       /* echo the subcommand (uniform across cases) */

    switch (sub) {
    case 0x01: {                              /* strobe head n all-on lines */
        uint8_t line[HEAD_BYTES];
        for (int i = 0; i < HEAD_BYTES; i++) line[i] = 0xFF;
        head_set_density(16);                 /* max dwell so it is unmistakable */
        for (uint8_t i = 0; i < arg; i++) { head_print_line(line, HEAD_BYTES); wdt_kick(); }
        set_density(s_density_pct);           /* restore the configured base level */
        r[2] = arg; r[3] = thermal_ok() ? 1 : 0;
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
    for (; d[i] && i < OP_SKU_MAX - 1; i++) cfg->sku[i] = d[i];
    cfg->sku[i] = 0;
    cfg->label_count = MODEL_DEFAULT_COUNT;
    cfg->density = 8;
    store_save();
    set_density(100);
}

static void emit_line(void)
{
    /* Center the raster on the head (narrow label, wide head); a raster wider
     * than the head is left-aligned and clipped at the buffer edge. */
    uint8_t tmp[HEAD_BYTES];
    for (uint16_t i = 0; i < HEAD_BYTES; i++)
        tmp[i] = (i >= s_xoff && (i - s_xoff) < s_bpl) ? s_line[i - s_xoff] : 0;
    for (uint16_t i = 0; i < HEAD_BYTES; i++) s_line[i] = tmp[i];
    wdt_kick();
    /* Bounded cool-down wait (max ~1 s), keep feeding the watchdog. After that
     * print anyway: the dwell is already thermally reduced, so this never hangs. */
    for (int i = 0; i < 100 && !thermal_ok(); i++) { delay_ms(10); wdt_kick(); }
    head_print_line(s_line, HEAD_BYTES);
    motor_step_lines(1);
    wdt_kick();
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
            case 's': s_arg1 = 's'; s_arg2 = 0; s_state = S_ARG4; break; /* job + ID */
            case 'L': s_arg1 = 'L'; s_arg2 = 0; s_state = S_ARG2; break; /* paper code */
            case 'n': s_arg1 = 'n'; s_arg2 = 0; s_state = S_ARG2; break; /* label index */
            case 'o': s_arg1 = 'o'; s_arg2 = 0; s_state = S_ARG2; break; /* set count   */
            case 'A': s_arg1 = 'A'; s_state = S_ARG1; break;    /* status + lock */
            case 'C': s_arg1 = 'C'; s_state = S_ARG1; break;    /* density       */
            case 'T': s_arg1 = 'T'; s_state = S_ARG1; break;    /* speed         */
            case 'q': s_arg1 = 'q'; s_state = S_ARG1; break;    /* tray          */
            case 'd': s_arg1 = 'd'; s_state = S_ARG1; break;    /* feed (backdoor) */
            case 'D': s_hcnt = 0; s_state = S_ESC_D; break;     /* raster header */
            case 'W': s_hcnt = 0; s_state = S_ESC_W; break;     /* control cmd   */
            case 'M': s_w_payload = 8; s_state = S_SKIP; break; /* media type +8B */
            case 'h': s_state = S_CMD; break;                   /* text mode     */
            case 'i': s_state = S_CMD; break;                   /* graphics mode */
            case 'G': feed_next_label(0); s_state = S_CMD; break;  /* short feed  */
            case 'E': feed_next_label(1); s_state = S_CMD; break;  /* tear feed   */
            case 'Q':                                 /* end of job / unlock     */
                s_job_active = 0; s_label_index = 0;
                s_state = S_CMD; break;
            case 'e': set_density(100); s_state = S_CMD; break;  /* density reset */
            case 'U': send_sku_record(); s_state = S_CMD; break;
            case 'V': send_version();    s_state = S_CMD; break;
            case '*': factory_reset();   s_state = S_CMD; break;
            case '@': protocol_reset();  s_state = S_CMD; return; /* pipeline reset */
            default:  s_arg1 = '?'; s_state = S_ARG1; break;     /* unknown: skip 1 arg */
            }
            break;

        case S_ARG1:
            switch (s_arg1) {
            case 'A': send_status(); break;
            case 'C': set_density(c); break;
            case 'd': motor_step_lines(c); break;
            /* 'T' speed, 'q' tray, '?': accept and ignore */
            }
            s_state = S_CMD;
            break;

        case S_ARG2:
            if (s_arg2 == 0) { s_arg2++; s_arg4[0] = c; break; }
            s_arg4[1] = c;
            {
                uint16_t v = (uint16_t)(s_arg4[0] | (c << 8));
                switch (s_arg1) {
                case 'L': {
                    /* Paper code from the driver GPD (e.g. "<1B>L<0867>"). Known
                     * codes set the stock; an unknown value in a plausible dot
                     * range is treated as a raw max label length. */
                    const paper_t *p = paper_find(v);
                    if (p) { s_paper = p; s_len_override = 0; }
                    else if (v >= 50 && v <= 32767) s_len_override = v;
                    break; }
                case 'n': s_label_index = v; break;
                case 'o': store_get_mut()->label_count = v; store_save(); break;
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
                if (w > 0xFFFF) w = 0xFFFF;
                if (h > 0x7FFF) h = 0x7FFF;      /* keep bpl in u16 range */
                begin_raster((uint16_t)w, (uint16_t)h, bpp);
            }
            break;

        case S_RASTER:
            if (s_line_rx < HEAD_BYTES) s_line[s_line_rx] = c;   /* clip overflow */
            s_line_rx++;
            if (s_line_rx >= s_bpl) {              /* line complete */
                emit_line();
                s_line_rx = 0;
                if (--s_lines_left == 0) {         /* label done */
                    s_last_lines = s_raster_dots;  /* printed height, for feed math */
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
            if (c == 0x01 || c == 0x02) s_state = S_DIAG_ARG;    /* needs a count */
            else { diagnose(c, 0); s_state = S_CMD; }
            break;

        case S_DIAG_ARG:
            diagnose(s_diag_sub, c);
            s_state = S_CMD;
            break;
        }
    }
}
