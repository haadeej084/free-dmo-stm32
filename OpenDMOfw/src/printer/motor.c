/* OpenDMOfw - feed stepper.
 *
 * One raster line = 1/300 inch of paper = 0.08467 mm (550 TRM: 300 dpi in the
 * feed direction). The µsteps per line is set by the drive train:
 *   mu_steps/line = (N_steps/rev * microstep * gear_ratio) / (pi * D_roller_mm * 11.811)
 * where 11.811 lines/mm = 300 dpi. N_steps/rev, microstepping, gear ratio and
 * roller diameter are NOT published — count the phase pulses during one ESC D
 * line to get it directly (see PINMAP.md / DECISIONS D17).
 *
 * Two wiring variants (select MOTOR_DRIVE):
 *   MOTOR_DRIVE_4PHASE  : direct 4-phase drive (A1/A2/B1/B2) — the EXPECTED
 *                         mode: a small dual-H-bridge (TB6612/MP6500 class) or
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
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

#define MOTOR_DRIVE_STEPDIR 0
#define MOTOR_DRIVE_4PHASE  1
#define MOTOR_DRIVE         MOTOR_DRIVE_4PHASE   /* expected: IN1-IN4 dual H-bridge */

#define MOTOR_STEPS_PER_LINE 1
#define MOTOR_STEP_US        800         /* per step; see the time budget above */

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
    gpio_set(PIN_MOTOR_A1, k_phase[s_ph][0]);
    gpio_set(PIN_MOTOR_A2, k_phase[s_ph][1]);
    gpio_set(PIN_MOTOR_B1, k_phase[s_ph][2]);
    gpio_set(PIN_MOTOR_B2, k_phase[s_ph][3]);
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

void motor_step_lines(uint16_t lines)
{
    motor_enable(1);
    s_energised = 1;
    for (uint16_t l = 0; l < lines; l++)
        for (int s = 0; s < MOTOR_STEPS_PER_LINE; s++)
            step_once();
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
