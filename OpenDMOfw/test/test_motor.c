/* OpenDMOfw - host tests for the feed stepper.
 *
 * motor.c had no host harness at all. What it does is almost entirely a
 * SEQUENCE of GPIO writes, and a sequence is exactly the thing a "check the
 * final state" test cannot see - the same blind spot that let cycle 2's
 * feed-axis bug pass 138 scenarios. So this harness records every gpio_set()
 * in the order the firmware issues it and asserts on the ORDER, not on where
 * the pins end up.
 *
 * The property that matters electrically: a coil must never have both ends
 * driven high at the same time. Before the break-before-make change that
 * happened on every second step, for the ~0.7 us between two gpio_set() calls
 * on the -Os Cortex-M0 build - a brake pulse on an integrated H-bridge, and
 * rail-to-rail cross-conduction on the discrete four-transistor bridge that
 * DECISIONS D17 also allows.
 *
 * Build (see the Makefile `test` target):
 *   cc -DOPENDMO_HOST_TEST -DMODEL_OP57 -Isrc test/test_motor.c src/printer/motor.c
 */
#include <stdio.h>
#include <stdint.h>
#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "system.h"
#include "printer/motor.h"
#include "host_periph.h"   /* defines the redirected RCC/GPIO/TIM storage */

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
                     else printf("ok   %s\n", #c); }while(0)

/* ---- the recorder ------------------------------------------------------- */
/* Every pin this firmware can drive, indexed [port][pin]; -1 = never written. */
static int  g_level[2][16];
static long g_writes;

/* A write log, so order can be asserted rather than inferred. */
#define LOGMAX 4096
static struct { int port, pin, high; } g_log[LOGMAX];
static int g_logn;

static int port_idx(GPIO_Type *g) { return (g == GPIOB) ? 1 : 0; }

void gpio_set(pin_t p, int high)
{
    int po = port_idx(p.port);
    if (p.pin < 16) g_level[po][p.pin] = high ? 1 : 0;
    if (g_logn < LOGMAX) { g_log[g_logn].port = po; g_log[g_logn].pin = p.pin;
                           g_log[g_logn].high = high ? 1 : 0; g_logn++; }
    g_writes++;
}

/* The real one, so MODER can be asserted: a pin the firmware leaves as an
 * input is one the board decides, which is not a state. */
static int g_mode[2][16];
void gpio_mode(pin_t p, gpio_mode_t m)
{
    if (p.pin < 16) g_mode[port_idx(p.port)][p.pin] = (int)m;
}
int  gpio_get(pin_t p)                 { (void)p; return 0; }
void gpio_pull(pin_t p, int pull)      { (void)p; (void)pull; }
void gpio_od(pin_t p, int od)          { (void)p; (void)od; }
void gpio_af(pin_t p, uint8_t af)      { (void)p; (void)af; }

static uint32_t g_us, g_ms;
void delay_us(uint32_t us) { g_us += us; g_ms = g_us / 1000u; }
void delay_ms(uint32_t ms) { g_us += ms * 1000u; g_ms = g_us / 1000u; }
uint32_t millis(void)      { return g_ms; }
void wdt_kick(void)        {}

static void reset_log(void) { g_logn = 0; g_writes = 0; }

/* Replay the log and report how often a coil had BOTH ends high at once.
 * A coil is (A1,A2) or (B1,B2); the state is evaluated after every single
 * write, because the hazard is the window BETWEEN two writes. */
static int both_ends_driven(int pin_lo, int pin_hi)
{
    int a = 0, b = 0, hits = 0;
    for (int i = 0; i < g_logn; i++) {
        if (g_log[i].port != 1) continue;              /* motor pins are port B */
        if (g_log[i].pin == pin_lo) a = g_log[i].high;
        else if (g_log[i].pin == pin_hi) b = g_log[i].high;
        else continue;
        if (a && b) hits++;
    }
    return hits;
}

int main(void)
{
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 16; i++) g_level[p][i] = -1;

    /* 1) motor_init() drives every phase pin, and leaves no coil energised.
     *
     *    It sets the four pins to output and does NOT write their levels. That
     *    is deliberate and correct rather than an oversight: RM0091 8.4.1 gives
     *    GPIOB ODR a reset value of 0, so switching a phase pin to output drives
     *    it low, which is de-energised. Writing a level first would be writing
     *    the value that is already there. The assertion is therefore about the
     *    property that matters - outputs, and nothing energised - not about the
     *    number of register writes it took to get there. */
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 16; i++) g_mode[p][i] = -1;
    motor_init();
    CHECK(g_mode[1][PIN_MOTOR_A1.pin] == GPIO_OUT && g_mode[1][PIN_MOTOR_A2.pin] == GPIO_OUT);
    CHECK(g_mode[1][PIN_MOTOR_B1.pin] == GPIO_OUT && g_mode[1][PIN_MOTOR_B2.pin] == GPIO_OUT);
    CHECK(g_level[1][PIN_MOTOR_A1.pin] <= 0 && g_level[1][PIN_MOTOR_A2.pin] <= 0
       && g_level[1][PIN_MOTOR_B1.pin] <= 0 && g_level[1][PIN_MOTOR_B2.pin] <= 0);

    /* 2) BREAK BEFORE MAKE. Over a long feed no coil may ever be observed with
     *    both ends high after any single write. Before the fix this reported
     *    25 + 25 over 100 steps - one winding shorted on every second step. */
    reset_log();
    motor_step_lines(100);
    {
        int a = both_ends_driven(PIN_MOTOR_A1.pin, PIN_MOTOR_A2.pin);
        int b = both_ends_driven(PIN_MOTOR_B1.pin, PIN_MOTOR_B2.pin);
        if (a || b) printf("     coil A both ends: %d, coil B both ends: %d\n", a, b);
        CHECK(a == 0);
        CHECK(b == 0);
    }

    /* 3) The feed actually stepped: 100 lines at MOTOR_STEPS_PER_LINE steps
     *    each, so the log is non-trivial and case 2 is not vacuous. */
    CHECK(g_writes >= 100);

    /* 4) A single line advances once, and crediting elapsed time does not skip
     *    the step itself - only its settle delay. */
    reset_log();
    motor_step_line_after(0);
    {
        int n0 = g_writes;
        reset_log();
        motor_step_line_after(100000);     /* far more than one step period */
        CHECK(g_writes == n0);             /* same pin activity either way */
    }

    /* 5) motor_enable(0) de-energises every phase. A coil left powered after a
     *    job is heat in the motor and paper the operator cannot pull. */
    reset_log();
    motor_enable(0);
    CHECK(g_level[1][PIN_MOTOR_A1.pin] == 0 && g_level[1][PIN_MOTOR_A2.pin] == 0);
    CHECK(g_level[1][PIN_MOTOR_B1.pin] == 0 && g_level[1][PIN_MOTOR_B2.pin] == 0);

    /* 6) motor_idle_tick() holds torque during a job and drops it afterwards.
     *    Cutting it between the USB packets of one label would let the paper
     *    creep; never cutting it leaves the coils powered forever. */
    motor_step_lines(1);
    {
        int before = g_level[1][PIN_MOTOR_A1.pin] + g_level[1][PIN_MOTOR_A2.pin]
                   + g_level[1][PIN_MOTOR_B1.pin] + g_level[1][PIN_MOTOR_B2.pin];
        motor_idle_tick(1000000u);         /* nowhere near idle yet */
        int held = g_level[1][PIN_MOTOR_A1.pin] + g_level[1][PIN_MOTOR_A2.pin]
                 + g_level[1][PIN_MOTOR_B1.pin] + g_level[1][PIN_MOTOR_B2.pin];
        CHECK(held == before);             /* still energised */
        delay_ms(2000);
        motor_idle_tick(1000u);            /* now past the idle window */
        CHECK(g_level[1][PIN_MOTOR_A1.pin] == 0 && g_level[1][PIN_MOTOR_A2.pin] == 0
           && g_level[1][PIN_MOTOR_B1.pin] == 0 && g_level[1][PIN_MOTOR_B2.pin] == 0);
    }

    printf(fails ? "\n%d test(s) FAILED\n" : "\nALL MOTOR CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
