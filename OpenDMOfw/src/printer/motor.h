/* OpenDMOfw - feed stepper. */
#ifndef OP57_MOTOR_H
#define OP57_MOTOR_H
#include <stdint.h>
void motor_init(void);
void motor_enable(int on);
void motor_step_lines(uint16_t lines);   /* feed n dot lines of paper through */
#endif
