/* OpenDMOfw - feed stepper.
 *
 * One raster line = 1/300 inch of paper = 0.08467 mm (550 TRM: 300 dpi in the
 * feed direction). The µsteps per line is set by the drive train:
 *   mu_steps/line = (N_steps/rev * microstep * gear_ratio) / (pi * D_roller_mm * 11.811)
 * where 11.811 lines/mm = 300 dpi. N_steps/rev, microstepping, gear ratio and
 * roller diameter are NOT published — count the phase pulses during one ESC D
 * line to get it directly (see PINMAP.md / DECISIONS D17).
 * What IS sourced: the LEILI 35BY412 family is 7.5 deg per full step, i.e. 48
 * steps/rev (leili-motor.net 35BY412 page), with no built-in gearbox on the
 * -339 part, so any reduction is in the printer's gear train. At the rated
 * 62 labels/min (about 1090 lines/s) one full step per line means ~1360 rpm,
 * which a 24 V low-resistance PM stepper can do; two per line (~2700 rpm)
 * is implausible. 1 is therefore the best estimate, not a measurement
 * (DECISIONS D24).
 *
 * Two wiring variants (select MOTOR_DRIVE):
 *   MOTOR_DRIVE_4PHASE  : direct 4-phase drive (A1/A2/B1/B2) — the EXPECTED
 *                         mode: a 24 V-capable driver (MP6500-class chopper) or
 *                         discrete H-bridge on 24 V drives IN1-IN4 directly, no
 *                         separate STEP/DIR chip.
 *   MOTOR_DRIVE_STEPDIR : STEP/DIR/ENABLE to an external driver IC (fallback).
 *
 * TIME BUDGET (sourced). DYMO rates the 550 at 62 labels/min and the 5XL at 53,
 * on a 4-line address label = 89 mm = 1050 dot lines. That is 0.92 ms and
 * 1.08 ms per line respectively, feed included. MOTOR_STEP_US must therefore
 * end up at or under ~900 us for one step per line, and the step has to overlap
 * the head strobe (motor_step_line_after) rather than follow it.
 *
 * ASSUMPTIONS (PINMAP.md): the drive variant, MOTOR_STEPS_PER_LINE and the step
 * timing are safe starting values; calibrate so one dot line advances exactly
 * one head line height (no stretching/compression of the image).
 */
#include "motor.h"
#include "../model.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

#define MOTOR_DRIVE_STEPDIR 0
#define MOTOR_DRIVE_4PHASE  1
#define MOTOR_DRIVE         MOTOR_DRIVE_4PHASE   /* expected: IN1-IN4 dual H-bridge */

#define MOTOR_STEPS_PER_LINE 1
/* Derived, not chosen: the line period is a model constant (model.h) because
 * the head's energy ceiling depends on it. One step per line makes the two
 * equal today; if MOTOR_STEPS_PER_LINE is ever calibrated to something else,
 * the step shortens and the LINE period - the quantity that matters to the
 * head - stays put. */
#define MOTOR_STEP_US        (MODEL_LINE_PERIOD_US / MOTOR_STEPS_PER_LINE)

void motor_init(void)
{
#if MOTOR_DRIVE == MOTOR_DRIVE_STEPDIR
    gpio_mode(PIN_MOTOR_STEP,   GPIO_OUT);
    gpio_mode(PIN_MOTOR_DIR,    GPIO_OUT);
    gpio_mode(PIN_MOTOR_ENABLE, GPIO_OUT);
    gpio_set(PIN_MOTOR_DIR, 1);          /* forward feed direction */
    gpio_set(PIN_MOTOR_ENABLE, 1);       /* active-low: off until we print */
    gpio_set(PIN_MOTOR_STEP, 0);
#else
    gpio_mode(PIN_MOTOR_A1, GPIO_OUT); gpio_mode(PIN_MOTOR_A2, GPIO_OUT);
    gpio_mode(PIN_MOTOR_B1, GPIO_OUT); gpio_mode(PIN_MOTOR_B2, GPIO_OUT);
#endif
}

void motor_enable(int on)
{
#if MOTOR_DRIVE == MOTOR_DRIVE_STEPDIR
    gpio_set(PIN_MOTOR_ENABLE, on ? 0 : 1);   /* active-low */
#else
    if (!on) { gpio_set(PIN_MOTOR_A1,0); gpio_set(PIN_MOTOR_A2,0);
               gpio_set(PIN_MOTOR_B1,0); gpio_set(PIN_MOTOR_B2,0); }
#endif
}

