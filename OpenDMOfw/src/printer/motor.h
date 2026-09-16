/* OpenDMOfw - feed stepper. */
#ifndef OP57_MOTOR_H
#define OP57_MOTOR_H
#include <stdint.h>
void motor_init(void);
void motor_enable(int on);
void motor_step_lines(uint16_t lines);   /* feed n dot lines of paper through */
/* One dot line, crediting `elapsed_us` that the caller already spent (the head
 * strobe). Lets the step settle time overlap the heat pulse instead of being
 * added to it - the difference between meeting DYMO's rated line rate and
 * missing it by a factor of two. */
void motor_step_line_after(uint32_t elapsed_us);
/* Call from the main loop: de-energises the motor once it has been idle for
 * idle_ms. Keeps holding torque for the duration of a job (so the paper cannot
 * creep between raster packets) without leaving the coils powered forever. */
void motor_idle_tick(uint32_t idle_ms);
#endif
