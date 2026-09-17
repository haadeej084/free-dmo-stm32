/* OpenDMOfw - thermal head driver. */
#ifndef OP57_HEAD_H
#define OP57_HEAD_H
#include <stdint.h>
void head_init(void);
void head_reset(void);
/* Print one dot line: nbytes bytes, MSB = leftmost dot. Performs
 * shift -> latch -> strobe with a thermally limited dwell. */
void head_print_line(const uint8_t *bits, uint16_t nbytes);
void head_set_density(uint8_t d);      /* 0 = heat off; 1..16 base dwell */
/* The strobe time one segment gets, for a density (1..16) and a thermal scale
 * (256 = 1.0), including the energy ceiling. Exposed so the arithmetic can be
 * tested on the host without a head: see test/test_thermal.c. */
uint32_t head_dwell_us(uint8_t density, uint16_t thermal_scale);
/* The same, plus rail-sag compensation for the coverage of one strobe.
 * `dots` is the energised dot count of the segment, `dots_max` its full
 * width. Re-clamps to the energy ceiling after the addition. */
uint32_t head_dwell_sag_us(uint8_t density, uint16_t thermal_scale,
                           uint16_t dots, uint16_t dots_max);
/* Microseconds of strobe the last head_print_line() spent: the COMMANDED dwell
 * times HEAD_STROBE_SEGMENTS. The true energised time per segment differs by
 * the head driver's edge skew (see head.c). The feed step can subtract this
 * from its own settling wait instead of adding to it. */
uint32_t head_last_strobe_us(void);
/* Call from the main loop: drops the 24 V heat rail once the head has been
 * idle for idle_ms. The rail is off at boot and switched on only to print. */
void head_idle_tick(uint32_t idle_ms);
int  head_vh_is_on(void);    /* 1 = the 24 V heat rail is currently enabled */
void head_vh_off(void);      /* drop it now, whatever the idle timer says */
#endif
