/* OpenDMOfw - host test for the thermal arithmetic and the head's dwell.
 *
 * thermal.c and the dwell computation had no test at all: both host suites
 * replace them with mocks, and the Renode scripts only see the resulting pin
 * activity. This links the REAL thermal.c and head.c against a software model
 * of the ADC, so the parts that decide how much energy goes into an
 * irreplaceable head are checked arithmetically:
 *
 *   - the dwell scale follows the published energy law E = E25 - Tc*(T-25),
 *     i.e. it is linear in TEMPERATURE, not in raw ADC code (the previous
 *     implementation sagged ~5 % low in the middle of the range);
 *   - the over-temperature gate latches at 70 C and only releases at 56 C;
 *   - the median-of-three rejects a single ADC outlier in both directions;
 *   - no combination of density and temperature can exceed the energy ceiling,
 *     and nothing the genuine driver can ask for is clipped by it.
 *
 * Build/run (see Makefile `test`):
 *   cc -std=c11 -DOPENDMO_HOST_TEST -Isrc -o test_thermal test/test_thermal.c \
 *      src/printer/thermal.c src/printer/head.c
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "system.h"
#include "config/store.h"
#include "printer/thermal.h"
#include "printer/head.h"

#include "host_periph.h"

/* The ADC model: a queue of samples, one consumed per conversion. Loading it
 * when ADSTART is seen is what lets a three-sample median be tested. */
static ADC_Type s_adc;
static uint16_t s_queue[64];
static int      s_qn, s_qi;

static void adc_queue(const uint16_t *v, int n)
{
    memcpy(s_queue, v, (size_t)n * sizeof v[0]);
    s_qn = n; s_qi = 0;
}
static void adc_constant(uint16_t v) { adc_queue(&v, 1); }

ADC_Type *host_adc(void)
{
    if (s_adc.CR & ADC_CR_ADSTART) {          /* a conversion was started */
        s_adc.DR = s_qn ? s_queue[s_qi < s_qn ? s_qi : s_qn - 1] : 0;
        if (s_qi < s_qn - 1) s_qi++;
        s_adc.CR &= ~ADC_CR_ADSTART;          /* the hardware clears it on EOC */
        s_adc.ISR |= ADC_ISR_EOC;
    }
    return &s_adc;
}

/* ---- stubs for everything below the arithmetic -------------------------- */
void wdt_kick(void) {}
void delay_us(uint32_t us) { (void)us; }
void delay_ms(uint32_t ms) { (void)ms; }
static uint32_t g_ms;
uint32_t millis(void) { return g_ms; }
void gpio_mode(pin_t p, gpio_mode_t m) { (void)p; (void)m; }
/* Record the level of every pin, so the heat strobes can be observed. */
static int g_level[2][16];
void gpio_set(pin_t p, int high)
{
    int port = (p.port == GPIOB) ? 1 : 0;
    if (p.pin < 16) g_level[port][p.pin] = high ? 1 : 0;
}
int  gpio_get(pin_t p) { (void)p; return 0; }
void gpio_pull(pin_t p, int pull) { (void)p; (void)pull; }
void gpio_od(pin_t p, int od) { (void)p; (void)od; }
void gpio_af(pin_t p, uint8_t af) { (void)p; (void)af; }
static op_config_t g_cfg;
const op_config_t *store_get(void) { return &g_cfg; }

/* ---- the NTC model, mirroring tools/gen_thermal_table.py ----------------- */
#define R25 30000.0
#define BETA 3950.0
#define R_P 20000.0

static uint16_t code_of(double t_c)
{
    double r = R25 * exp(BETA * (1.0 / (t_c + 273.15) - 1.0 / 298.15));
    double frac = R_P / (r + R_P);            /* NTC to VDD, R_P to GND */
    long c = lround(frac * 4095.0);
    if (c < 0) c = 0;
    if (c > 4095) c = 4095;
    return (uint16_t)c;
}
/* The published law, in scale units (256 = 1.0). */
static int law_scale(double t_c)
{
    if (t_c <= 25.0) return 320;
    if (t_c >= 70.0) return 160;
    return (int)lround(320.0 - 160.0 * (t_c - 25.0) / 45.0);
}

