/* OpenDMO-FW - head temperature: measurement + limiting. */
#ifndef OP57_THERMAL_H
#define OP57_THERMAL_H
#include <stdint.h>
void     thermal_init(void);
uint16_t thermal_read_raw(void);     /* raw ADC value of the head thermistor */
int      thermal_ok(void);           /* 1 = below the safety limit */
/* Dwell scale (256 = 1.0). Colder -> longer strobe for the same black level. */
uint16_t thermal_dwell_scale(void);
#endif
