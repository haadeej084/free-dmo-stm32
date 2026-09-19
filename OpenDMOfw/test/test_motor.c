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
 * Since D43 the default MOTOR_DRIVE is STEP/DIR (U2 = SGM42630). The same
 * harness then asserts the driver-IC contract instead of the coil one: nENABLE
 * goes low BEFORE the first STEP rising edge, nSLEEP (when routed) goes high
 * at least tWAKE = 1 ms before it, every STEP pulse is >= 1 us high and low,
 * the feed is exactly one rising edge per line, and enable/idle drop the
 * driver. The 4-phase branch is kept for a board with a plain H-bridge.
 *
 * Build (see the Makefile `test` target):
 *   cc -DOPENDMO_HOST_TEST -DMODEL_OP57 -Isrc test/test_motor.c src/printer/motor.c
 *   ... and once more with -DPIN_MOTOR_SLEEP='((pin_t){GPIOB,11})'
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
static struct { int port, pin, high; uint32_t us; } g_log[LOGMAX];
static int g_logn;
static uint32_t g_us, g_ms;

static int port_idx(GPIO_Type *g) { return (g == GPIOB) ? 1 : 0; }

void gpio_set(pin_t p, int high)
{
    int po = port_idx(p.port);
    if (p.pin < 16) g_level[po][p.pin] = high ? 1 : 0;
    if (g_logn < LOGMAX) { g_log[g_logn].port = po; g_log[g_logn].pin = p.pin;
                           g_log[g_logn].high = high ? 1 : 0; g_log[g_logn].us = g_us;
                           g_logn++; }
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

void delay_us(uint32_t us) { g_us += us; g_ms = g_us / 1000u; }
void delay_ms(uint32_t ms) { g_us += ms * 1000u; g_ms = g_us / 1000u; }
uint32_t millis(void)      { return g_ms; }
void wdt_kick(void)        {}

static void reset_log(void) { g_logn = 0; g_writes = 0; }

/* Replay the log and report how often a coil had BOTH ends high at once.
 * A coil is (A1,A2) or (B1,B2); the state is evaluated after every single
 * write, because the hazard is the window BETWEEN two writes. */
/* How many STEPS the log contains. A step is a change of the four-bit phase
 * vector - not a pin write, because break-before-make spends two writes on some
 * pins and none on others. Counting the vector makes the measure independent of
 * how the driver chooses to get there. */
#if MOTOR_DRIVE == MOTOR_DRIVE_4PHASE
static int step_events(void)
{
    /* The four-phase sequence, as motor.c drives it. A STEP is the vector
     * arriving at the next entry - not any change of it, because
     * break-before-make writes the pins one at a time and the vector passes
     * through intermediate values inside a single step. Matching the sequence
     * ignores those, and it also checks the ORDER: a driver that advanced the
     * phase backwards would feed the paper the wrong way and count zero here. */
    /* k_phase[] as {A1,A2,B1,B2}, packed here the same way: {1,0,1,0} -> 0xA,
     * {0,1,1,0} -> 0x6, {0,1,0,1} -> 0x5, {1,0,0,1} -> 0x9. The phase index at
     * entry depends on what ran before, so the sequence is joined wherever it
     * starts rather than assumed - what is asserted is that it then ADVANCES,
     * in order, once per step. */
    static const int seq[4] = { 0xA, 0x6, 0x5, 0x9 };
    int lvl[16], steps = 0, want = -1;
    for (int i = 0; i < 16; i++) lvl[i] = 0;
    for (int i = 0; i < g_logn; i++) {
        if (g_log[i].port != 1) continue;
        lvl[g_log[i].pin] = g_log[i].high;
        int v = (lvl[PIN_MOTOR_A1.pin] << 3) | (lvl[PIN_MOTOR_A2.pin] << 2)
              | (lvl[PIN_MOTOR_B1.pin] << 1) |  lvl[PIN_MOTOR_B2.pin];
        if (want < 0) {
            for (int k = 0; k < 4; k++)
                if (v == seq[k]) { steps = 1; want = (k + 1) & 3; break; }
            continue;
        }
        if (v == seq[want]) { steps++; want = (want + 1) & 3; }
    }
    return steps;
}

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
#endif /* MOTOR_DRIVE_4PHASE */


#if MOTOR_DRIVE == MOTOR_DRIVE_STEPDIR
static int is_pin(int i, pin_t p)
{
    return g_log[i].port == port_idx(p.port) && g_log[i].pin == p.pin;
}

/* STEP rising edges in the log: what the SGM42630 indexer actually counts. */
static int step_rises(void)
{
    int lvl = 0, n = 0;
    for (int i = 0; i < g_logn; i++) {
        if (!is_pin(i, PIN_MOTOR_STEP)) continue;
        if (g_log[i].high && !lvl) n++;
        lvl = g_log[i].high;
    }
    return n;
}

/* Shortest STEP high time and low time seen, in us (datasheet min 1 us each). */
static void step_widths(uint32_t *min_high, uint32_t *min_low)
{
    int lvl = 0, seen = 0; uint32_t t_edge = 0;
    *min_high = *min_low = 0xFFFFFFFFu;
    for (int i = 0; i < g_logn; i++) {
        if (!is_pin(i, PIN_MOTOR_STEP)) continue;
        if (g_log[i].high == lvl) continue;
        if (seen) {
            uint32_t w = g_log[i].us - t_edge;
            if (lvl && w < *min_high) *min_high = w;
            if (!lvl && w < *min_low) *min_low = w;
        }
        lvl = g_log[i].high; t_edge = g_log[i].us; seen = 1;
    }
}

/* Index of the first STEP rising edge, or -1. */
static int first_step_rise(void)
{
    int lvl = 0;
    for (int i = 0; i < g_logn; i++) {
        if (!is_pin(i, PIN_MOTOR_STEP)) continue;
        if (g_log[i].high && !lvl) return i;
        lvl = g_log[i].high;
    }
    return -1;
}

/* Level of the last write to `p` BEFORE log index `before` (-1 = none), and
 * when it happened. */
static int level_before(pin_t p, int before, uint32_t *at_us)
{
    int lv = -1;
    for (int i = 0; i < before && i < g_logn; i++)
        if (is_pin(i, p)) { lv = g_log[i].high; if (at_us) *at_us = g_log[i].us; }
    return lv;
}
#define LVL(p)  g_level[port_idx((p).port)][(p).pin]
#define MODE(p) g_mode [port_idx((p).port)][(p).pin]
#endif

int main(void)
{
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 16; i++) g_level[p][i] = -1;

#if MOTOR_DRIVE == MOTOR_DRIVE_STEPDIR
    /* 1) motor_init(): STEP, DIR and nENABLE become outputs; the driver is left
     *    DISABLED (nENABLE high) and STEP low, so nothing moves before a job.
     *    With nSLEEP routed, it is written LOW before the pin becomes an output
     *    (level first, then mode - the D31 rule for the VH gate). */
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 16; i++) g_mode[p][i] = -1;
    reset_log();
    motor_init();
    CHECK(MODE(PIN_MOTOR_STEP) == GPIO_OUT);
    CHECK(MODE(PIN_MOTOR_DIR) == GPIO_OUT);
    CHECK(MODE(PIN_MOTOR_ENABLE) == GPIO_OUT);
    CHECK(LVL(PIN_MOTOR_ENABLE) == 1);            /* active-low: driver off */
    CHECK(LVL(PIN_MOTOR_STEP) == 0);
    CHECK(LVL(PIN_MOTOR_DIR) == 1);               /* forward */
