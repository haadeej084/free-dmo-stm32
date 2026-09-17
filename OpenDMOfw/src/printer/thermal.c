/* OpenDMOfw - head thermistor via ADC + safety limiting.
 *
 * The head has a BUILT-IN NTC thermistor (TM pin): 30 kOhm, B = 3950 - sourced
 * from the ROHM KF3002-GL50A datasheet (equivalent circuit "THERMISTOR 30k
 * B:3950" + Fig.5 thermistor curve). NTC means hotter = LOWER resistance.
 *
 * THERMAL POLICY IS NOT INVENTED. The LabelWriter 450 Series Technical
 * Reference Manual (Sanford/DYMO, p.7) states the genuine printer's rule:
 *
 *   "In order to protect the print head from excessive heat, the control
 *    electronics halt printing if the print head temperature exceeds 70 C.
 *    Printing resumes when the print head cools to 56 C."
 *
 * So: halt at 70 C, resume at 56 C - a latched limit with 14 C of hysteresis,
 * not a single threshold.
 *
 * CONFIRMED AGAINST THE GENUINE FIRMWARE (DECISIONS D30). The LabelWriter 450
 * image - which drives this very mechanism, per the owner's mainboard-swap
 * report - carries the same two thresholds as raw ADC counts: it halts at 176
 * and resumes only above 255, on a 10-bit channel whose sense is inverted
 * (higher count = colder). Fitting the KF3002's own 30 kOhm B=3950 NTC through
 * a single pull-up reproduces 70 C at 174 counts and 56 C at 256, i.e. both to
 * about 1 %. This is no longer a rule read out of a manual: it is the same rule
 * observed in shipping code, on the same head.
 *
 * The same manual also describes how the genuine firmware sets dwell: "the
 * control electronics measure the print voltage and the head temperature before
 * each print cycle, and then calculate the required print [energy]". The
 * recovered firmware does exactly that, and D30 has the arithmetic. We do the
 * temperature half of it in thermal_dwell_scale(); the voltage half is the
 * 450's ADC channel 6, which gates printing (suspend below 19.3 V, resume at
 * 21 V) rather than entering the pulse width - and it needs a divider on the
 * 24 V rail that is not in pins.h yet (see PINMAP.md "head voltage sense").
 *
 * ASSUMPTION (the only one left here): the divider topology on the 550 board,
 * i.e. the pull resistor R_p and whether the NTC pulls the ADC pin up or down.
 * The defaults below are the 450 BOARD'S: a single ~25.75 k pull-UP with the
 * NTC to ground, so a hotter head reads LOWER - that is the divider which
 * reproduces the 450 firmware's own 176/255 thresholds at 70/56 C (D30, D41),
 * and the owner's rule is to start from the 450. The 550 board is its own
 * design, so FIELDWORK measurement 3 still checks it: two ADC readings pick the
 * column and both thresholds off it, with no arithmetic.
 *
 * NTC curve (30 kOhm at 25 degC, B=3950): R(T) = 30000*exp(3950*(1/T - 1/298.15)),
 * T in kelvin. At the temperatures that matter here:
 *   25 C = 30.00 k    56 C = 8.61 k    70 C = 5.28 k
 * Independently confirmed: a published mechanism reference tabulates the same
 * class of head thermistor at 25 C = 30.00 k, 55 C = 8.92 k and 70 C = 5.27 k,
 * against 5.280 k computed here - 0.2 % apart. tools/gen_thermal_table.py
 * --check reprints that comparison.
 */
#include "thermal.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

/* 1 = NTC to VDD and R_p to GND, so a hotter head reads HIGHER.
 * 0 = R_p to VDD and NTC to GND, so a hotter head reads LOWER - the 450's
 *     topology ("higher count = colder", D30) and the default since D41. */
#define THERMAL_HOTTER_IS_HIGHER 0

/* NORMALISED 12-bit codes (higher = hotter) for the three temperatures above,
 * R_p = 25.75 k. The code inverts a pull-up board's reading before comparing,
 * and the inverted pull-up code equals the pull-down code for the same R_p -
 * so these are the "pull-down column" numbers whatever the board does. See
 * FIELDWORK.md measurement 3; tools/gen_thermal_table.py --check prints them. */
#define THERMAL_LIMIT_RAW   3398   /* 70 degC: halt printing (TRM)            */
#define THERMAL_RESUME_RAW  3068   /* 56 degC: printing may resume (TRM)      */
#define THERMAL_COLD_RAW    1891   /* 25 degC: at/below this, full dwell      */

