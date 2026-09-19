/* OpenDMOfw - startup + vector table (in C, no assembly needed).
 * Copies .data, zeroes .bss, calls SystemInit + main. */
#include <stdint.h>
#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "system.h"   /* BOOT_MAGIC */

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
void SystemInit(void);

/* USB DFU entry. sys_enter_bootloader() (system.c) stores BOOT_MAGIC here and
 * resets; .noinit is neither copied nor zeroed, so the value survives the
 * system reset, and Reset_Handler hands over to ST's boot loader before it
 * touches anything else - in particular before the independent watchdog is
 * started, which once running could not be stopped and would reset the part
 * out of the boot loader after ~4 s. Clearing the magic first means a boot
 * loader "leave" (dfu-util :leave) returns to this application normally. */
__attribute__((section(".noinit"))) uint32_t g_boot_request;

__attribute__((naked, noreturn)) static void jump_with_msp(uint32_t sp, uint32_t pc)
{
    __asm volatile("msr msp, r0" ::: "memory");
    __asm volatile("bx r1" ::: "memory");
}

/* ---- Fault safe state ---------------------------------------------------
 *
 * Every fault and every unused vector lands here. The old handler was
 * `for(;;){}`, which is correct for a CPU and wrong for a thermal printer: the
 * GPIO output latches keep whatever they held when the fault was taken. If the
 * fault arrives inside head_print_line(), that is a heat strobe asserted and
 * the 24 V rail enabled, and they stay that way until the independent watchdog
 * resets the part - 3.2 s at the datasheet's maximum LSI, 5.3 s at its minimum
 * (PR=5, RLR=1250; f072 datasheet Table 45). Measured on the real image in
 * Renode: PC in this handler, PIN_HEAD_VH still at HEAD_VH_ON_LEVEL and a
 * strobe still at MODEL_STB_ACTIVE_LEVEL, with both ports still driven.
 *
 * What that costs the head, from the ROHM KF3002-GD31A characteristics table:
 * 0.42 W per dot at a rated pulse width of 0.308 ms is 0.129 mJ per dot per
 * pulse. Holding one strobe for 3.2 s puts 1.34 J into each of that half's
 * dots - about 10,400x the rated pulse energy - and the whole half at once is
 * 141 W on OP57 (336 dots) and 262 W on OP104 (624 dots) continuous, against a
 * supply DYMO sizes for "an average of 37% of the total dots per line". The
 * head is the one part of this printer that cannot be replaced, so it gets the
 * deterministic ending rather than the arbitrary one.
 *
 * ORDER IS LOAD-BEARING, twice over:
 *  - Level before direction. Writing MODER first would drive whatever the ODR
 *    latch happened to hold, which after a cold reset is 0 - and 0 is the
 *    firing level for both the strobes and the VH gate.
 *  - Strobes before the rail. With the head's own drivers already off, the
 *    rail transition cannot put current through a dot.
 *
 * The RCC write is for the cold case: a fault before SystemInit() leaves the
 * port clocks gated, and a write to a gated port is silently discarded.
 *
 * RESIDUAL RISK, stated plainly: in that cold case reset had left PA8 a
 * floating input (RM0091 8.4.1), and this handler actively drives it to
 * !HEAD_VH_ON_LEVEL. If the ASSUMED gate polarity is inverted, the handler
 * switches the rail ON in a window that was previously safe by default. The
 * mitigation is the ordering above - the strobes are already at
 * !MODEL_STB_ACTIVE_LEVEL, so the head draws nothing - and that mitigation in
 * turn assumes the strobe polarity. Both polarities inverted is the single
 * case where this handler creates the hazard instead of removing it, which is
 * why FIELDWORK measurement 5 confirms the PA8 gate polarity and the STB
 * polarity together, as one prerequisite, before the first 24 V test.
 *
 * Finally the watchdog is started here. If the fault predates main()'s
 * wdt_init() - a hang in SystemInit()'s clock-ready spin, say - nothing was
 * ever going to reset the part, and the safe state would have been permanent
 * rather than temporary. Starting it costs three register writes and turns
 * every fault into a reboot. It cannot be stopped again, which is exactly what
 * is wanted from here. */
static inline void safe_out(GPIO_Type *g, unsigned pin, unsigned level)
{
    g->BSRR  = level ? (1u << pin) : (1u << (pin + 16));        /* level  */
    g->MODER = (g->MODER & ~(3u << (pin * 2))) | (1u << (pin * 2)); /* output */
}

