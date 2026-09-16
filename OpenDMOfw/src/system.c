/* OpenDMOfw - clock/GPIO/delay implementation. */
#include "system.h"

static volatile uint32_t s_millis;

/* HSI48 as SYSCLK (48 MHz) + CRS autotrim on USB SOF. This gives a
 * USB-compliant clock without an external crystal (RM0091 RCC/CRS). On the
 * F072, CFGR.SW=11 selects HSI48 as the system clock (verified vs CMSIS). */
/* Clock source. Default is the crystal-less HSI48 + CRS path, because it needs
 * nothing from the board and therefore also works on the bare F072 a
 * fieldworker brings up first (FIELDWORK.md step A).
 *
 * The genuine Rev K mainboard does carry a crystal: a close-up of the board
 * shows "AXC12.00-115" in an HC-49 can at Y1, immediately beside the 48-pin
 * MCU, with its load capacitors. 12 MHz x PLL4 = exactly 48 MHz, which is a
 * better USB clock than a trimmed RC. Build with -DOPENDMO_CLOCK_HSE12=1 to use
 * it; if the crystal is absent or a different frequency, the HSERDY wait would
 * hang, so this is opt-in rather than auto-detected. */
#ifndef OPENDMO_CLOCK_HSE12
#define OPENDMO_CLOCK_HSE12 0
#endif

void SystemInit(void)
{
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY1;   /* 1 wait state for 48 MHz */

#if OPENDMO_CLOCK_HSE12
    /* SYSCLK and USB from the board's 12 MHz crystal via PLL x4. */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {}
    RCC->CFGR2 = 0;                                  /* PREDIV = /1 */
    RCC->CFGR = (RCC->CFGR & ~((3u<<15) | (0xFu<<18) | 0x3u))
              | RCC_CFGR_PLLSRC_HSE_PREDIV | RCC_CFGR_PLLMUL4;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & (3u<<2)) != RCC_CFGR_SWS_PLL) {}
    RCC->CFGR3 |= RCC_CFGR3_USBSW_PLL;               /* USB from PLL, not HSI48 */
#else
    /* HSI48 on: it is both the CPU clock and the USB clock. */
    RCC->CR2 |= RCC_CR2_HSI48ON;
    while (!(RCC->CR2 & RCC_CR2_HSI48RDY)) {}

    /* SYSCLK = HSI48 (SW=11). AHB/APB prescalers = 1 (reset default) -> 48 MHz. */
    RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_HSI48;
    while ((RCC->CFGR & RCC_CFGR_SWS_HSI48) != RCC_CFGR_SWS_HSI48) {}

    /* CRS: sync source = USB SOF (do not rely on reset default), autotrim.
     * RCC_CFGR3.USBSW is left at its reset value, which already selects HSI48
     * as the USB clock on the F072. */
    RCC->APB1ENR |= RCC_APB1ENR_CRSEN;
    CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC_Msk) | CRS_CFGR_SYNCSRC_USB;
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
#endif

    /* GPIO port clocks that we use. */
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |
                   RCC_AHBENR_GPIOCEN | RCC_AHBENR_GPIOFEN;
}

/* TIM3 free-runs at 1 MHz and is the time base for delay_us(). A calibrated
 * NOP loop (the previous implementation) drifts with compiler version and
 * optimisation level and, worse, is stretched by every interrupt that lands
 * inside it — which for a head strobe means extra heat energy per dot line.
 * Reading a hardware counter costs the same and cannot drift. */
static void us_timer_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    TIM3->PSC = (SYSCLK_HZ / 1000000u) - 1u;   /* 48 -> 1 MHz, 1 tick = 1 us */
    TIM3->ARR = 0xFFFFu;                        /* free-running, wraps at 65.5 ms */
    TIM3->EGR = TIM_EGR_UG;                     /* latch PSC/ARR */
    TIM3->CR1 = TIM_CR1_CEN;
}

void systick_init(void)
{
    SysTick->LOAD = (SYSCLK_HZ / 1000u) - 1u;
    SysTick->VAL  = 0;
    SysTick->CTRL = 7;   /* CLKSOURCE=AHB | TICKINT | ENABLE */
    us_timer_init();     /* must be up before the first delay_us() */
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
    while ((s_millis - t0) < ms) { wdt_kick(); __asm volatile("wfi"); }
}

/* Busy-wait on the 1 MHz TIM3 counter. 16-bit wrap-around arithmetic, so a
 * chunk may not exceed the 65.5 ms period; chunks of 30 ms keep a wide margin.
 * Note this bounds the WAIT exactly, not the whole pulse: an interrupt taken
 * after the wait still delays the falling edge by its own duration. */
void delay_us(uint32_t us)
{
    while (us) {
        uint16_t chunk = (us > 30000u) ? 30000u : (uint16_t)us;
        uint16_t t0 = (uint16_t)TIM3->CNT;
        while ((uint16_t)((uint16_t)TIM3->CNT - t0) < chunk) { }
        us -= chunk;
    }
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

/* ---- Diagnostic pin access ---------------------------------------------- */
static GPIO_Type *port_of(uint8_t port)
{
    switch (port) {
    case 0: return GPIOA;
    case 1: return GPIOB;
    case 2: return GPIOC;
    default: return 0;
    }
}

uint16_t sys_port_idr(uint8_t port)
{
    GPIO_Type *g = port_of(port);
    return g ? (uint16_t)g->IDR : 0u;
}

int sys_pin_toggle(uint8_t port, uint8_t pin, uint8_t n)
{
    GPIO_Type *g = port_of(port);
    if (!g || pin > 15) return 0;
    /* Refuse the pins that carry this very command: PA11/PA12 are USB D-/D+
     * and PA13/PA14 are SWD. Toggling those would end the session rather than
     * answer a question. */
    if (g == GPIOA && (pin == 11 || pin == 12 || pin == 13 || pin == 14)) return 0;

    pin_t p = { g, pin };
    uint32_t save = g->MODER;
    gpio_mode(p, GPIO_OUT);
    for (uint8_t i = 0; i < n; i++) {
        gpio_set(p, 1); delay_ms(1);
        gpio_set(p, 0); delay_ms(1);
        wdt_kick();
    }
    g->MODER = save;               /* back to whatever it was, level untouched */
    return 1;
}
