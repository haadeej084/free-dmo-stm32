/* OpenDMO-FW - thermal head driver (ROHM KF3002-family shift-register head).
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
 * Sourced from the datasheet: signal set, LAT polarity (Low=THROUGH), 2x
 * shift-register halves driven in parallel (DI1||DI2), STB polarity
 * (Low = fires the heat driver), built-in NTC thermistor (30 kOhm B=3950 on TM).
 * ASSUMPTIONS (verify on hardware, see PINMAP.md / DECISIONS D16): which MCU
 * pin Dymo wired each signal to, the VH heat-supply voltage on this board, and
 * the base dwell.
 */
#include "head.h"
#include "thermal.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

#define HEAD_BASE_DWELL_US 400      /* base strobe time per half; calibrate on hardware */
static uint8_t s_density = 8;

/* The heat lines, in segment order. head_print_line uses the first
 * HEAD_STROBE_SEGMENTS of them (from model.h). */
static const pin_t k_strobe[4] = {
    PIN_HEAD_STROBE, PIN_HEAD_STROBE2, PIN_HEAD_STROBE3, PIN_HEAD_STROBE4
};

/* Dot `dot` of a line held in `nbytes` bytes (MSB first, dot 0 = MSB of byte
 * 0 per the tech reference). Dots beyond the sent data print white. */
static uint8_t dot_at(const uint8_t *bits, uint16_t nbytes, uint16_t dot)
{
    uint16_t b = (uint16_t)(dot >> 3);
    if (b >= nbytes) return 0;
    return (uint8_t)((bits[b] >> (7u - (dot & 7u))) & 1u);
}

void head_init(void)
{
    /* All head signals are plain GPIO outputs, idle state:
     * CLK/DI low, LAT high (HOLD), strobes high (off — STB is active-low). */
    gpio_mode(PIN_HEAD_CLK,   GPIO_OUT);  gpio_set(PIN_HEAD_CLK, 0);
    gpio_mode(PIN_HEAD_DI1,   GPIO_OUT);  gpio_set(PIN_HEAD_DI1, 0);
    gpio_mode(PIN_HEAD_DI2,   GPIO_OUT);  gpio_set(PIN_HEAD_DI2, 0);
    gpio_mode(PIN_HEAD_LATCH, GPIO_OUT);  gpio_set(PIN_HEAD_LATCH, 1);
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++) {
        gpio_mode(k_strobe[s], GPIO_OUT);
        gpio_set(k_strobe[s], 1);           /* active-low: idle high = off */
    }
}

void head_reset(void)
{
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++) gpio_set(k_strobe[s], 1);
    gpio_set(PIN_HEAD_LATCH, 1);   /* HOLD */
    gpio_set(PIN_HEAD_CLK, 0);
    gpio_set(PIN_HEAD_DI1, 0);
    gpio_set(PIN_HEAD_DI2, 0);
}

void head_set_density(uint8_t d) { if (d >= 1 && d <= 16) s_density = d; }

static void strobe(pin_t p, uint32_t us)
{
    gpio_set(p, 0);                 /* active-low: Low = fire the heat driver */
    while (us > 1000) { delay_us(1000); us -= 1000; }
    delay_us(us);
    gpio_set(p, 1);                 /* High = off */
}

void head_print_line(const uint8_t *bits, uint16_t nbytes)
{
    /* 1) shift in the dot line: one CLK per dot pair (half 1 on DI1, half 2
     *    on DI2). HEAD_DOTS is even for both models (672 = 2x336,
     *    1248 = 2x624). */
    uint16_t half = (uint16_t)(HEAD_DOTS / 2);
    for (uint16_t i = 0; i < half; i++) {
        gpio_set(PIN_HEAD_DI1, dot_at(bits, nbytes, i));
        gpio_set(PIN_HEAD_DI2, dot_at(bits, nbytes, (uint16_t)(half + i)));
        gpio_set(PIN_HEAD_CLK, 1);
        gpio_set(PIN_HEAD_CLK, 0);
    }

    /* 2) latch: Low = THROUGH (datasheet timing chart) */
    gpio_set(PIN_HEAD_LATCH, 0);
    delay_us(1);
    gpio_set(PIN_HEAD_LATCH, 1);

    /* 3) dwell = base * density/8 * thermal scale/256 */
    uint32_t dwell = (uint32_t)HEAD_BASE_DWELL_US * s_density / 8u;
    dwell = dwell * thermal_dwell_scale() / 256u;
    if (dwell > 2000) dwell = 2000; /* hard upper limit per line */

    /* 4) fire the halves sequentially (peak current / number of segments) */
    for (int s = 0; s < HEAD_STROBE_SEGMENTS; s++)
        strobe(k_strobe[s], dwell);
}
