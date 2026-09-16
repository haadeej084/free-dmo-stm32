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

/* Base strobe time per half at density 8 (= ESC C 100 %), before the thermal
 * scale.
 *
 * ANALOGUE, not sourced for our head: ROHM publish no datasheet for the
 * KF3002-GK11C (bar marking 3C56-9638) - it is in neither the SF2023 nor the
 * SF2024 catalogue. 270 us is pinned to the KF3002 FAMILY's rated operating
 * point instead, all at Rave 1250 ohm and VH 24 V:
 *   GL50A: Po 0.43  W/dot, SLT 0.83 ms, TON 0.28  ms
 *   GD31A: Po 0.42  W/dot, SLT 0.82 ms, TON 0.308 ms
 *   GM50A: Po 0.434 W/dot, SLT 0.41 ms, TON 0.263 ms
 * 270 us sits inside that 263-308 us band and within 4 % of the GL50A's
 * 0.28 ms; at 0.43 W that is 0.116 mJ/dot against its rated 0.120 mJ/dot.
 *
 * Do NOT justify this from a density curve: the three siblings' Fig.4 disagree
 * by a factor 1.9 in energy-for-OD-1.0 at identical electrical spec, and none
 * of them is measured on DYMO stock. Only a bench sweep on a genuine roll can
 * put a real density scale under this (FIELDWORK measurement 2). */
#define HEAD_BASE_DWELL_US 270

/* Energy ceiling, not a time ceiling. The previous 2000 us clamp was not
 * derived from anything: at an ASSUMED Po of 0.43 W/dot it allows 0.86 mJ/dot,
 * four times ROHM's flat maximum rating of 0.215 mJ/dot, and 4.8x the family's
 * rated operating energy. ROHM's maximum-energy envelope is a function of
 * scanning line time (ANALOGUE, GL50A/GD31A Fig.5): ~0.155 mJ/dot at 0.65 ms,
 * 0.177 at 0.92 ms, 0.21 at 1.0 ms. At the 550's rated 0.92 ms per line,
 * 0.177 mJ/dot is 412 us.
 *
 * Nothing the genuine driver can ask for is clipped by this: DYMO's darkest
 * preset is ESC g = 112.5 %, which is 380 us at 25 C. ESC C 200 % on a cold
 * head would have been 675 us - above ROHM's flat maximum, and 1350 us of
 * strobe against a 920 us per-line budget, i.e. the printer would also have
 * run half speed. Raise this only with a measured Po for the fitted head. */
#define HEAD_MAX_DWELL_US  410
#define VH_SETTLE_US       2000     /* load-switch rise time before the first strobe */

/* Extra dwell for the second and later segments, compensating the rail sag
 * their predecessors caused. ANALOGUE: a shipping 24 V mechanism adds a fixed
 * 10 us to the second heat group "to compensate for the voltage drop during
 * the second group's heating". Our VH is an unregulated wall brick, so the sag
 * is at least as large - but the magnitude does not transfer (10 us was 4 % of
 * their pulse on their supply), and a wrong value darkens one half of every
 * label. Kept at 0 until the rail can be measured; the knob exists so the
 * mechanism is recorded rather than forgotten. */
#define HEAD_SEGMENT_SAG_US 0

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
        gpio_set(k_strobe[s], !MODEL_STB_ACTIVE_LEVEL);  /* idle = not firing */
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
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++)
        gpio_set(k_strobe[s], !MODEL_STB_ACTIVE_LEVEL);
    gpio_set(PIN_HEAD_LATCH, 1);   /* HOLD */
    gpio_set(PIN_HEAD_CLK, 0);
    gpio_set(PIN_HEAD_DI1, 0);
    gpio_set(PIN_HEAD_DI2, 0);
    gpio_set(PIN_HEAD_VH, !HEAD_VH_ON_LEVEL);
    s_vh_on = 0;
}

void head_set_density(uint8_t d) { if (d <= 16) s_density = d; }  /* 0 = heat off */

uint32_t head_dwell_us(uint8_t density, uint16_t thermal_scale)
{
    uint32_t dwell = (uint32_t)HEAD_BASE_DWELL_US * density / 8u;
    dwell = dwell * thermal_scale / 256u;
    if (dwell > HEAD_MAX_DWELL_US) dwell = HEAD_MAX_DWELL_US;
    return dwell;
}

uint32_t head_last_strobe_us(void) { return s_last_strobe_us; }

static void strobe(pin_t p, uint32_t us)
{
    gpio_set(p, MODEL_STB_ACTIVE_LEVEL);        /* fire the heat drivers */
    while (us > 1000) { delay_us(1000); us -= 1000; }
    delay_us(us);
    gpio_set(p, !MODEL_STB_ACTIVE_LEVEL);       /* off */
}

/* Dots set in one half of the line. A half with no dots is not worth a strobe:
 * firing it prints nothing, draws no heater current and only costs time. This
 * also gives the per-line dot count that any future current-aware scheme needs
 * (a shipping mechanism bin-packs its groups by exactly this count). */
static uint16_t dots_in(const uint8_t *bits, uint16_t from, uint16_t to, uint16_t nbytes)
{
    uint16_t n = 0;
    for (uint16_t i = from; i < to && i < nbytes; i++) {
        uint8_t v = bits[i];
        while (v) { n += (uint16_t)(v & 1u); v = (uint8_t)(v >> 1); }
    }
    return n;
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

    /* 2) latch: Low = THROUGH (datasheet timing chart). tw(LAT) min is 100 ns
     *    and we hold 1 us. */
    gpio_set(PIN_HEAD_LATCH, 0);
    delay_us(1);
    gpio_set(PIN_HEAD_LATCH, 1);
    /* t setup(STB) min 300 ns from the latch rising edge to a strobe, per the
     * KF3002 timing chart - 15 core cycles at 48 MHz. The dwell arithmetic
     * below happens to cover it today, but that is the compiler's choice, not
     * ours; make it explicit. */
    for (int i = 0; i < 20; i++) __asm volatile("nop");

    /* 3) dwell = base * density/8 * thermal scale/256, capped by energy */
    uint32_t dwell = head_dwell_us(s_density, thermal_dwell_scale());

    /* 4) fire the halves sequentially. The HEAD does not require this: both
     *    published KF3002 siblings rate "maximum number of dots energized
     *    simultaneously" at the full dot count. It is a SUPPLY constraint -
     *    DYMO size the 550's brick for "an average of 37 % of the total dots
     *    per line at full speed" (450 TRM p.7) on a 42 W supply, while a full
     *    line of KF3002-class dots would draw hundreds of watts from the bulk
     *    capacitor. A half with no dots in it is skipped entirely. */
    uint32_t fired = 0;
    const uint16_t half = (uint16_t)((HEAD_DI1_DOTS + 7) / 8);
    vh_enable();
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++) {
        uint16_t from = (uint16_t)(s * half), to = (uint16_t)(from + half);
        if (HEAD_STROBE_SEGMENTS == 2 && dots_in(bits, from, to, nbytes) == 0)
            continue;                        /* nothing to print in this half */
        uint32_t us = dwell + (uint32_t)(s ? HEAD_SEGMENT_SAG_US : 0);
        strobe(k_strobe[s], us);
        fired += us;
    }

    s_last_strobe_us = fired;
}
