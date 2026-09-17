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
 * That envelope RISES WITH LINE TIME - a faster printer has a LOWER ceiling,
 * which is the one thing about this rating that must never be forgotten. The
 * static assert below ties the constant to MODEL_LINE_PERIOD_US so the two
 * cannot drift apart silently.
 *
 * Nothing the genuine driver can ask for is clipped by this: DYMO's darkest
 * preset is ESC g = 112.5 %, which is 380 us at 25 C. ESC C 200 % on a cold
 * head would have been 675 us - above ROHM's flat maximum, and 1350 us of
 * strobe against a 920 us per-line budget, i.e. the printer would also have
 * run half speed. Raise this only with a measured Po for the fitted head. */
#define HEAD_MAX_DWELL_US  410
#define VH_SETTLE_US       2000     /* load-switch rise time before the first strobe */

/* HEAD_MAX_DWELL_US is 0.177 mJ/dot read off ROHM's maximum-energy envelope AT
 * A 0.92 ms LINE TIME. The envelope rises with line time, so a shorter line
 * means a lower ceiling, and this constant silently becomes wrong. Fail the
 * build instead: anyone moving the line period has to come back here. */
_Static_assert(MODEL_LINE_PERIOD_US >= 700 && MODEL_LINE_PERIOD_US <= 1100,
               "HEAD_MAX_DWELL_US was derived from ROHM's maximum-energy "
               "envelope at a 0.92 ms line; re-derive it for this line period");

/* Rail-sag compensation, in microseconds added at FULL coverage and scaled
 * down in proportion to the dots actually energised.
 *
 * The shape of this term is no longer a guess. The genuine LabelWriter 450
 * firmware - which the owner reports drives this very mechanism correctly when
 * its mainboard is fitted to a 550 - computes its strobe width as
 *
 *     w = 400 + (1.5*P - 250) + 3*B + 3*(T>>2)     ticks of 375 ns
 *
 * and `3*B` is exactly this: B is the number of image data bytes in the line,
 * so the term is worth up to 252 ticks = 94.5 us across the full 672-dot line
 * and nothing at all on a blank one (DECISIONS D30). Our own earlier knob was
 * a FLAT constant added to the second segment only, which had the structure
 * wrong in two ways: the sag scales with printed content, and it is the
 * content of the segment being fired that matters, not its ordinal.
 *
 * Two deliberate differences from the genuine model:
 *   - halved, because we fire half a line per strobe where the 450 fires all
 *     672 dots in one;
 *   - keyed to the true energised-dot count from dots_in() rather than the
 *     450's byte-span proxy, which counts a byte with one dot set the same as
 *     a solid one.
 *
 * MAGNITUDE STILL 0, deliberately. 47 us is what the genuine firmware implies,
 * but that number was calibrated against the 450's own 60 W supply; the 550
 * ships a 42 W brick (DSA-42PFC-24, 24 V / 1.75 A), so our rail sags MORE while
 * our energy headroom is SMALLER. Copying the constant across that difference
 * is the exact mistake D30 refused to make with the base dwell. The mechanism
 * is now correct and inside the ceiling; the magnitude waits for a scope on the
 * rail during a solid-black line (FIELDWORK measurement 2). */
#ifndef HEAD_SAG_FULL_US                 /* the thermal test builds it armed */
#define HEAD_SAG_FULL_US 0
#endif

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

/* The same dwell plus rail-sag compensation for the coverage of THIS strobe.
 *
 * The clamp is re-applied after the addition, and that is the whole point of
 * this function existing. The sag used to be added at the call site, outside
 * head_dwell_us(), so it went straight past HEAD_MAX_DWELL_US - a hole in the
 * only energy guard this firmware has. It was invisible while the constant was
 * 0 and would have opened the moment anyone set it, which is precisely when
 * someone is measuring a rail and least wants a surprise. */
uint32_t head_dwell_sag_us(uint8_t density, uint16_t thermal_scale,
                           uint16_t dots, uint16_t dots_max)
{
    uint32_t dwell = head_dwell_us(density, thermal_scale);
    if (dwell == 0u || dots == 0u || dots_max == 0u) return dwell;
    if (dots > dots_max) dots = dots_max;      /* a count we did not bound is not a count */
    dwell += (uint32_t)HEAD_SAG_FULL_US * dots / dots_max;
    if (dwell > HEAD_MAX_DWELL_US) dwell = HEAD_MAX_DWELL_US;
    return dwell;
}

uint32_t head_last_strobe_us(void) { return s_last_strobe_us; }