/* Advance the phase/STEP output WITHOUT the settle delay, so the caller can
 * decide how much of it has already elapsed. */
#if MOTOR_DRIVE == MOTOR_DRIVE_4PHASE
static const uint8_t k_phase[4][4] = {
    {1,0,1,0}, {0,1,1,0}, {0,1,0,1}, {1,0,0,1}   /* full-step */
};
static uint8_t s_ph;
static void step_pulse(void)
{
    s_ph = (s_ph + 1) & 3;
    const uint8_t *p = k_phase[s_ph];
    /* Break before make. The four pins used to be written unconditionally in
     * A1, A2, B1, B2 order, and two of the four transitions in the sequence put
     * a coil's RISING pin before its FALLING one - so both ends of that winding
     * were driven high for the gap between two gpio_set() calls. Measured on the
     * -Os Cortex-M0 build that gap is about 0.7 us (gpio_set does not inline),
     * and it happened on every second step.
     *
     * On an integrated dual H-bridge that is a brake pulse; on the discrete
     * four-transistor bridge D17 also allows, it is rail-to-rail
     * cross-conduction; on a unipolar winding it is both halves fighting.
     * Dropping first costs four predicated writes inside an 800 us step and
     * removes the question entirely. */
    if (!p[0]) gpio_set(PIN_MOTOR_A1, 0);
    if (!p[1]) gpio_set(PIN_MOTOR_A2, 0);
    if (!p[2]) gpio_set(PIN_MOTOR_B1, 0);
    if (!p[3]) gpio_set(PIN_MOTOR_B2, 0);
    if (p[0])  gpio_set(PIN_MOTOR_A1, 1);
    if (p[1])  gpio_set(PIN_MOTOR_A2, 1);
    if (p[2])  gpio_set(PIN_MOTOR_B1, 1);
    if (p[3])  gpio_set(PIN_MOTOR_B2, 1);
}
#else
static void step_pulse(void)
{
    gpio_set(PIN_MOTOR_STEP, 1);
    delay_us(MOTOR_STEP_US / 2u);       /* driver-IC minimum pulse width */
    gpio_set(PIN_MOTOR_STEP, 0);
}
#endif

static void step_once(void)
{
    step_pulse();
    delay_us(MOTOR_STEP_US);
}

static uint32_t s_last_step_ms;
static int      s_energised;

/* A long feed is the one loop in this firmware that can outlast the watchdog:
 * protocol.c caps a feed at MAX_FEED_DOTS (4000) lines, and at MOTOR_STEP_US
 * (800 us) per step that is 3.2 s of uninterrupted stepping, against an IWDG
 * timeout of 128*1251/f_LSI = 3.2 s at the datasheet's maximum LSI of 50 kHz
 * (4.0 s typical). With MOTOR_DRIVE_STEPDIR, step_pulse() adds another half
 * period per step and a full feed reaches 4.8 s, past even the typical timeout.
 * So the kick belongs in the loop body, not around the call. */
void motor_step_lines(uint16_t lines)
{
    motor_enable(1);
    s_energised = 1;
    for (uint16_t l = 0; l < lines; l++) {
        wdt_kick();
        for (int s = 0; s < MOTOR_STEPS_PER_LINE; s++)
            step_once();
    }
    s_last_step_ms = millis();
    /* Holding torque stays on; motor_idle_tick() drops it once feeding stops.
     * Cutting it right after every call would de-energise the coils between the
     * 64-byte USB packets of a single label and let the paper slip. */
}

void motor_step_line_after(uint32_t elapsed_us)
{
    motor_enable(1);
    s_energised = 1;
    for (int s = 0; s < MOTOR_STEPS_PER_LINE; s++) {
        step_pulse();
        /* Only the LAST step of the line may be credited with the strobe time;
         * intermediate microsteps still need their full spacing. */
        if (s + 1 < MOTOR_STEPS_PER_LINE) delay_us(MOTOR_STEP_US);
    }
    if (elapsed_us < MOTOR_STEP_US) delay_us(MOTOR_STEP_US - elapsed_us);
    s_last_step_ms = millis();
}

void motor_idle_tick(uint32_t idle_ms)
{
    if (!s_energised) return;
    if ((millis() - s_last_step_ms) < idle_ms) return;
    motor_enable(0);
    s_energised = 0;
}
