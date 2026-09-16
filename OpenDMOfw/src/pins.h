/* OpenDMOfw - CENTRAL PIN MAP.
 *
 * This is the only place where hardware pins are defined. The head INTERFACE
 * (which signals exist, latch polarity) is sourced from the ROHM KF3002 head
 * datasheet — see the head section below and PINMAP.md. The board-level pin
 * ROUTING (which MCU pin D.mo wired each signal to) is still an ASSUMPTION
 * (no board dump was used); adjust once you have measured the board.
 *
 * Notation: {port, pin number}. Port is a GPIO_Type* from mcu.h.
 *
 * PHYSICAL PIN MAP: the F072CB 48-pin package numbers (1-48, counter-
 * clockwise from the corner dimple) are documented in PINMAP.md, section
 * "F072CB 48-pin physical pin map" — extracted from datasheet Table 13, whose
 * LQFP48/UFQFPN48 column covers both packages the board may carry. Use
 * it to trace a GPIO name to the actual pad when probing the board. SWD flash
 * points: SWDIO = PA13 (pad 34), SWCLK = PA14 (pad 37); GND = any VSS pad
 * (23/35/47), power = VDD pad (24 or 48).
 */
#ifndef OP57_PINS_H
#define OP57_PINS_H

#include "mcu.h"
#include "model.h"   /* head geometry (HEAD_DOTS/HEAD_BYTES/HEAD_STROBE_SEGMENTS) per model */

typedef struct { GPIO_Type *port; uint8_t pin; } pin_t;

/* ---- Thermal head -------------------------------------------------------- *
 * The head is a ROHM KF3002-family module with BUILT-IN shift registers,
 * latch and heat drivers — the host only feeds serial data and fires strobes:
 *   57 mm (550 class): SHEC 3C56-9638 / GK11C308 / KF3002-GK11C (D.mo assembly
 *                      PRTA05412) — replacement-head listings for the 400/450
 *                      Turbo generation, which shares this head (57 mm, 672
 *                      dots, 300 dpi, per both tech references).
 *   105.7 mm (5XL class): ROHM TE3004-TP1W00A class — ROHM catalog SF2024 lists
 *                      exactly 1248 dots @ 300 dpi / 105.706 mm.
 * Interface per the ROHM KF3002-GL50A datasheet (equivalent circuit + pin
 * config + timing chart):
 *   CLK    serial clock (host -> head)
 *   DI1/DI2 one serial data line per shift-register half (host -> head)
 *   LAT    latch: High = HOLD, Low = THROUGH (active-low, sourced)
 *   STB1/STB2 heat strobe per half (host -> head; fired sequentially to split
 *            peak current). Low = fires the heat driver (active-low, sourced).
 *   TM     built-in NTC thermistor 30 kOhm B=3950 -> ADC (thermal.c)
 * There is NO MISO: DO1/DO2 are for daisy-chaining extra heads and stay open.
 * All signals are plain GPIO (bit-banged in head.c); no SPI peripheral needed. */
#define PIN_HEAD_CLK        ((pin_t){GPIOA, 5})   /* shift clock             */
#define PIN_HEAD_DI1        ((pin_t){GPIOA, 6})   /* shift data, half 1      */
#define PIN_HEAD_DI2        ((pin_t){GPIOA, 7})   /* shift data, half 2      */
/* 1 while CLK, DI1 and DI2 share one GPIO port (all GPIOA above). head.c then
 * shifts with two BSRR writes per dot instead of four, which is what keeps a
 * 1248-dot line inside the genuine per-line time budget. The preprocessor
 * cannot compare the ports itself: set this to 0 if you move one of the three
 * to another port, and head_init() traps a mismatch at run time. */
#define HEAD_SHIFT_SAME_PORT 1
#define PIN_HEAD_LATCH      ((pin_t){GPIOA, 4})   /* Low = THROUGH (sourced) */
#define PIN_HEAD_STROBE     ((pin_t){GPIOB, 0})   /* STB1: heat half 1       */
#define PIN_HEAD_STROBE2    ((pin_t){GPIOB, 1})   /* STB2: heat half 2       */
#define PIN_HEAD_STROBE3    ((pin_t){GPIOB, 2})   /* STB3: spare (wider heads) */
#define PIN_HEAD_STROBE4    ((pin_t){GPIOB, 3})   /* STB4: spare (wider heads) */
/* head.c uses the first HEAD_STROBE_SEGMENTS pins from this list.
 * HEAD_STROBE_SEGMENTS comes from model.h (both models = 2, per the two-half
 * KF3002 architecture). If the board shows more heat lines on the wide head,
 * raise it in model.h; enough PIN_HEAD_STROBE* are defined here. */
/* HEAD_DOTS / HEAD_BYTES / HEAD_STROBE_SEGMENTS come from model.h (per model).
 * OP104 (default): 1248 dots = two 624-dot halves -> STB1+STB2.
 * OP57 (-DMODEL_OP57): 672 dots = two 336-dot halves -> STB1+STB2. */

