/* OpenDMOfw - head temperature: measurement + limiting. */
#ifndef OP57_THERMAL_H
#define OP57_THERMAL_H
#include <stdint.h>
void     thermal_init(void);
uint16_t thermal_read_raw(void);     /* raw ADC value of the head thermistor */
int      thermal_ok(void);           /* 1 = below the safety limit */
/* 1 = the thermistor reading is not believable (open circuit, short, or no
 * divider fitted). Distinct from thermal_ok(): that answers "is the head too
 * hot", this answers "do we know anything about the head at all". */
int      thermal_sensor_fault(void);
/* Dwell scale (256 = 1.0). Colder -> longer strobe for the same black level. */
uint16_t thermal_dwell_scale(void);
/* Sample EVERY ADC-capable pin into out[10]: IN0..IN7 = PA0..PA7, IN8 = PB0,
 * IN9 = PB1. Each pin is switched to analog only for its own conversion and
 * put straight back. This is how the GS D scan lets an operator FIND the
 * thermistor (warm the head, diff two scans) instead of tracing it. */
void thermal_scan_adc(uint16_t out[10]);
#endif