/* Iteration ceiling for one microsecond of strobe dwell.
 *
 * delay_us() busy-waits on TIM3's counter. If TIM3 ever stops advancing - its
 * clock gated by a stray RCC write, the peripheral reset, a debugger halting
 * the timer - then `while ((uint16_t)(TIM3->CNT - t0) < chunk) {}` never ends,
 * and it never ends WITH A HEAT STROBE ASSERTED. That is a livelock, not a
 * fault: the CPU is executing happily, so Fault_Handler never runs, and the
 * only thing left is the watchdog at 3.2-5.3 s. DECISIONS D31 put a number on
 * what that costs the head.
 *
 * So the strobe does not delegate its own release to the time base. It watches
 * TIM3 *and* counts iterations, and ends on whichever comes first. The loop
 * body is a volatile read, a subtract, a compare and a branch - call it 8
 * cycles at 48 MHz, so about 6 iterations per microsecond. 64 per microsecond
 * is roughly ten times that, which cannot cut a healthy dwell short, and the
 * +4096 covers the short-dwell case where the constant term dominates.
 *
 * With a dead timer the worst case becomes ~6 ms of stuck strobe instead of
 * seconds - 0.16 % of the watchdog's window, and below the head's rated pulse
 * energy at any density this firmware can be asked for.
 *
 * WHY ONLY HERE, and not in delay_us() itself: the other delay_us() call sites
 * on the print path all wait in a COLD state. The 1 us latch pulse at step 2
 * below holds LATCH low with both strobes inactive; vh_enable()'s 2 ms settle
 * holds the rail up with both strobes inactive, and a rail with no strobe puts
 * no current through a dot. A dead timer hangs at the first of those and never
 * reaches the heat at all - verified by stopping TIM3 on the real image in
 * Renode, where execution stops in delay_us() called from the latch. Bounding
 * delay_us() globally would turn every one of those into a silently shortened
 * wait, which is a worse trade for a timing-critical shift register. The guard
 * belongs exactly where heat is possible, which is here.
 *
 * Tested from the host side (test_thermal.c), where TIM3 is a RAM word that
 * nothing increments - the same failure, reproduced for free. Note the shape
 * of that test: remove this guard and it does not fail, it HANGS. */
#define STROBE_SPIN_PER_US  64u
#define STROBE_SPIN_FLOOR   4096u

static void strobe(pin_t p, uint32_t us)
{
    uint32_t guard = us * STROBE_SPIN_PER_US + STROBE_SPIN_FLOOR;
    gpio_set(p, MODEL_STB_ACTIVE_LEVEL);        /* fire the heat drivers */
    {
        uint16_t t0 = (uint16_t)TIM3->CNT;
        /* us is clamped to HEAD_MAX_DWELL_US (410) by head_dwell_sag_us(), far
         * inside TIM3's 65.5 ms wrap, so 16-bit difference arithmetic is exact
         * and no chunking is needed. */
        while ((uint32_t)(uint16_t)((uint16_t)TIM3->CNT - t0) < us) {
            if (--guard == 0u) break;           /* the time base is not moving */
        }
    }
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
#if MODEL_HEAD_SHIFT_LINES == 1
    /* ONE data line, HEAD_DOTS clocks - the vendor's own topology (D36).
     * Byte 0 bit 7 first, exactly as the two-line paths feed DI1; the rest of
     * the line follows on the SAME pin instead of in parallel on a second one. */
    (void)pdi2; (void)d2;
    {
        const uint16_t nb = (uint16_t)(HEAD_BYTES);
        for (uint16_t b = 0; b < nb; b++) {
            uint8_t v = (b < nbytes) ? bits[b] : 0u;
            for (uint8_t m = 0x80u; m; m >>= 1) {
                pdi1->BSRR = (v & m) ? d1 : (d1 << 16);
                pclk->BSRR = ck;
                __asm volatile("nop");
                pclk->BSRR = ck << 16;
            }
        }
    }
#else
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

    /* 3) dwell = base * density/8 * thermal scale/256, capped by energy.
     *    The temperature is sampled ONCE for the whole line: the two halves of
     *    one dot line must not be printed at different darknesses because a
     *    20 ms ADC cache happened to expire between them. */
    const uint16_t t_scale = thermal_dwell_scale();

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
        uint16_t n = dots_in(bits, from, to, nbytes);
        if (HEAD_STROBE_SEGMENTS == 2 && n == 0)
            continue;                        /* nothing to print in this half */
        uint32_t us = head_dwell_sag_us(s_density, t_scale, n,
                                        (uint16_t)(half * 8u));
        strobe(k_strobe[s], us);
        fired += us;
    }

    s_last_strobe_us = fired;
}
