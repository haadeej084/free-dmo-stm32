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
#endif
