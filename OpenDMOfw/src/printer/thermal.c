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
 * not a single threshold. The same manual also describes how the genuine
 * firmware sets dwell: "the control electronics measure the print voltage and
 * the head temperature before each print cycle, and then calculate the required
 * print [energy]". We do the temperature half of that in thermal_dwell_scale();
 * the voltage half needs a divider on the 24 V rail that is not in pins.h yet
 * (see PINMAP.md "head voltage sense").
 *
 * ASSUMPTION (the only one left here): the divider topology on the D.mo board,
 * i.e. the pull resistor R_p and whether the NTC pulls the ADC pin up or down.
 * The defaults below are the R_p = 20 k pull-down column. FIELDWORK.md
 * measurement 3 carries the full table - two ADC readings pick the right column
 * and both thresholds off it, with no arithmetic.
 *
 * NTC curve (30 kOhm at 25 degC, B=3950): R(T) = 30000*exp(3950*(1/T - 1/298.15)),
 * T in kelvin. At the temperatures that matter here:
 *   25 C = 30.00 k    56 C = 8.61 k    70 C = 5.28 k
 */
#include "thermal.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

/* 1 = NTC to VDD and R_p to GND, so a hotter head reads HIGHER.
 * 0 = R_p to VDD and NTC to GND, so a hotter head reads LOWER. */
#define THERMAL_HOTTER_IS_HIGHER 1

/* Raw 12-bit ADC codes for the three temperatures above, R_p = 20 k pull-down.
 * Enter the pull-down column for your R_p even when THERMAL_HOTTER_IS_HIGHER
 * is 0 - the code inverts the reading first. See FIELDWORK.md measurement 3. */
#define THERMAL_LIMIT_RAW   3240   /* 70 degC: halt printing (TRM)            */
#define THERMAL_RESUME_RAW  2862   /* 56 degC: printing may resume (TRM)      */
#define THERMAL_COLD_RAW    1638   /* 25 degC: at/below this, full dwell      */

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
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    gpio_mode(((pin_t){GPIOA, 1}), GPIO_ANALOG);   /* PA1 = ADC_IN1 */

    /* F0 ADC: calibrate while ADEN=0, then enable (RM0091). */
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) {}
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {}
    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    ADC1->SMPR   = 7;                              /* longest sample time */
    s_have_raw = 0;
    s_over_temp = 0;
}

static uint16_t adc_sample(void)
{
    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    ADC1->CR |= ADC_CR_ADSTART;
    uint32_t guard = 100000;
    while (!(ADC1->ISR & ADC_ISR_EOC) && --guard) {}
    return (uint16_t)ADC1->DR;
}

/* Median of three. This reading gates every printed line and the GS D head
 * self-test, so a single ADC glitch must not be able to drop a raster line or
 * refuse a diagnostic. Median rejects a lone outlier without the lag an
 * averaging filter would add. */
static uint16_t sample_median3(void)
{
    uint16_t a = adc_sample(), b = adc_sample(), c = adc_sample(), t;
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

void thermal_scan_adc(uint16_t out[10])
{
    for (uint8_t ch = 0; ch < 10; ch++) {
        pin_t p = (ch < 8) ? (pin_t){ GPIOA, ch }
                           : (pin_t){ GPIOB, (uint8_t)(ch - 8) };
        uint32_t sh = (uint32_t)p.pin * 2u;
        uint32_t save = p.port->MODER;
        p.port->MODER = (save & ~(3u << sh)) | ((uint32_t)GPIO_ANALOG << sh);
        ADC1->CHSELR = (1u << ch);
        out[ch] = adc_sample();
        p.port->MODER = save;      /* restore mode; the output level is untouched */
    }
    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    s_have_raw = 0;                /* the cached thermistor reading is stale now */
}

uint16_t thermal_dwell_scale(void)
{
    uint16_t v = thermal_hot_scale();
    if (v <= THERMAL_COLD_RAW)  return 320;        /* 25 C and below: 1.25x dwell */
    if (v >= THERMAL_LIMIT_RAW) return 160;        /* 70 C: 0.625x dwell          */
    /* Linear taper 320 -> 160 between 25 C and 70 C. The genuine firmware
     * computes energy from temperature and head voltage; this is the
     * temperature half of that, kept monotonic and bounded. */
    uint32_t span = THERMAL_LIMIT_RAW - THERMAL_COLD_RAW;
    uint32_t d = (uint32_t)v - THERMAL_COLD_RAW;
    return (uint16_t)(320u - (160u * d) / span);
}
