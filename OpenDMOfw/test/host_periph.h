/* OpenDMOfw - storage for the peripherals a host test build redirects.
 *
 * mcu.h, under OPENDMO_HOST_TEST, points RCC/GPIOA/GPIOB/GPIOC/TIM3 at these
 * objects so that firmware modules which touch them (usb_core.c's usb_init,
 * thermal.c's ADC setup, head.c's pin setup) link and run natively. They are
 * plain storage: a test that cares about a register reads or writes it
 * directly. The ADC is deliberately NOT here - it goes through host_adc(),
 * which each test defines itself, because a conversion is several accesses and
 * a test may want a different sample per conversion.
 *
 * Include once per test program (it defines, not declares).
 */
#ifndef OPENDMO_HOST_PERIPH_H
#define OPENDMO_HOST_PERIPH_H
#include "mcu.h"

RCC_Type  host_rcc;
GPIO_Type host_gpioa, host_gpiob, host_gpioc;
TIM_Type  host_tim3;

#endif
