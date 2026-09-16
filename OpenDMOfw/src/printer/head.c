/* OpenDMOfw - thermal head driver (ROHM KF3002-family shift-register head).
 *
 * The head module contains its own shift registers, latch and heat drivers
 * (ROHM KF3002-GL50A datasheet, equivalent circuit + timing chart). Per dot
 * line the host:
 *   1) shifts HEAD_DOTS bits in on CLK, one bit per half on DI1/DI2 in
 *      parallel (dot i of half 1 and dot i of half 2 share one clock);
 *   2) pulses LAT low (Low = THROUGH) to load the registers into the drivers;
 *   3) fires STB1 then STB2, each for a dwell that scales with density and
 *      head temperature. Firing the two halves sequentially splits the peak
 *      current in half (required for the wide 1248-dot head).
 *
 * The mechanism is described in DYMO's own LabelWriter 450 Series Technical
 * Reference (p.7): "To print a line, the control electronics load the desired
 * data into a serial shift register... A '1' in a register causes the
 * corresponding dot to be printed", and the elements are "0.085 mm square and
 * are spaced at 300 per inch" - i.e. exactly the 1/300 inch pitch the feed
 * math in motor.c assumes.
 *
 * PER-LINE TIME BUDGET (sourced, use it when calibrating). DYMO rates the 550
 * at 62 labels/min and the 5XL at 53 labels/min on a 4-line address label,
 * which is 89 mm = 1050 dot lines. That is 0.92 ms per line for the 550 and
 * 1.08 ms for the 5XL, including the inter-label feed. Everything this
 * function does has to fit in that if the printer is to run at genuine speed:
 * shift + latch + 2 x dwell, with the motor step overlapped (see emit_line).
 *
 * Sourced from the head datasheet: signal set, LAT polarity (Low=THROUGH), 2x
 * shift-register halves driven in parallel (DI1||DI2), STB polarity
 * (Low = fires the heat driver), built-in NTC thermistor (30 kOhm B=3950 on TM).
 * ASSUMPTIONS (verify on hardware, see PINMAP.md / DECISIONS D16): which MCU
 * pin D.mo wired each signal to, the DI1/DI2 split (model.h), and the base dwell.
 *
 * STROBE PROPAGATION. The head's driver ICs add a strobe-to-driver-output delay
 * on each edge: max 10 us in ROHM KF3002-GL50A Fig.2, max 3.5 us (TpLH/TpHL)
 * for the SHEC G56 class. Both edges are specified together, so the heat pulse
 * is expected to shift rather than stretch; the residual edge skew is
 * unspecified and matters only at the lowest densities. Do not add a minimum
 * dwell cut-off for it: density 1-6 % must still print (tech ref p.16).
 */
#include "head.h"
#include "thermal.h"
#include "../config/store.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

/* Base strobe time per half at density 8, before the thermal scale. 270 us
 * puts a dot at roughly 0.116 mJ at the KF3002 family's ~0.43 W/dot, which is
 * about the knee of the published optical-density curve and sits inside the
 * family's typical TON band. The old 400 us started above saturation, so the
 * whole 1..16 density range clustered at maximum black with nothing to trade.
 * Still provisional until measured on a bench - see FIELDWORK measurement 2
 * for the per-line time budget this has to fit inside. */
#define HEAD_BASE_DWELL_US 270
#define VH_SETTLE_US       2000     /* load-switch rise time before the first strobe */

static uint8_t  s_density = 8;
static int      s_vh_on;
static uint32_t s_vh_ms;
static uint32_t s_last_strobe_us;

/* The two data inputs must cover the head exactly. */
typedef char head_di_split_covers_head[(HEAD_DI1_DOTS + HEAD_DI2_DOTS == HEAD_DOTS) ? 1 : -1];
typedef char head_di_split_nonzero[(HEAD_DI1_DOTS > 0 && HEAD_DI2_DOTS > 0) ? 1 : -1];

/* The heat lines, in segment order. head_print_line uses the first
 * HEAD_STROBE_SEGMENTS of them (from model.h). */
static const pin_t k_strobe[4] = {
    PIN_HEAD_STROBE, PIN_HEAD_STROBE2, PIN_HEAD_STROBE3, PIN_HEAD_STROBE4
};