#ifdef PIN_MOTOR_SLEEP
    CHECK(MODE(PIN_MOTOR_SLEEP) == GPIO_OUT);
    CHECK(LVL(PIN_MOTOR_SLEEP) == 0);             /* asleep until the first feed */
#endif

    /* 2) THE DRIVER CONTRACT. Over a feed: nENABLE is low before the first STEP
     *    rising edge (tnENABLE = 20 us is covered by the step period), nSLEEP -
     *    when routed - is high at least tWAKE = 1 ms before it, and no STEP
     *    pulse is narrower than the 1 us the SGM42630 needs high or low. */
    reset_log();
    motor_step_lines(100);
    {
        int fr = first_step_rise();
        CHECK(fr > 0);
        uint32_t t_en = 0, t_sl = 0;
        CHECK(level_before(PIN_MOTOR_ENABLE, fr, &t_en) == 0);
        (void)t_en;
#ifdef PIN_MOTOR_SLEEP
        CHECK(level_before(PIN_MOTOR_SLEEP, fr, &t_sl) == 1);
        CHECK(fr > 0 && g_log[fr].us - t_sl >= 1000u);   /* tWAKE */
#else
        (void)t_sl;
#endif
        uint32_t mh, ml;
        step_widths(&mh, &ml);
        CHECK(mh >= 1u && ml >= 1u);
    }

    /* 3) THE FEED DISTANCE, exactly: one rising edge per line. The literal 100
     *    is deliberate (D34) - MOTOR_STEPS_PER_LINE is the constant under test.
     *    With the SGM42630 it can only be 1, 2, 4 or 8 times the full steps
     *    per line; FIELDWORK measurement 2 settles which. */
    CHECK(step_rises() == 100);
    CHECK(g_writes >= 200);          /* each pulse is two writes */

    /* 4) A single line advances once, and crediting elapsed time does not skip
     *    the pulse itself - only its settle delay. */
    reset_log();
    motor_step_line_after(0);
    {
        int n0 = g_writes;
        reset_log();
        motor_step_line_after(100000);
        CHECK(g_writes == n0);
        CHECK(step_rises() == 1);
    }

    /* 5) motor_enable(0) disables the driver (and sleeps it, if routed). */
    reset_log();
    motor_enable(0);
    CHECK(LVL(PIN_MOTOR_ENABLE) == 1);