/* ---- Feed stepper -------------------------------------------------------- *
 * Two wiring variants are supported (choose via MOTOR_DRIVE in
 * motor.c): STEP/DIR to a driver IC, or direct 4-phase drive. */
#define PIN_MOTOR_STEP      ((pin_t){GPIOB, 4})
#define PIN_MOTOR_DIR       ((pin_t){GPIOB, 5})
#define PIN_MOTOR_ENABLE    ((pin_t){GPIOB, 10})  /* active-low enable (STEPDIR mode only; PB8 is I2C SCL) */
/* 4-phase fallback (only used when MOTOR_DRIVE == MOTOR_DRIVE_4PHASE): */
#define PIN_MOTOR_A1        ((pin_t){GPIOB, 4})
#define PIN_MOTOR_A2        ((pin_t){GPIOB, 5})
#define PIN_MOTOR_B1        ((pin_t){GPIOB, 6})
#define PIN_MOTOR_B2        ((pin_t){GPIOB, 7})

/* ---- Sensors ------------------------------------------------------------ */
#define PIN_PAPER_SENSE     ((pin_t){GPIOA, 0})   /* digital: paper present */
#define PAPER_PRESENT_LEVEL 0                       /* active-low               */
#define ADC_HEAD_TEMP_CH    1                       /* PA1 = ADC_IN1 (thermistor)*/

/* Head heat supply enable (P-MOS / load switch on the 24 V VH rail).
 * ASSUMED pin and polarity — LQFP48 has no spare Port-C pins for this.
 * P-MOS gate is typically active-low (Low = VH on). Verify on the board. */
#define PIN_HEAD_VH         ((pin_t){GPIOA, 8})
#define HEAD_VH_ON_LEVEL    0

/* ---- UI -----------------------------------------------------------------
 * The 48-pin F072CB bonds only PC13/PC14/PC15 on port C. PC6/PC7 exist on the
 * LQFP64 (F072RB) only — do not use them here. PA2/PA3 are unused in the
 * assumed head/motor map (pads 12/13). */
#define PIN_LED             ((pin_t){GPIOA, 2})
#define PIN_BUTTON          ((pin_t){GPIOA, 3})
#define BUTTON_PRESSED_LEVEL 0

/* ---- I2C config-EEPROM (24Cxx) ------------------------------------------ *
 * On the STM32F0 line I2C is AF2 (NOT AF1), and I2C1 exists ONLY on:
 *   SCL = PB6 or PB8,  SDA = PB7 or PB9
 * (datasheet DocID025004 Rev 2, Table 14 "STM32F072xx alternate function pin
 * description", Port B, AF2 row). Note PB12/PB14 are EVENTOUT and TIM15_CH1 at
 * AF1 (TIM1_BKIN / TIM1_CH2N at AF2, SPI2 at AF0) -- none of them is I2C.
 * Errata ES0223 2.2.1 (I2C analog filter): AF5 on PB9/PB10 and AF1 on PB14
 * misbehave while the I2C analog filter is enabled; nothing here uses those,
 * but check before remapping. We use the PB8/PB9 pair so it sits cleanly next to the
 * head-strobe (PB0-3) and motor (PB4-7) blocks with no pin conflict; PB6/PB7 is
 * the other valid pair.
 * CONFIRM by continuity on the board: trace the EEPROM SCL/SDA to whichever
 * pair D.mo used, then set these two macros + AF2 in store.c. */
/* The pins that can put heat into the head: the 24 V rail gate and the heat
 * strobes actually fitted on this model. The diagnostic pin-toggle (GS D 0x07)
 * refuses them - it drives a pin low for a millisecond at a time and cannot
 * know a pin's polarity, which on the active-low VH gate means switching the
 * rail on, and on a strobe means an unmetered heat pulse outside the thermal
 * gate. The rail and the head have their own commands (GS D 0x08, GS D 0x01).
 * The spare strobes (PB2/PB3) stay toggleable: finding them is the point. */
static inline int pin_is_head_hot(GPIO_Type *port, uint8_t pin)
{
    const pin_t hot[1 + HEAD_STROBE_SEGMENTS] = {
        PIN_HEAD_VH,
        PIN_HEAD_STROBE,
#if HEAD_STROBE_SEGMENTS > 1
        PIN_HEAD_STROBE2,
#endif
#if HEAD_STROBE_SEGMENTS > 2
        PIN_HEAD_STROBE3,
#endif
#if HEAD_STROBE_SEGMENTS > 3
        PIN_HEAD_STROBE4,
#endif
    };
    for (unsigned i = 0; i < sizeof hot / sizeof hot[0]; i++)
        if (hot[i].port == port && hot[i].pin == pin) return 1;
    return 0;
}

#define PIN_I2C_SCL         ((pin_t){GPIOB, 8})   /* I2C1_SCL AF2            */
#define PIN_I2C_SDA         ((pin_t){GPIOB, 9})   /* I2C1_SDA AF2            */
#define EEPROM_I2C_ADDR     0x50                    /* 7-bit                  */

#endif /* OP57_PINS_H */