void head_init(void)
{
    /* All head signals are plain GPIO outputs, idle state:
     * CLK/DI low, LAT high (HOLD), strobes high (off - STB is active-low). */
#if HEAD_SHIFT_SAME_PORT
    /* pins.h promises one port for the shift lines; a remap that breaks that
     * would make the fast shift loop drive the wrong pins. Stop here, before
     * any head output is configured - the watchdog then resets visibly
     * instead of printing garbage with the heat rail live. */
    if (PIN_HEAD_DI1.port != PIN_HEAD_CLK.port || PIN_HEAD_DI2.port != PIN_HEAD_CLK.port)
        for (;;) {}
#endif
    gpio_mode(PIN_HEAD_CLK,   GPIO_OUT);  gpio_set(PIN_HEAD_CLK, 0);
    gpio_mode(PIN_HEAD_DI1,   GPIO_OUT);  gpio_set(PIN_HEAD_DI1, 0);
    gpio_mode(PIN_HEAD_DI2,   GPIO_OUT);  gpio_set(PIN_HEAD_DI2, 0);
    gpio_mode(PIN_HEAD_LATCH, GPIO_OUT);  gpio_set(PIN_HEAD_LATCH, 1);
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++) {
        gpio_set(k_strobe[s], 1);           /* active-low: idle high = off */
        gpio_mode(k_strobe[s], GPIO_OUT);   /* level before mode, as for VH */
    }
    /* The 24 V heat rail starts OFF and is switched on only around an actual
     * print (head_idle_tick drops it again). The genuine printer does the same
     * kind of thing - the 550 TRM describes shutting down unused peripherals
     * after 30 s idle - and on a board where this pin is still an assumption,
     * "off unless printing" is the difference between a wrong guess costing
     * nothing and a wrong guess cooking the head. */
    /* Level FIRST, then mode: gpio_set writes ODR, and switching the pin to an
     * output before ODR holds the off level would drive whatever ODR happened
     * to contain for one instruction. On the VH gate that instant is 24 V.
     * The board should also pull this net to the OFF level externally, so the
     * rail is dead whenever the MCU is unpowered or in reset - verify that
     * before the first 24 V test (FIELDWORK measurement 5). */
    gpio_set(PIN_HEAD_VH, !HEAD_VH_ON_LEVEL);
    gpio_mode(PIN_HEAD_VH, GPIO_OUT);
    s_vh_on = 0;
}

int  head_vh_is_on(void) { return s_vh_on; }

void head_vh_off(void)
{
    gpio_set(PIN_HEAD_VH, !HEAD_VH_ON_LEVEL);
    s_vh_on = 0;
}

static void vh_enable(void)
{
    /* Hard interlock. While OP_FLAG_VH_INHIBIT is set there is no command
     * sequence at all that puts 24 V on the head - the exploration image builds
     * with it set, so poking at unknown pins cannot end in a dead head. */
    if (store_get()->flags & OP_FLAG_VH_INHIBIT) return;
    if (!s_vh_on) {
        gpio_set(PIN_HEAD_VH, HEAD_VH_ON_LEVEL);
        s_vh_on = 1;
        delay_us(VH_SETTLE_US);
    }
    s_vh_ms = millis();
}

void head_idle_tick(uint32_t idle_ms)
{
    if (!s_vh_on) return;
    if ((millis() - s_vh_ms) < idle_ms) return;
    gpio_set(PIN_HEAD_VH, !HEAD_VH_ON_LEVEL);
    s_vh_on = 0;
}

void head_reset(void)
{
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++) gpio_set(k_strobe[s], 1);
    gpio_set(PIN_HEAD_LATCH, 1);   /* HOLD */
    gpio_set(PIN_HEAD_CLK, 0);
    gpio_set(PIN_HEAD_DI1, 0);
    gpio_set(PIN_HEAD_DI2, 0);
    gpio_set(PIN_HEAD_VH, !HEAD_VH_ON_LEVEL);
    s_vh_on = 0;
}

void head_set_density(uint8_t d) { if (d <= 16) s_density = d; }  /* 0 = heat off */

uint32_t head_last_strobe_us(void) { return s_last_strobe_us; }

static void strobe(pin_t p, uint32_t us)
{
    gpio_set(p, 0);                 /* active-low: Low = fire the heat driver */
    while (us > 1000) { delay_us(1000); us -= 1000; }
    delay_us(us);
    gpio_set(p, 1);                 /* High = off */
}

