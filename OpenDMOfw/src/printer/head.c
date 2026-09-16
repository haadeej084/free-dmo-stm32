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
 * pin D.mo wired each signal to, and the base dwell.
 */
#include "head.h"
#include "thermal.h"
#include "../config/store.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

#define HEAD_BASE_DWELL_US 400      /* base strobe time per half; calibrate on hardware */
#define VH_SETTLE_US       2000     /* load-switch rise time before the first strobe */

static uint8_t  s_density = 8;
static int      s_vh_on;
static uint32_t s_vh_ms;
static uint32_t s_last_strobe_us;

/* HEAD_DOTS must split into two whole-byte halves for the shift loop below. */
typedef char head_dots_split_into_bytes[(HEAD_DOTS % 16 == 0) ? 1 : -1];

/* The heat lines, in segment order. head_print_line uses the first
 * HEAD_STROBE_SEGMENTS of them (from model.h). */
static const pin_t k_strobe[4] = {
    PIN_HEAD_STROBE, PIN_HEAD_STROBE2, PIN_HEAD_STROBE3, PIN_HEAD_STROBE4
};

void head_init(void)
{
    /* All head signals are plain GPIO outputs, idle state:
     * CLK/DI low, LAT high (HOLD), strobes high (off - STB is active-low). */
    gpio_mode(PIN_HEAD_CLK,   GPIO_OUT);  gpio_set(PIN_HEAD_CLK, 0);
    gpio_mode(PIN_HEAD_DI1,   GPIO_OUT);  gpio_set(PIN_HEAD_DI1, 0);
    gpio_mode(PIN_HEAD_DI2,   GPIO_OUT);  gpio_set(PIN_HEAD_DI2, 0);
    gpio_mode(PIN_HEAD_LATCH, GPIO_OUT);  gpio_set(PIN_HEAD_LATCH, 1);
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++) {
        gpio_mode(k_strobe[s], GPIO_OUT);
        gpio_set(k_strobe[s], 1);           /* active-low: idle high = off */
    }
    /* The 24 V heat rail starts OFF and is switched on only around an actual
     * print (head_idle_tick drops it again). The genuine printer does the same
     * kind of thing - the 550 TRM describes shutting down unused peripherals
     * after 30 s idle - and on a board where this pin is still an assumption,
     * "off unless printing" is the difference between a wrong guess costing
     * nothing and a wrong guess cooking the head. */
    gpio_mode(PIN_HEAD_VH, GPIO_OUT);
    gpio_set(PIN_HEAD_VH, !HEAD_VH_ON_LEVEL);
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
    const uint16_t hb = (uint16_t)(HEAD_DOTS / 16);   /* bytes per half */

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