/* Plausibility band on the NORMALISED reading (higher = hotter, whichever way
 * the divider is wired). Outside it, the sensor is not telling us about a
 * temperature at all.
 *
 * THE FAILURE THAT MATTERS IS THE OPEN CIRCUIT, and it used to be invisible.
 * With the NTC to VDD and R_p to GND (the topology shipped before D41), a
 * disconnected head flex - or a divider
 * that was never populated, which is the DEFAULT STATE OF A BRING-UP BOARD -
 * parks the ADC node at 0 V. That reads as code 0, which the curve calls "very
 * cold", so thermal_ok() stayed true and thermal_dwell_scale() returned 320:
 * the gate permanently satisfied AND the longest strobe this firmware will ever
 * ask for, at exactly the moment it knows least about the head. The pull-up
 * topology that is the default now (D41) fails the same way mirrored (open
 * pulls the node to VDD, which normalises to 0), so the check belongs here,
 * after normalisation, where one band covers both.
 *
 * The SHORT is the benign direction: it normalises to 4095, which the existing
 * latch already reads as over-temperature and refuses.
 *
 * Bounds, from the same NTC curve as the table above (30 k at 25 C, B = 3950,
 * R_p = 25.75 k): code 64 is about -44 C and code 4032 about +169 C. Both are
 * far outside anything a printer can be in - the operating band is 1891..3398 - so
 * a working divider cannot trip this, and a disconnected one always does. */
#define THERMAL_OPEN_RAW      64   /* at/below: open circuit or no divider     */
#define THERMAL_SHORT_RAW   4032   /* at/above: shorted sensor                 */

/* Re-sampling interval. The head's thermal mass moves in seconds, so sampling
 * per line (thermal_ok() and thermal_dwell_scale() are both called for every
 * dot line) burns ADC time for no information. It also makes the two agree
 * with each other within one line, which matters: the gate and the dwell must
 * be computed from the same temperature. */
#define THERMAL_CACHE_MS 20u

static uint16_t s_raw = THERMAL_COLD_RAW;   /* last reading, cold until measured */
static uint32_t s_raw_ms;
static int      s_have_raw;
static int      s_over_temp;                /* latched: set at 70 C, cleared at 56 C */

void thermal_init(void)
{
    /* ES0223 2.4.1 "ADC calibration must not be performed twice without an
     * intervening ADC disable": make this structurally impossible rather than
     * a convention, so a future re-init path cannot wedge the ADCAL loop. */
    static int s_inited;

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    /* ADC_INn is PAn for n <= 7 (RM0091 13.3.4), so the channel number in
     * pins.h is also the pad - one constant, not two that can drift apart. */
    _Static_assert(ADC_HEAD_TEMP_CH <= 7, "ADC_INn == PAn only holds for n <= 7");
    gpio_mode(((pin_t){GPIOA, ADC_HEAD_TEMP_CH}), GPIO_ANALOG);
#if PAPER_SENSE_ANALOG
    _Static_assert(PAPER_ADC_CH <= 7, "ADC_INn == PAn only holds for n <= 7");
    gpio_mode(((pin_t){GPIOA, PAPER_ADC_CH}), GPIO_ANALOG);
#endif

    if (!s_inited) {
        /* F0 ADC: calibrate while ADEN=0, then enable (RM0091). Every wait is
         * bounded and kicks the watchdog - an unbounded poll here would turn a
         * dead ADC into a silent boot loop, since wdt_init() runs first. */
        uint32_t g = 100000;
        ADC1->CR |= ADC_CR_ADCAL;
        while ((ADC1->CR & ADC_CR_ADCAL) && --g) { wdt_kick(); }

        /* ES0223 2.4.3 "ADEN bit cannot be set immediately after the ADC
         * calibration": ST requires a gap of at least four ADC clock cycles
         * between clearing ADCAL and setting ADEN. At 48 MHz the few core
         * cycles between the two statements are shorter than four cycles of
         * the 14 MHz ADC clock, so we land inside the erratum window without
         * this delay. ST's workaround is also to re-assert ADEN until ADRDY
         * comes up, which the loop below does. */
        delay_us(1);
        g = 100000;
        do {
            ADC1->CR |= ADC_CR_ADEN;
            wdt_kick();
        } while (!(ADC1->ISR & ADC_ISR_ADRDY) && --g);
        s_inited = 1;
    }

    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    ADC1->SMPR   = 7;                              /* longest sample time */
    s_have_raw = 0;
    s_over_temp = 0;
}

/* Convert one channel. The channel is selected here, per conversion: a single
 * shared "select the thermistor" line at the top of this function silently
 * overrode the per-channel selection made by thermal_scan_adc(), so the scan
 * reported the thermistor ten times and could identify nothing.
 *
 * On a timeout the conversion is stopped and any stale result drained. RM0091
 * 13.5 allows writing CHSELR only while ADSTART is 0 and warns there is "no
 * hardware protection preventing software from making write operations
 * forbidden"; leaving a conversion running would make every later channel
 * selection land in that window and bring the same symptom back in a subtler
 * form. */
