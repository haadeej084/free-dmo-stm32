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
#define MOTOR_STEP_US        1200        /* per half period; calibrate */

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

#if MOTOR_DRIVE == MOTOR_DRIVE_4PHASE
static const uint8_t k_phase[4][4] = {
    {1,0,1,0}, {0,1,1,0}, {0,1,0,1}, {1,0,0,1}   /* full-step */
};
static uint8_t s_ph;
static void step_once(void)
{
    s_ph = (s_ph + 1) & 3;
    gpio_set(PIN_MOTOR_A1, k_phase[s_ph][0]);
    gpio_set(PIN_MOTOR_A2, k_phase[s_ph][1]);
    gpio_set(PIN_MOTOR_B1, k_phase[s_ph][2]);
    gpio_set(PIN_MOTOR_B2, k_phase[s_ph][3]);
    delay_us(MOTOR_STEP_US);
}
#else
static void step_once(void)
{
    gpio_set(PIN_MOTOR_STEP, 1);
    delay_us(MOTOR_STEP_US);
    gpio_set(PIN_MOTOR_STEP, 0);
    delay_us(MOTOR_STEP_US);
}
#endif

void motor_step_lines(uint16_t lines)
{
    motor_enable(1);
    for (uint16_t l = 0; l < lines; l++)
        for (int s = 0; s < MOTOR_STEPS_PER_LINE; s++)
            step_once();
    /* keep enable on between lines; the main loop can later call motor_enable(0) */
}