__attribute__((noreturn)) void Fault_Handler(void);
void Fault_Handler(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN;

    /* 1. Heat strobes off - the fitted ones for this model. */
    {
        const pin_t stb[HEAD_STROBE_SEGMENTS] = {
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
        for (unsigned i = 0; i < HEAD_STROBE_SEGMENTS; i++)
            safe_out(stb[i].port, stb[i].pin, !MODEL_STB_ACTIVE_LEVEL);
    }

    /* 2. Latch to HOLD, shift lines idle - same levels head_reset() uses. */
    safe_out(PIN_HEAD_LATCH.port, PIN_HEAD_LATCH.pin, 1);
    safe_out(PIN_HEAD_CLK.port,   PIN_HEAD_CLK.pin,   0);
    safe_out(PIN_HEAD_DI1.port,   PIN_HEAD_DI1.pin,   0);
#if MODEL_HEAD_SHIFT_LINES != 1
    /* With one data line the DI2 pad may be the head's own DO1 output; it is
     * an input then (head_init) and must not be driven here either (D39). */
    safe_out(PIN_HEAD_DI2.port,   PIN_HEAD_DI2.pin,   0);
#endif

    /* 3. Only now the 24 V rail. */
    safe_out(PIN_HEAD_VH.port, PIN_HEAD_VH.pin, !HEAD_VH_ON_LEVEL);

    /* 4. Motor phases de-energised, so a stalled coil is not held at DC. */
    safe_out(PIN_MOTOR_A1.port, PIN_MOTOR_A1.pin, 0);
    safe_out(PIN_MOTOR_A2.port, PIN_MOTOR_A2.pin, 0);
    safe_out(PIN_MOTOR_B1.port, PIN_MOTOR_B1.pin, 0);
    safe_out(PIN_MOTOR_B2.port, PIN_MOTOR_B2.pin, 0);
#if MOTOR_DRIVE == MOTOR_DRIVE_STEPDIR
    safe_out(PIN_MOTOR_ENABLE.port, PIN_MOTOR_ENABLE.pin, 1);   /* active-low: driver off */
#ifdef PIN_MOTOR_SLEEP
    safe_out(PIN_MOTOR_SLEEP.port,  PIN_MOTOR_SLEEP.pin,  0);   /* SGM42630 nSLEEP low: outputs off */
#endif
#endif

    /* 5. Guarantee the reboot even if the fault predates wdt_init(). */
    IWDG->KR  = 0x5555;
    IWDG->PR  = 5;
    IWDG->RLR = 1250;
    IWDG->KR  = 0xAAAA;
    IWDG->KR  = 0xCCCC;

    for (;;) {}
}

/* Handlers - weak defaults; the real ones are in usb_core.c (USB) and
 * system.c (SysTick). TIM3 free-runs as the delay_us time base and raises no
 * interrupt, so it keeps the default handler. */
void Default_Handler(void) { Fault_Handler(); }
void USB_IRQHandler(void)  __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

void Reset_Handler(void)
{
    if (g_boot_request == BOOT_MAGIC) {
        g_boot_request = 0;
        RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
        SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE_Msk)
                      | SYSCFG_CFGR1_MEM_MODE_SYSMEM;
        jump_with_msp(((const uint32_t *)SYSMEM_BASE)[0],
                      ((const uint32_t *)SYSMEM_BASE)[1]);
    }

    /* The watchdog starts HERE, not in main(). SystemInit() contains four
     * unbounded spins waiting on clock-ready flags - HSERDY, PLLRDY, the SW/SWS
     * handshake and HSI48RDY - and a dead crystal or a PLL that never locks
     * hangs in one of them forever. That is a HANG, not a fault, so the fault
     * handler cannot see it, and until now nothing else could either: the
     * device would sit there drawing power with no LED, no USB and no reset,
     * looking exactly like a dead board.
     *
     * The IWDG is clocked from the LSI, which is independent of everything
     * SystemInit() is waiting for, so it keeps counting even if the system
     * clock never arrives. ~4 s later the part resets and tries again.
     *
     * It must come AFTER the DFU hand-over above and not before: once started
     * the IWDG cannot be stopped, and it would reset the part out of ST's boot
     * loader mid-download. That ordering is the whole reason this is here
     * rather than at the top of the function. */
    wdt_init();

    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0;
    SystemInit();
    main();
    for (;;) {}
}

/* Cortex-M0 vector table: 16 system + 32 IRQ (RM0091). Only the used ones are
 * named; the rest point to Default_Handler. */
typedef void (*vec_t)(void);
__attribute__((section(".isr_vector"), used))
const vec_t g_vectors[16 + 32] = {
    (vec_t)&_estack,      /* 0  initial SP     */
    Reset_Handler,        /* 1  reset          */
    Fault_Handler,        /* 2  NMI            */
    Fault_Handler,        /* 3  HardFault      */
    0,0,0,0,0,0,0,        /* 4..10 reserved    */
    Default_Handler,      /* 11 SVCall         */
    0,0,                  /* 12,13 reserved    */
    Default_Handler,      /* 14 PendSV         */
    SysTick_Handler,      /* 15 SysTick        */
    /* --- external IRQ 0..31 --- */
    Default_Handler,      /* 0  WWDG           */
    Default_Handler,      /* 1  PVD_VDDIO2     */
    Default_Handler,      /* 2  RTC            */
    Default_Handler,      /* 3  FLASH          */
    Default_Handler,      /* 4  RCC_CRS        */
    Default_Handler,      /* 5  EXTI0_1        */
    Default_Handler,      /* 6  EXTI2_3        */
    Default_Handler,      /* 7  EXTI4_15       */
    Default_Handler,      /* 8  TSC            */
    Default_Handler,      /* 9  DMA_CH1        */
    Default_Handler,      /* 10 DMA_CH2_3      */
    Default_Handler,      /* 11 DMA_CH4_5_6_7  */
    Default_Handler,      /* 12 ADC_COMP       */
    Default_Handler,      /* 13 TIM1_BRK_UP    */
    Default_Handler,      /* 14 TIM1_CC        */
    Default_Handler,      /* 15 TIM2           */
    Default_Handler,      /* 16 TIM3 (free-running, no IRQ) */
    Default_Handler,      /* 17 TIM6_DAC       */
    Default_Handler,      /* 18 TIM7           */
    Default_Handler,      /* 19 TIM14          */
    Default_Handler,      /* 20 TIM15          */
    Default_Handler,      /* 21 TIM16          */
    Default_Handler,      /* 22 TIM17          */
    Default_Handler,      /* 23 I2C1           */
    Default_Handler,      /* 24 I2C2           */
    Default_Handler,      /* 25 SPI1           */
    Default_Handler,      /* 26 SPI2           */
    Default_Handler,      /* 27 USART1         */
    Default_Handler,      /* 28 USART2         */
    Default_Handler,      /* 29 USART3_4       */
    Default_Handler,      /* 30 CEC_CAN        */
    USB_IRQHandler,       /* 31 USB            */
};