static int fails, checks;
#define CHECK(c) do { checks++; if (c) printf("ok   %s\n", #c); \
                      else { printf("FAIL %s  (line %d)\n", #c, __LINE__); fails++; } } while (0)

/* Force a fresh sample: the module caches for 20 ms of millis(). */
static uint16_t scale_at(double t_c)
{
    adc_constant(code_of(t_c));
    g_ms += 1000;
    return thermal_dwell_scale();
}

int main(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);

    /* 1) The dwell scale follows the temperature law, not the ADC code.
     *    The old linear-in-code taper is off by 13/256 at 47.5 C and 10/256 at
     *    60 C, so the 6/256 tolerance here fails against it and passes with
     *    the generated table (whose own quantisation error is <= 5/256). */
    {
        const double t[] = { 25, 30, 35, 40, 45, 47.5, 50, 55, 60, 65, 70 };
        int worst = 0;
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            int got = scale_at(t[i]), want = law_scale(t[i]);
            int err = got > want ? got - want : want - got;
            if (err > worst) worst = err;
        }
        printf("     worst deviation from the published law: %d/256\n", worst);
        CHECK(worst <= 6);
    }

    /* 2) Ends are exact and clamped beyond them. */
    CHECK(scale_at(25.0) == 320);
    CHECK(scale_at(0.0) == 320);
    CHECK(scale_at(70.0) == 160);
    CHECK(scale_at(85.0) == 160);

    /* 3) Monotone non-increasing in temperature, and always inside the ends -
     *    a non-monotone table would make a hotter head print darker. */
    {
        int prev = 400, mono = 1, bounded = 1;
        for (double t = 20.0; t <= 80.0; t += 0.5) {
            int v = scale_at(t);
            if (v > prev) mono = 0;
            if (v < 160 || v > 320) bounded = 0;
            prev = v;
        }
        CHECK(mono);
        CHECK(bounded);
    }

    /* 4) The over-temperature gate latches at 70 C and holds until 56 C. */
    CHECK(scale_at(25.0) && thermal_ok());
    adc_constant(code_of(71.0)); g_ms += 1000;
    CHECK(!thermal_ok());                       /* above the limit: halted */
    adc_constant(code_of(60.0)); g_ms += 1000;
    CHECK(!thermal_ok());                       /* still latched below 70 */
    adc_constant(code_of(55.0)); g_ms += 1000;
    CHECK(thermal_ok());                        /* released at the resume point */

    /* 5) A single ADC outlier must not move the reading - this gate runs for
     *    every printed line, so one glitch must not drop a line or unlatch. */
    {
        uint16_t hot = code_of(25.0);
        uint16_t spike[3] = { hot, 4095, hot };     /* one high outlier */
        adc_queue(spike, 3); g_ms += 1000;
        CHECK(thermal_dwell_scale() == 320);
        uint16_t dip[3] = { hot, 0, hot };          /* one low outlier */
        adc_queue(dip, 3); g_ms += 1000;
        CHECK(thermal_dwell_scale() == 320);
    }

    /* 6) The reading is cached: three conversions per sample, not per call. */
    {
        uint16_t v[3] = { code_of(25.0), code_of(25.0), code_of(25.0) };
        adc_queue(v, 3); g_ms += 1000;
        uint16_t a = thermal_dwell_scale();
        adc_constant(code_of(70.0));            /* new sample, but cached */
        uint16_t b = thermal_dwell_scale();
        CHECK(a == b);
        g_ms += 1000;                           /* past the cache window */
        CHECK(thermal_dwell_scale() == 160);
    }

    /* 7) The energy ceiling: nothing a host can ask for exceeds it, and
     *    nothing the genuine driver asks for is clipped by it. */
    {
        int over = 0;
        for (int d = 0; d <= 16; d++)
            for (int sc = 160; sc <= 320; sc += 8)
                if (head_dwell_us((uint8_t)d, (uint16_t)sc) > 410) over++;
        CHECK(over == 0);
        /* ESC g = 112.5 % is DYMO's darkest preset -> density 9 */
        CHECK(head_dwell_us(9, 320) == 378 && head_dwell_us(9, 320) < 410);  /* not clipped */
        CHECK(head_dwell_us(16, 320) == 410);       /* ESC C 200 % is clipped */
        CHECK(head_dwell_us(0, 320) == 0);          /* density 0 = no heat */

        /* Rail-sag compensation must live INSIDE the energy ceiling. It used
         * to be added at the call site, outside head_dwell_us(), so it went
         * straight past HEAD_MAX_DWELL_US - a hole in the only energy guard
         * this firmware has, invisible while the constant was 0 and open the
         * moment anyone set it. This binary is built twice, the second time
         * with HEAD_SAG_FULL_US forced to 47, so the clamp below is actually
         * exercised rather than merely asserted against zero. */
        {
            int over_sag = 0, mono_sag = 1, zero_ok = 1;
            for (int d = 0; d <= 16; d++)
                for (int sc = 0; sc <= 512; sc += 8)
                    for (int n = 0; n <= 336; n += 16) {
                        uint32_t v = head_dwell_sag_us((uint8_t)d, (uint16_t)sc,
                                                       (uint16_t)n, 336);
                        if (v > 410) over_sag++;
                        if (n == 0 && v != head_dwell_us((uint8_t)d, (uint16_t)sc))
                            zero_ok = 0;
                        if (n > 0 && v < head_dwell_sag_us((uint8_t)d, (uint16_t)sc,
                                                           (uint16_t)(n - 16), 336))
                            mono_sag = 0;
                    }
            CHECK(over_sag == 0);      /* the ceiling holds at every coverage */
            CHECK(zero_ok);            /* a blank segment gets no sag at all  */
            CHECK(mono_sag);           /* more dots never means less dwell    */
            /* A dot count above the segment width is clamped, not scaled past
             * full coverage: an unbounded count is not a count. */
            CHECK(head_dwell_sag_us(8, 256, 9999, 336) ==
                  head_dwell_sag_us(8, 256,  336, 336));
            CHECK(head_dwell_sag_us(8, 256, 336, 0) == head_dwell_us(8, 256));
        }
        /* monotone in density and in temperature scale */
        int mono = 1;
        for (int d = 1; d < 16; d++)
            if (head_dwell_us((uint8_t)d, 256) > head_dwell_us((uint8_t)(d + 1), 256)) mono = 0;
        for (int sc = 160; sc < 320; sc += 8)
            if (head_dwell_us(8, (uint16_t)sc) > head_dwell_us(8, (uint16_t)(sc + 8))) mono = 0;
        CHECK(mono);
    }

    /* The heat strobe must release even if its time base never advances.
     *
     * strobe() busy-waits on TIM3's counter. Under OPENDMO_HOST_TEST that
     * counter is a plain RAM word that nothing increments - which is exactly
     * the on-target failure worth worrying about: a TIM3 whose clock gets
     * gated, whose peripheral is held in reset, or that a debugger halts. A
     * wait that trusts it alone never ends, and it never ends WITH A HEAT
     * STROBE ASSERTED. That is a livelock, not a fault, so the fault handler in
     * startup.c never sees it, and the only thing left is the watchdog seconds
     * later - DECISIONS D31 has the arithmetic on what that costs the head.
     *
     * So this is not a normal assertion: if the iteration guard in strobe() is
     * removed, THIS TEST DOES NOT FAIL, IT HANGS. That is the correct signal
     * (CI reports a timeout), and it is the only way to state the property from
     * the host side. The checks below then confirm the strobes came back to
     * their inactive level rather than merely that the loop exited. */
    {
        static uint8_t line[HEAD_BYTES];
        for (unsigned i = 0; i < HEAD_BYTES; i++) line[i] = 0xFF;  /* solid black */
        g_level[1][0] = g_level[1][1] = MODEL_STB_ACTIVE_LEVEL;    /* poisoned */
        head_set_density(8);
        head_print_line(line, HEAD_BYTES);
        CHECK(g_level[1][0] == !MODEL_STB_ACTIVE_LEVEL);
        CHECK(g_level[1][1] == !MODEL_STB_ACTIVE_LEVEL);
        /* And a blank line must not strobe at all, so the guard is not quietly
         * doing the work that dots_in()'s skip is supposed to do. */
        for (unsigned i = 0; i < HEAD_BYTES; i++) line[i] = 0x00;
        g_level[1][0] = g_level[1][1] = 0x55;                      /* sentinel */
        head_print_line(line, HEAD_BYTES);
        CHECK(g_level[1][0] == 0x55 && g_level[1][1] == 0x55);
    }

    /* An unbelievable reading is a THIRD state, not a cold head.
     *
     * With the NTC to VDD and R_p to GND, an open circuit - a disconnected head
     * flex, or a divider that was never populated, which is the default state of
     * a bring-up board - parks the ADC node at 0 V. Code 0 sits at the far cold
     * end of the curve, so the firmware used to answer "very cold": the D7 gate
     * permanently satisfied AND the longest strobe it will ever ask for, at
     * exactly the moment it knows least about the head. That is the dangerous
     * failure direction getting the maximum energy.
     *
     * The benign direction already worked: a short normalises to 4095, which the
     * existing latch reads as over-temperature and refuses. Both are asserted
     * here so the asymmetry is on the record. */
    {
        adc_constant(0); g_ms += 1000;
        CHECK(thermal_sensor_fault());
        CHECK(thermal_dwell_scale() == 160);      /* least energy, not 320 */
        CHECK(head_dwell_us(8, thermal_dwell_scale()) <
              head_dwell_us(8, 320));

        adc_constant(4095); g_ms += 1000;
        CHECK(thermal_sensor_fault());
        CHECK(!thermal_ok());                     /* short: already refused */
        CHECK(thermal_dwell_scale() == 160);

        /* A working divider must never trip it, at either end of the real
         * operating range or beyond it in both directions. */
        adc_constant(code_of(25.0)); g_ms += 1000;
        CHECK(!thermal_sensor_fault());
        adc_constant(code_of(0.0)); g_ms += 1000;
        CHECK(!thermal_sensor_fault());
        adc_constant(code_of(70.0)); g_ms += 1000;
        CHECK(!thermal_sensor_fault());
        adc_constant(code_of(-20.0)); g_ms += 1000;
        CHECK(!thermal_sensor_fault());           /* cold store, still believable */
    }

    printf(fails ? "\n%d of %d THERMAL check(s) FAILED\n" : "\nALL %d THERMAL CHECKS PASSED\n",
           fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