static uint16_t adc_sample_ch(uint8_t ch)
{
    ADC1->CHSELR = (1u << ch);
    ADC1->CR |= ADC_CR_ADSTART;
    uint32_t guard = 100000;
    while (!(ADC1->ISR & ADC_ISR_EOC) && --guard) { }
    if (!guard) {
        ADC1->CR |= ADC_CR_ADSTP;
        uint32_t stop = 100000;
        while ((ADC1->CR & ADC_CR_ADSTP) && --stop) { }
        (void)ADC1->DR;                     /* drain, clears EOC */
        wdt_kick();
        return 0;
    }
    return (uint16_t)ADC1->DR;
}

/* Median of three. This reading gates every printed line and the GS D head
 * self-test, so a single ADC glitch must not be able to drop a raster line or
 * refuse a diagnostic. Median rejects a lone outlier without the lag an
 * averaging filter would add. */
static uint16_t sample_median3(void)
{
    uint16_t a = adc_sample_ch(ADC_HEAD_TEMP_CH),
             b = adc_sample_ch(ADC_HEAD_TEMP_CH),
             c = adc_sample_ch(ADC_HEAD_TEMP_CH), t;
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = a; a = b; b = t; }
    return b;
}

/* Raw code, normalised so that HIGHER always means HOTTER regardless of the
 * divider topology. Cached for THERMAL_CACHE_MS. */
static uint16_t thermal_hot_scale(void)
{
    uint32_t now = millis();
    if (!s_have_raw || (now - s_raw_ms) >= THERMAL_CACHE_MS) {
        uint16_t v = sample_median3();
#if !THERMAL_HOTTER_IS_HIGHER
        v = (uint16_t)(4095u - v);
#endif
        s_raw = v;
        s_raw_ms = now;
        s_have_raw = 1;
        /* Latched limit with hysteresis, exactly as the genuine printer:
         * halt above 70 C, resume only once back down at 56 C. */
        if (s_over_temp) { if (s_raw <= THERMAL_RESUME_RAW) s_over_temp = 0; }
        else             { if (s_raw >= THERMAL_LIMIT_RAW)  s_over_temp = 1; }
    }
    return s_raw;
}

/* The raw ADC code as the pin actually reads it (GS D 0x04 reports this, and a
 * fieldwork report needs the un-normalised value to identify the divider). */
uint16_t thermal_read_raw(void)
{
    uint16_t v = thermal_hot_scale();
#if !THERMAL_HOTTER_IS_HIGHER
    v = (uint16_t)(4095u - v);
#endif
    return v;
}

int thermal_ok(void)
{
    thermal_hot_scale();          /* refreshes the latch */
    return !s_over_temp;
}

/* Deliberately NOT folded into thermal_ok().
 *
 * A broken sensor and a hot head call for different responses. emit_line()
 * waits up to a second per dot line for thermal_ok(), then prints anyway (D7);
 * if a sensor fault closed that gate, a printer with an unpopulated divider
 * would take seventeen minutes per address label. That is not "safe", it is
 * broken. So the gate stays a TEMPERATURE gate and the fault is reported
 * separately, with two consequences drawn where they belong:
 *
 *   - thermal_dwell_scale() returns the MINIMUM scale, so the failure that used
 *     to grant maximum energy now grants the least. This is the actual fix.
 *   - protocol_self_test() refuses outright, because D7's reasoning applies
 *     there and nowhere else: nobody is waiting on its output, it is the
 *     operator's own bring-up tool, and on a bring-up board the thermistor is
 *     precisely what is missing.
 *
 * The host is told through status byte 8, whose value 2 means "unknown" - which
 * is what the tech reference gives as that byte's default, and exactly what we
 * know here. */
int thermal_sensor_fault(void)
{
    uint16_t v = thermal_hot_scale();
    return (v <= THERMAL_OPEN_RAW) || (v >= THERMAL_SHORT_RAW);
}

void thermal_scan_adc(uint16_t out[10])
{
    for (uint8_t ch = 0; ch < 10; ch++) {
        pin_t p = (ch < 8) ? (pin_t){ GPIOA, ch }
                           : (pin_t){ GPIOB, (uint8_t)(ch - 8) };
        uint32_t sh = (uint32_t)p.pin * 2u;
        uint32_t save = p.port->MODER;
        p.port->MODER = (save & ~(3u << sh)) | ((uint32_t)GPIO_ANALOG << sh);
        out[ch] = adc_sample_ch(ch);
        p.port->MODER = save;      /* restore mode; the output level is untouched */
    }
    /* Cosmetic: every conversion selects its own channel now, so this only
     * leaves the register in its resting state. */
    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    s_have_raw = 0;                /* the cached thermistor reading is stale now */
}

