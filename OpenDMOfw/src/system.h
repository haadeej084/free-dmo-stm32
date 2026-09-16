/* OpenDMOfw - system: clock, GPIO helpers, delays. */
#ifndef OP57_SYSTEM_H
#define OP57_SYSTEM_H

#include "mcu.h"
#include "pins.h"

#define SYSCLK_HZ 48000000u

void SystemInit(void);          /* clock -> 48 MHz HSI48 (SYSCLK), CRS trims on SOF */
void systick_init(void);        /* 1 ms tick for delays/timeouts */
void wdt_init(void);            /* IWDG ~4 s; must be kicked periodically */
void wdt_kick(void);            /* reload the watchdog */
uint32_t millis(void);
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);

/* GPIO */
typedef enum { GPIO_IN, GPIO_OUT, GPIO_AF, GPIO_ANALOG } gpio_mode_t;
void gpio_mode(pin_t p, gpio_mode_t m);
void gpio_af(pin_t p, uint8_t af);
void gpio_pull(pin_t p, int pull);      /* 0 none, 1 up, 2 down */
void gpio_od(pin_t p, int open_drain);
void gpio_set(pin_t p, int high);
int  gpio_get(pin_t p);

/* ---- Diagnostic pin access (GS D 0x06 / 0x07) ---------------------------
 * These exist so an operator can FIND a signal by driving candidates and
 * watching what responds, instead of tracing it on an unpowered board. Port
 * index: 0 = A, 1 = B, 2 = C. */
uint16_t sys_port_idr(uint8_t port);          /* input levels of a whole port */
/* Drive `pin` as an output and toggle it n times at ~1 ms per half period,
 * then restore its previous mode. Returns 0 (and does nothing) for an unknown
 * port, a pin above 15, or the four pins that would cut our own lifeline:
 * PA11/PA12 (USB) and PA13/PA14 (SWD). */
int      sys_pin_toggle(uint8_t port, uint8_t pin, uint8_t n);

#endif
