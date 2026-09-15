/* OpenDMOfw - head thermistor via ADC + safety limiting.
 *
 * The head has a BUILT-IN NTC thermistor (TM pin): 30 kOhm, B = 3950 — sourced
 * from the ROHM KF3002-GL50A datasheet (equivalent circuit "THERMISTOR 30k
 * B:3950" + Fig.5 thermistor curve). NTC means hotter = LOWER resistance.
 *
 * ASSUMPTION (verify on hardware, see PINMAP.md / DECISIONS D16): the divider
 * topology on the D.mo board. If the thermistor sits between the ADC pin and
 * GND (pull-up to VDD), hotter = lower voltage = lower ADC value, so set
 * THERMAL_HOTTER_IS_HIGHER 0. If it sits between VDD and the ADC pin
 * (pull-down to GND), hotter = higher ADC value (=1). The limit threshold is
 * deliberately conservative: a from-scratch firmware must not let the head run
 * hot; calibrate THERMAL_LIMIT_RAW against the Fig.5 curve + a real reading.
 *
 * NTC curve (30 kOhm at 25 degC, B=3950): R(T) = 30000*exp(3950*(1/T - 1/298.15)),
 * T in kelvin. Reference values: 25 degC = 30.0 kOhm, 45 degC = ~13.4 kOhm,
 * 60 degC = ~7.8 kOhm. ONE measurement at a known temperature pins the divider
 * resistor R_p (ADC = R_NTC/(R_p+R_NTC)*4095 for a pull-up), after which any
 * ADC reading maps to a temperature.
 */
#include "thermal.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

#define THERMAL_HOTTER_IS_HIGHER 1
/* Raw threshold (12-bit ADC 0..4095). Calibrate with the thermistor curve;
 * default assumes a 30 kOhm / B3950 NTC in a typical divider. */
#define THERMAL_LIMIT_RAW 2600
#define THERMAL_COLD_RAW  1400   /* at/below this value: full dwell scale */

void thermal_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    gpio_mode(((pin_t){GPIOA, 1}), GPIO_ANALOG);   /* PA1 = ADC_IN1 */

    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {}
    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    ADC1->SMPR   = 7;                              /* longest sample time */
}

uint16_t thermal_read_raw(void)
{
    ADC1->CHSELR = (1u << ADC_HEAD_TEMP_CH);
    ADC1->CR |= ADC_CR_ADSTART;
    uint32_t guard = 100000;
    while (!(ADC1->ISR & ADC_ISR_EOC) && --guard) {}
    return (uint16_t)ADC1->DR;
}

int thermal_ok(void)
{
    uint16_t v = thermal_read_raw();
#if THERMAL_HOTTER_IS_HIGHER
    return v < THERMAL_LIMIT_RAW;
#else
    return v > (4095 - THERMAL_LIMIT_RAW);
#endif
}

uint16_t thermal_dwell_scale(void)
{
    uint16_t v = thermal_read_raw();
#if !THERMAL_HOTTER_IS_HIGHER
    v = 4095 - v;
#endif
    if (v <= THERMAL_COLD_RAW) return 320;         /* cold: 1.25x dwell */
    if (v >= THERMAL_LIMIT_RAW) return 160;        /* hot: 0.625x dwell */
    /* linear between cold and limit: 320 -> 160 */
    uint32_t span = THERMAL_LIMIT_RAW - THERMAL_COLD_RAW;
    uint32_t d = v - THERMAL_COLD_RAW;
    return (uint16_t)(320 - (160 * d) / span);
}
