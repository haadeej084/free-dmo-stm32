/* OpenDMO-FW - clock/GPIO/delay implementation. */
#include "system.h"

static volatile uint32_t s_millis;

/* HSI48 as SYSCLK (48 MHz) + CRS autotrim on USB SOF. This gives a
 * USB-compliant clock without an external crystal (RM0091 RCC/CRS). On the
 * F072, CFGR.SW=11 selects HSI48 as the system clock (verified vs CMSIS). */
void SystemInit(void)
{
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY1;   /* 1 wait state for 48 MHz */

    /* HSI48 on: it is both the CPU clock and the USB clock. */
    RCC->CR2 |= RCC_CR2_HSI48ON;
    while (!(RCC->CR2 & RCC_CR2_HSI48RDY)) {}

    /* SYSCLK = HSI48 (SW=11). AHB/APB prescalers = 1 (reset default) -> 48 MHz. */
    RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_HSI48;
    while ((RCC->CFGR & RCC_CFGR_SWS_HSI48) != RCC_CFGR_SWS_HSI48) {}

    /* CRS on: sync source = USB SOF (default), autotrim + counter enable. */
    RCC->APB1ENR |= RCC_APB1ENR_CRSEN;
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    /* GPIO port clocks that we use. */
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |
                   RCC_AHBENR_GPIOCEN | RCC_AHBENR_GPIOFEN;
}

void systick_init(void)
{
    SysTick->LOAD = (SYSCLK_HZ / 1000u) - 1u;
    SysTick->VAL  = 0;
    SysTick->CTRL = 7;   /* CLKSOURCE=AHB | TICKINT | ENABLE */
}

void SysTick_Handler(void) { s_millis++; }

uint32_t millis(void) { return s_millis; }

/* IWDG on the LSI (~40 kHz). PR=5 -> /128 -> ~312 Hz; RLR=1250 -> ~4 s timeout.
 * The print loop kicks per dot line, so 4 s is plenty; if the firmware hangs
 * (e.g. a stuck sensor wait), the watchdog resets the device. */
void wdt_init(void)
{
    IWDG->KR  = 0x5555;      /* write access to PR/RLR */
    IWDG->PR  = 5;           /* /128 */
    IWDG->RLR = 1250;        /* ~4 s */
    IWDG->KR  = 0xAAAA;      /* reload */
    IWDG->KR  = 0xCCCC;      /* start the watchdog */
}
void wdt_kick(void) { IWDG->KR = 0xAAAA; }

void delay_ms(uint32_t ms)
{
    uint32_t t0 = s_millis;
    while ((s_millis - t0) < ms) { __asm volatile("wfi"); }
}

void delay_us(uint32_t us)
{
    /* Coarse busy-wait; 48 MHz => ~48 cycles/us. Sufficient for pulse widths. */
    volatile uint32_t n = us * 6u;
    while (n--) { __asm volatile("nop"); }
}

/* ---- GPIO --------------------------------------------------------------- */
void gpio_mode(pin_t p, gpio_mode_t m)
{
    uint32_t sh = p.pin * 2u;
    p.port->MODER = (p.port->MODER & ~(3u << sh)) | ((uint32_t)m << sh);
}

void gpio_af(pin_t p, uint8_t af)
{
    uint32_t idx = p.pin >> 3, sh = (p.pin & 7u) * 4u;
    p.port->AFR[idx] = (p.port->AFR[idx] & ~(0xFu << sh)) | ((uint32_t)af << sh);
    gpio_mode(p, GPIO_AF);
}

void gpio_pull(pin_t p, int pull)
{
    uint32_t sh = p.pin * 2u;
    p.port->PUPDR = (p.port->PUPDR & ~(3u << sh)) | ((uint32_t)(pull & 3) << sh);
}

void gpio_od(pin_t p, int od)
{
    if (od) p.port->OTYPER |=  (1u << p.pin);
    else    p.port->OTYPER &= ~(1u << p.pin);
}

void gpio_set(pin_t p, int high)
{
    p.port->BSRR = high ? (1u << p.pin) : (1u << (p.pin + 16u));
}

int gpio_get(pin_t p) { return (p.port->IDR >> p.pin) & 1u; }