void head_print_line(const uint8_t *bits, uint16_t nbytes)
{
    /* ESC C duty 0 = printing disabled: skip heat, caller still feeds. */
    if (s_density == 0) {
        s_last_strobe_us = 0;
        return;
    }

    /* 1) Shift in the dot line: one CLK per dot pair (half 1 on DI1, half 2 on
     *    DI2). Written straight to BSRR rather than through gpio_set(): at
     *    1248 dots this loop runs 624 times, and four function calls per
     *    iteration cost about a millisecond per line - the entire per-line
     *    budget above. Data is placed before the rising edge (setup), and one
     *    NOP widens the CLK high phase past the head's ~100 ns class minimum.
     *    Dot 0 = MSB of byte 0 (tech reference); dots beyond the sent data
     *    print white. */
    GPIO_Type *pdi1 = PIN_HEAD_DI1.port;
    GPIO_Type *pdi2 = PIN_HEAD_DI2.port;
    GPIO_Type *pclk = PIN_HEAD_CLK.port;
    const uint32_t d1 = 1u << PIN_HEAD_DI1.pin;
    const uint32_t d2 = 1u << PIN_HEAD_DI2.pin;
    const uint32_t ck = 1u << PIN_HEAD_CLK.pin;
#if (HEAD_DI1_DOTS == HEAD_DI2_DOTS) && (HEAD_DI1_DOTS % 8 == 0) && HEAD_SHIFT_SAME_PORT
    /* Fastest path: equal byte-aligned halves, CLK/DI1/DI2 on one port.
     * Two BSRR writes per dot: (a) both data bits plus CLK low in one atomic
     * write, (b) CLK high. Data therefore changes on the falling edge and is
     * stable for the whole low phase before the rising edge that samples it;
     * the high phase lasts until the next write (a) - several instructions,
     * far above the head's ~100 ns class minimum. Measured in Renode on the
     * 1248-dot build this roughly halves the instructions per line compared
     * with the four-write loop below (DECISIONS D24). */
    (void)pdi1; (void)pdi2;
    const uint16_t hb = (uint16_t)(HEAD_DI1_DOTS / 8);   /* bytes per half */
    /* The four possible "data + CLK low" words, indexed by (DI1 << 1) | DI2,
     * computed once per line so the inner loop does no constant building. */
    uint32_t word[4];
    for (unsigned i = 0; i < 4; i++) {
        uint32_t set = ((i & 2u) ? d1 : 0u) | ((i & 1u) ? d2 : 0u);
        word[i] = set | ((set ^ (d1 | d2)) << 16) | (ck << 16);
    }
    volatile uint32_t *bsrr = &pclk->BSRR;

    for (uint16_t b = 0; b < hb; b++) {
        /* DI1 byte in bits 15..8, DI2 byte in 7..0: after k left shifts bit 15
         * is DI1 bit 7-k and bit 7 is DI2 bit 7-k; DI2's bits only reach bit
         * 15 after eight shifts, when the byte is done. */
        uint32_t w = (uint32_t)(((b < nbytes) ? bits[b] : 0u) << 8)
                   | (((uint16_t)(hb + b) < nbytes) ? bits[hb + b] : 0u);
        for (unsigned k = 0; k < 8; k++, w <<= 1) {
            *bsrr = word[((w >> 14) & 2u) | ((w >> 7) & 1u)];
            *bsrr = ck;
        }
    }
    *bsrr = ck << 16;                                /* park CLK low */
#elif (HEAD_DI1_DOTS == HEAD_DI2_DOTS) && (HEAD_DI1_DOTS % 8 == 0)
    /* Fast path: equal, byte-aligned halves, pins on different ports. */
    const uint16_t hb = (uint16_t)(HEAD_DI1_DOTS / 8);   /* bytes per half */

    for (uint16_t b = 0; b < hb; b++) {
        uint8_t v1 = (b < nbytes) ? bits[b] : 0u;
        uint8_t v2 = ((uint16_t)(hb + b) < nbytes) ? bits[hb + b] : 0u;
        for (uint8_t m = 0x80u; m; m >>= 1) {
            pdi1->BSRR = (v1 & m) ? d1 : (d1 << 16);
            pdi2->BSRR = (v2 & m) ? d2 : (d2 << 16);
            pclk->BSRR = ck;
            __asm volatile("nop");
            pclk->BSRR = ck << 16;
        }
    }
#else
    /* Unequal halves: clock max(DI1, DI2) times. The shorter register keeps
     * only its LAST n bits, so it is fed leading zeros first. */
    const uint16_t n1 = HEAD_DI1_DOTS, n2 = HEAD_DI2_DOTS;
    const uint16_t nc = n1 > n2 ? n1 : n2;
    const uint32_t have = (uint32_t)nbytes * 8u;
    for (uint16_t i = 0; i < nc; i++) {
        int o1 = (int)i - (int)(nc - n1);        /* dot index within half 1 */
        int o2 = (int)i - (int)(nc - n2);        /* dot index within half 2 */
        uint32_t k1 = (uint32_t)o1, k2 = (uint32_t)n1 + (uint32_t)o2;
        int b1 = o1 >= 0 && k1 < have && (bits[k1 >> 3] & (0x80u >> (k1 & 7u)));
        int b2 = o2 >= 0 && k2 < have && (bits[k2 >> 3] & (0x80u >> (k2 & 7u)));
        pdi1->BSRR = b1 ? d1 : (d1 << 16);
        pdi2->BSRR = b2 ? d2 : (d2 << 16);
        pclk->BSRR = ck;
        __asm volatile("nop");
        pclk->BSRR = ck << 16;
    }
#endif

    /* 2) latch: Low = THROUGH (datasheet timing chart) */
    gpio_set(PIN_HEAD_LATCH, 0);
    delay_us(1);
    gpio_set(PIN_HEAD_LATCH, 1);

    /* 3) dwell = base * density/8 * thermal scale/256 */
    uint32_t dwell = (uint32_t)HEAD_BASE_DWELL_US * s_density / 8u;
    dwell = dwell * thermal_dwell_scale() / 256u;
    if (dwell > 2000) dwell = 2000; /* hard upper limit per line */

    /* 4) fire the halves sequentially (peak current / number of segments).
     *    DYMO sizes the supply for "an average of 37% of the total dots per
     *    line at full speed" (450 TRM p.7), so the peak of an all-black line
     *    is well above the rail's continuous rating - splitting it is not
     *    optional. */
    vh_enable();
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++)
        strobe(k_strobe[s], dwell);

    s_last_strobe_us = dwell * (uint32_t)HEAD_STROBE_SEGMENTS;
}