/* Dwell scale (256 = 1.0), LINEAR IN TEMPERATURE - not in raw ADC code.
 *
 * SOURCED (that a law of this shape exists for our printer): the LW450 TRM
 * p.7 says the engine "measure[s] the print voltage and the head temperature
 * before each print cycle, and then calculate[s] the required print strobe
 * time".
 * ANALOGUE (its form and slope): published thermal-mechanism references give
 * E = E25 - Tc*(T-25); for one characterised paper that is -1.16 %/K, and a
 * shipping 24 V product's own strobe table is linear in temperature at
 * -1.38 %/K with the same relative slope at every line rate, i.e. temperature
 * is a pure multiplicative factor independent of print speed.
 * ASSUMED (our endpoints): 320/256 at 25 C and 160/256 at 70 C = -1.11 %/K,
 * within 5 % of the published coefficient. This change linearises the middle;
 * it deliberately does not retune the ends.
 *
 * Why a table: the previous code tapered linearly in RAW CODE, and code(T) for
 * an NTC divider is S-shaped, so the delivered curve sagged below the law in
 * the middle - 227/256 where the law wants 240 at 47.5 C (-5.5 %), 186 vs 196
 * at 60 C. Always on the cold-energy side, never dangerous, but not the law.
 * The table is generated by tools/gen_thermal_table.py from declared inputs
 * (R25 = 30 k, B = 3950, R_p, divider topology); re-run it if the divider
 * turns out different. Indexed by raw >> 7, so no floating point on the M0.
 *
 * Paper matters more than this correction: published Tc spans a factor 2.2
 * across papers. Once DYMO stock is characterised, Tc belongs in the config,
 * not in a constant here. */
static const uint16_t k_dwell_scale[32] = {
    320, 320, 320, 320, 320, 320, 320, 320,
    320, 320, 320, 320, 320, 320, 320, 313,
    302, 292, 281, 270, 258, 246, 232, 217,
    201, 183, 161, 160, 160, 160, 160, 160,
};

uint16_t thermal_dwell_scale(void)
{
    uint16_t v = thermal_hot_scale();
    /* An unbelievable reading gets the LEAST energy, not the most. Before this
     * an open thermistor read as 0, fell into the "25 C and below" branch on
     * the next line, and returned the cold maximum. */
    if (v <= THERMAL_OPEN_RAW || v >= THERMAL_SHORT_RAW) return 160;
    if (v <= THERMAL_COLD_RAW)  return 320;        /* 25 C and below: 1.25x dwell */
    if (v >= THERMAL_LIMIT_RAW) return 160;        /* 70 C: 0.625x dwell          */
    return k_dwell_scale[(v >> 7) & 31u];
}

/* ---- Top-of-form photocell -------------------------------------------------
 *
 * The genuine 450 firmware reads its label-gap sensor as an ANALOG channel and
 * squares it up with a software Schmitt trigger at 294 / 320 counts on 10 bits
 * (FIELDWORK 3.1 row 7). A digital read of an analog node is a coin toss near
 * the threshold, so the firmware does the same as the vendor: sample, and only
 * change state past the far threshold (D42). Thresholds are the 450's scaled to
 * 12 bits; the DIRECTION (which side is "no paper") is an assumption, see
 * pins.h. Cached like the thermistor. Nothing depends on this for printing -
 * OP_FLAG_PAPER_FORCE is the default - so a wrong answer costs the panel light,
 * not a label. */
#if PAPER_SENSE_ANALOG
static int      s_paper_present = 1;      /* until the first sample says otherwise */
static uint32_t s_paper_ms;
static int      s_paper_have;

int paper_present(void)
{
    uint32_t now = millis();
    if (!s_paper_have || (now - s_paper_ms) >= THERMAL_CACHE_MS) {
        uint16_t v = adc_sample_ch(PAPER_ADC_CH);
#if !PAPER_ADC_HIGH_IS_ABSENT
        v = (uint16_t)(4095u - v);
#endif
        if (s_paper_present) { if (v >= PAPER_ADC_ABSENT_ABOVE)  s_paper_present = 0; }
        else                 { if (v <= PAPER_ADC_PRESENT_BELOW) s_paper_present = 1; }
        s_paper_ms = now;
        s_paper_have = 1;
    }
    return s_paper_present;
}
#else
int paper_present(void)
{
    return gpio_get(PIN_PAPER_SENSE) == PAPER_PRESENT_LEVEL;
}
#endif