#ifdef PIN_MOTOR_SLEEP
    CHECK(LVL(PIN_MOTOR_SLEEP) == 0);
#endif

    /* 6) motor_idle_tick() holds the driver enabled during a job and drops it
     *    afterwards. */
    motor_step_lines(1);
    CHECK(LVL(PIN_MOTOR_ENABLE) == 0);
    motor_idle_tick(1000000u);
    CHECK(LVL(PIN_MOTOR_ENABLE) == 0);            /* still on */
    delay_ms(2000);
    motor_idle_tick(1000u);
    CHECK(LVL(PIN_MOTOR_ENABLE) == 1);            /* dropped */
#ifdef PIN_MOTOR_SLEEP
    CHECK(LVL(PIN_MOTOR_SLEEP) == 0);
#endif
#else /* MOTOR_DRIVE_4PHASE */
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

    /* 3) THE FEED DISTANCE, exactly. This used to be `g_writes >= 100` for a
     *    100-line feed - a bound so loose that MOTOR_STEPS_PER_LINE was untested
     *    by construction, and it is the constant that sets how long every label
     *    is. DECISIONS D24 now pins it to 1 from DYMO's own 450 firmware.
     *
     *    The expectation is the literal 100, deliberately NOT lines *
     *    MOTOR_STEPS_PER_LINE: a test that recomputes the macro would agree with
     *    whatever the macro said (D34). Change the constant and this fails,
     *    which is the entire point. */
    CHECK(step_events() == 100);
    CHECK(g_writes >= 100);          /* and the log is non-trivial */

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

#endif

    printf(fails ? "\n%d test(s) FAILED\n" : "\nALL MOTOR CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
