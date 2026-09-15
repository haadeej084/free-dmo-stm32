/* OpenDMOfw - startup + vector table (in C, no assembly needed).
 * Copies .data, zeroes .bss, calls SystemInit + main. */
#include <stdint.h>
#include "mcu.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
void SystemInit(void);

/* Handlers - weak defaults; the real ones are in usb_core.c / motor.c. */
void Default_Handler(void) { for(;;){} }
void USB_IRQHandler(void)  __attribute__((weak, alias("Default_Handler")));
void TIM3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

void Reset_Handler(void)
{
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
    Default_Handler,      /* 2  NMI            */
    Default_Handler,      /* 3  HardFault      */
    0,0,0,0,0,0,0,        /* 4..10 reserved    */
    Default_Handler,      /* 11 SVCall         */
    0,0,                  /* 12,13 reserved    */
    Default_Handler,      /* 14 PendSV         */
    SysTick_Handler,      /* 15 SysTick        */
    /* --- externe IRQ 0..31 --- */
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
    TIM3_IRQHandler,      /* 16 TIM3           */
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
