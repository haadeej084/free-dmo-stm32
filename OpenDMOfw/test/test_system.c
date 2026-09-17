/* OpenDMOfw - host tests for src/system.c.
 *
 * system.c is compiled by no host suite and reached by no Renode assertion, and
 * a mutation run put a number on what that costs: 3 of 33 real mutants killed,
 * 9.1 % - the worst in the tree. The concrete consequence is worse than the
 * number. The guard that stops GS D 0x07 from driving the 24 V heat gate and
 * the heat strobes from a host command:
 *
 *     if (pin_is_head_hot(g, pin)) return 0;        // src/system.c
 *
 * could be DELETED and every suite in the project stayed green - make test on
 * both models, all five Renode scripts, make stack. The reason is exact:
 * test_protocol.c does not mock sys_pin_toggle(), it RE-IMPLEMENTS it, and
 * re-implements a different guard (it refuses PA11-PA14 and omits the hot-pin
 * check entirely). Scenario 43 is therefore answered by the test's own copy and
 * never reaches this file at all.
 *
 * That guard is a cycle-1 safety fix. This harness compiles the real thing.
 *
 * Build (see the Makefile `test` target):
 *   cc -DOPENDMO_HOST_TEST -DMODEL_OP57 -Isrc -Itest test/test_system.c src/system.c
 */
#include <stdio.h>
#include <stdint.h>
#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "system.h"
#include "host_periph.h"

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
                     else printf("ok   %s\n", #c); }while(0)

/* system.c's own delay/watchdog helpers are in the file under test; only the
 * ones it calls out to are stubbed. */
void head_vh_off(void) {}
/* startup.c owns this in the firmware; the DFU hand-over is covered by
 * test/renode/dfu.py on the real image. */
uint32_t g_boot_request;

/* n = 0 throughout: delay_ms() lives in system.c and spins on a millisecond
 * counter that only SysTick advances, which no host build does. Both things
 * this harness cares about - the refusal list and the save/restore - sit
 * OUTSIDE that loop, so zero pulses exercises them exactly. */
static int toggled(GPIO_Type *g, uint8_t pin, uint8_t n)
{
    /* A refused pin must be left completely alone: same MODER, same ODR. */
    uint32_t m0 = g->MODER, o0 = g->ODR;
    int r = sys_pin_toggle((uint8_t)(g == GPIOB ? 1 : 0), pin, n);
    if (!r) {
        if (g->MODER != m0 || g->ODR != o0) {
            printf("     refused pin %u was still touched (MODER %08x->%08x, ODR %08x->%08x)\n",
                   pin, (unsigned)m0, (unsigned)g->MODER, (unsigned)o0, (unsigned)g->ODR);
            return -1;
        }
    }
    return r;
}

int main(void)
{
    host_rcc.AHBENR = 0xFFFFFFFFu;        /* clocks on, as after SystemInit() */

    /* 1) THE HOT PINS ARE REFUSED. This is the assertion the tree did not have.
     *    PIN_HEAD_VH is the 24 V gate and this function has no polarity table,
     *    so "drive it low for a millisecond" is exactly how the rail is
     *    switched ON; a strobe would fire an unmetered pulse outside the
     *    thermal gate. */
    CHECK(toggled(PIN_HEAD_VH.port, PIN_HEAD_VH.pin, 0) == 0);
    CHECK(toggled(PIN_HEAD_STROBE.port, PIN_HEAD_STROBE.pin, 0) == 0);
#if HEAD_STROBE_SEGMENTS > 1
    CHECK(toggled(PIN_HEAD_STROBE2.port, PIN_HEAD_STROBE2.pin, 0) == 0);
#endif

    /* 2) The pins that carry the command itself are refused too: PA11/PA12 are
     *    USB D-/D+ and PA13/PA14 are SWD. Toggling those ends the session
     *    instead of answering the question. */
    CHECK(toggled(GPIOA, 11, 0) == 0);
    CHECK(toggled(GPIOA, 12, 0) == 0);
    CHECK(toggled(GPIOA, 13, 0) == 0);
    CHECK(toggled(GPIOA, 14, 0) == 0);

    /* 3) The SPARE strobes stay toggleable - finding them is the whole point of
     *    the command, so the guard must be a list and not a blanket. */
    CHECK(toggled(GPIOB, 2, 0) == 1);
    CHECK(toggled(GPIOB, 3, 0) == 1);

    /* 4) An ordinary pin is toggled, and comes back EXACTLY as it was. The old
     *    code left every toggled pin driving low, which on an output pin is not
     *    "untouched" - it is a permanent low. */
    {
        GPIO_Type *g = GPIOB;
        g->MODER = 0;                       /* pin 9: input */
        g->ODR   = 0;
        CHECK(sys_pin_toggle(1, 9, 0) == 1);
        CHECK(((g->MODER >> (9 * 2)) & 3u) == 0u);   /* input again */
        CHECK(((g->ODR >> 9) & 1u) == 0u);

        g->MODER = (1u << (9 * 2));         /* pin 9: output, driving HIGH */
        g->ODR   = (1u << 9);
        CHECK(sys_pin_toggle(1, 9, 0) == 1);
        CHECK(((g->MODER >> (9 * 2)) & 3u) == 1u);   /* still an output */
        CHECK(((g->ODR >> 9) & 1u) == 1u);           /* still high */
    }

    /* 5) Out-of-range arguments are refused rather than folded into something
     *    valid: a bad port index and a pin above 15. */
    CHECK(sys_pin_toggle(7, 0, 0) == 0);
    CHECK(sys_pin_toggle(0, 16, 0) == 0);
    CHECK(sys_pin_toggle(0, 255, 0) == 0);

    printf(fails ? "\n%d test(s) FAILED\n" : "\nALL SYSTEM CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
