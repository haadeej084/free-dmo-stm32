/* OpenDMOfw - minimal STM32F072 register definitions.
 *
 * Deliberately NOT a full CMSIS/vendor SDK: only the peripheral registers that
 * this firmware actually touches, so the repo builds standalone with a bare
 * arm-none-eabi-gcc. To extend = add a struct field at the correct offset.
 *
 * All offsets come from RM0091 (STM32F0x1/F0x2/F0x8 reference manual).
 */
#ifndef OP57_MCU_H
#define OP57_MCU_H

#include <stdint.h>
#include <stddef.h>

#define __IO volatile

/* Host unit test (test/test_usb.c) only: the inline-assembly construct below is
 * specific to the Arm toolchain, so a native compiler sees it as a no-op. Never
 * defined in a firmware build. */
#ifdef OPENDMO_HOST_TEST
#define __asm
#define volatile(...) ((void)0)
#endif

/* ---- Cortex-M0 core (NVIC + SysTick + SCB, subset) ---------------------- */
typedef struct {
    __IO uint32_t ISER[1]; uint32_t _r0[31];
    __IO uint32_t ICER[1]; uint32_t _r1[31];
    __IO uint32_t ISPR[1]; uint32_t _r2[31];
    __IO uint32_t ICPR[1]; uint32_t _r3[31];
    uint32_t _r4[64];
    __IO uint32_t IPR[8];
} NVIC_Type;
#define NVIC ((NVIC_Type*)0xE000E100u)

typedef struct {
    __IO uint32_t CTRL, LOAD, VAL, CALIB;
} SysTick_Type;
#define SysTick ((SysTick_Type*)0xE000E010u)

static inline void nvic_enable(int irq) { NVIC->ISER[0] = (1u << (irq & 31)); }

/* IRQ numbers (RM0091 vector table) that we use. */
#define USB_IRQn      31
#define TIM3_IRQn     16

/* ---- RCC ---------------------------------------------------------------- */
typedef struct {
    __IO uint32_t CR, CFGR, CIR, APB2RSTR, APB1RSTR, AHBENR, APB2ENR, APB1ENR;
    __IO uint32_t BDCR, CSR, AHBRSTR, CFGR2, CFGR3, CR2;
} RCC_Type;
#define RCC ((RCC_Type*)0x40021000u)

#define RCC_CR_HSEON     (1u<<16)
#define RCC_CR_HSERDY    (1u<<17)
#define RCC_CR_PLLON     (1u<<24)
#define RCC_CR_PLLRDY    (1u<<25)
/* PLL from HSE/PREDIV at x4 -> 48 MHz from the board's 12 MHz crystal. */
#define RCC_CFGR_PLLSRC_HSE_PREDIV (2u<<15)
#define RCC_CFGR_PLLMUL4           (2u<<18)
#define RCC_CFGR_SW_PLL            (2u<<0)
#define RCC_CFGR_SWS_PLL           (2u<<2)
#define RCC_CFGR3_USBSW_PLL        (1u<<7)   /* 0 = HSI48 (reset default) */
#define RCC_CR2_HSI48ON  (1u<<16)
#define RCC_CR2_HSI48RDY (1u<<17)
/* On the STM32F0x2 (F072) the CFGR.SW field DOES have an HSI48 option:
 * SW=11 selects HSI48 as the system clock (verified vs CMSIS stm32f072xb.h:
 * RCC_CFGR_SW_HSI48=0x3, RCC_CFGR_SWS_HSI48=0xC). This is the crystalless 48 MHz
 * design: HSI48 is both the CPU clock and the USB clock, trimmed by CRS on SOF. */
#define RCC_CFGR_SW_HSI48 (3u<<0)
#define RCC_CFGR_SWS_HSI48 (3u<<2)

#define RCC_AHBENR_GPIOAEN (1u<<17)
#define RCC_AHBENR_GPIOBEN (1u<<18)
#define RCC_AHBENR_GPIOCEN (1u<<19)
#define RCC_AHBENR_GPIOFEN (1u<<22)

#define RCC_APB1ENR_TIM3EN  (1u<<1)
#define RCC_APB1ENR_I2C1EN  (1u<<21)
#define RCC_APB1ENR_USBEN   (1u<<23)
#define RCC_APB1ENR_CRSEN   (1u<<27)
#define RCC_APB1ENR_PWREN   (1u<<28)

#define RCC_APB2ENR_ADC1EN  (1u<<9)
#define RCC_APB2ENR_SPI1EN  (1u<<12)
#define RCC_APB2ENR_SYSCFGEN (1u<<0)

/* ---- FLASH interface ---------------------------------------------------- */
/* ---- SCB reset + SYSCFG memory remap (USB DFU entry, see startup.c) ----- */
#define SCB_AIRCR                (*(volatile uint32_t*)0xE000ED0Cu)
#define SCB_AIRCR_SYSRESETREQ    (0x05FA0000u | (1u<<2))  /* VECTKEY + SYSRESETREQ */
typedef struct { __IO uint32_t CFGR1; } SYSCFG_Type;
#define SYSCFG ((SYSCFG_Type*)0x40010000u)
#define SYSCFG_CFGR1_MEM_MODE_Msk    3u
#define SYSCFG_CFGR1_MEM_MODE_SYSMEM 1u   /* 01: system flash at 0x00000000 */
/* ST system-memory boot loader of the STM32F071xx/072xx: 0x1FFFC800 (AN2606,
 * "STM32F071xx/072xx" table). It offers USART and USB DFU; the DFU path clocks
 * itself from HSI48 + CRS, so no crystal is needed. */
#define SYSMEM_BASE 0x1FFFC800u

typedef struct { __IO uint32_t ACR; } FLASH_Type;
#define FLASH ((FLASH_Type*)0x40022000u)
#define FLASH_ACR_LATENCY1 (1u<<0)
#define FLASH_ACR_PRFTBE   (1u<<4)

/* ---- IWDG (independent watchdog, LSI-fed) ------------------------------- */
typedef struct { __IO uint32_t KR, PR, RLR, SR, WINR; } IWDG_Type;
#define IWDG ((IWDG_Type*)0x40003000u)

/* ---- Unique device ID (96-bit) ------------------------------------------ */
#define UID_BASE 0x1FFFF7ACu

/* ---- CRS (auto-trim HSI48 on USB SOF) ----------------------------------- */
typedef struct { __IO uint32_t CR, CFGR, ISR, ICR; } CRS_Type;
#define CRS ((CRS_Type*)0x40006C00u)
/* RM0091 CRS_CR: bit 5 = CEN, bit 6 = AUTOTRIMEN. These were swapped here.
 * Invisible today because system.c ORs both in one write, but any future code
 * touching one alone would have hit the wrong bit. */
#define CRS_CR_CEN        (1u<<5)
#define CRS_CR_AUTOTRIMEN (1u<<6)
#define CRS_CFGR_SYNCSRC_Msk (3u<<28)
#define CRS_CFGR_SYNCSRC_USB (2u<<28)   /* USB SOF (RM0091) */

/* ---- GPIO --------------------------------------------------------------- */
typedef struct {
    __IO uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2], BRR;
} GPIO_Type;
#define GPIOA ((GPIO_Type*)0x48000000u)
#define GPIOB ((GPIO_Type*)0x48000400u)
#define GPIOC ((GPIO_Type*)0x48000800u)
#define GPIOF ((GPIO_Type*)0x48001400u)

/* The head is bit-banged GPIO (see pins.h / head.c), so there is deliberately
 * no SPI register map here. If you ever move the shift to SPI1, add it then -
 * an unused register map only reads as "this is wired up" when it is not.
 * A move to SPI/DMA that overlaps the shift with the strobe must keep a quiet
 * gap between the end of one line's strobe and the next latch: other thermal
 * heads specify one (Seiko LTP02-245-13, Table 3-10: "/LAT wait time" 8000 ns
 * min, else "Vp voltage fluctuations"). Today the bit-banged shift itself fills
 * that gap (hundreds of us); the KF3002 figure is not in our notes. */

/* ---- I2C (v2, config EEPROM) -------------------------------------------- */
typedef struct {
    __IO uint32_t CR1, CR2, OAR1, OAR2, TIMINGR, TIMEOUTR, ISR, ICR, PECR, RXDR, TXDR;
} I2C_Type;
#define I2C1 ((I2C_Type*)0x40005400u)
#define I2C_CR1_PE   (1u<<0)
#define I2C_CR2_START (1u<<13)
#define I2C_CR2_STOP  (1u<<14)
#define I2C_CR2_RD_WRN (1u<<10)
#define I2C_CR2_AUTOEND (1u<<25)
#define I2C_ISR_TXIS  (1u<<1)
#define I2C_ISR_RXNE  (1u<<2)
#define I2C_ISR_TC    (1u<<6)
#define I2C_ISR_STOPF (1u<<5)
#define I2C_ISR_NACKF (1u<<4)
#define I2C_ISR_BUSY  (1u<<15)
#define I2C_ICR_NACKCF (1u<<4)
#define I2C_ICR_STOPCF (1u<<5)

/* ---- ADC (head thermistor temperature) ---------------------------------- */
typedef struct {
    __IO uint32_t ISR, IER, CR, CFGR1, CFGR2, SMPR;
    uint32_t _r0[2];
    __IO uint32_t TR, _r1, CHSELR;
    uint32_t _r2[5];
    __IO uint32_t DR;
} ADC_Type;
#define ADC1 ((ADC_Type*)0x40012400u)
#define ADC_CR_ADEN    (1u<<0)
#define ADC_CR_ADSTART (1u<<2)
#define ADC_CR_ADSTP   (1u<<4)
#define ADC_CR_ADCAL   (1u<<31)
#define ADC_ISR_ADRDY  (1u<<0)
#define ADC_ISR_EOC    (1u<<2)

/* ---- TIM3 (free-running 1 MHz counter behind delay_us, see system.c) ----- */
typedef struct {
    __IO uint32_t CR1, CR2, SMCR, DIER, SR, EGR, CCMR1, CCMR2, CCER, CNT, PSC, ARR;
} TIM_Type;
#define TIM3 ((TIM_Type*)0x40000400u)
#define TIM_CR1_CEN (1u<<0)
#define TIM_EGR_UG  (1u<<0)

/* ---- USB device controller (RM0091 chapter 30) -------------------------- */
/* Register map (verified vs CMSIS stm32f072xb.h): the endpoint registers AND
 * the control registers are spaced 4 bytes apart - EP0R@0x00, EP1R@0x04, ...
 * EP7R@0x1C, then reserved 0x20-0x3F, CNTR@0x40, ISTR@0x44, FNR@0x48,
 * DADDR@0x4C, BTABLE@0x50, LPMCSR@0x54, BCDR@0x58. Each register lives in the
 * LOW half of its 4-byte slot, so a uint32_t per slot keeps USB->EPR[n] indexing
 * correct while landing on the right address (all values used are <=16 bits). */
typedef struct {
    __IO uint32_t EPR[8];     /* EP0R..EP7R @ 0x00,0x04,...,0x1C */
    uint32_t      _r0[8];     /* reserved 0x20-0x3F */
    __IO uint32_t CNTR;       /* @ 0x40 */
    __IO uint32_t ISTR;       /* @ 0x44 */
    __IO uint32_t FNR;        /* @ 0x48 */
    __IO uint32_t DADDR;      /* @ 0x4C */
    __IO uint32_t BTABLE;     /* @ 0x50 */
    __IO uint32_t LPMCSR;     /* @ 0x54 */
    __IO uint32_t BCDR;       /* @ 0x58 */
} USB_Type;
#define USB ((USB_Type*)0x40005C00u)
/* Packet Memory Area: 1024 B at 0x40006000 with 1:1 CPU addressing. RM0091
 * Rev 9 gives the scheme as "2 x 16 bits / word" for STM32F072, and its Table 1
 * maps exactly 1 KB of "USB/CAN SRAM" at 0x40006000-0x400063FF, which only a 1:1
 * layout fits (ST's LL driver agrees: PMA_ACCESS 1U). Byte or halfword accesses
 * only - 32-bit accesses are not allowed. If RCC_APB1ENR.CANEN is ever set, CAN
 * takes the last 256 B; our buffers end at 0x13F, inside the first 768 B. */
#define USB_PMA_BASE 0x40006000u

#define USB_CNTR_FRES   (1u<<0)
#define USB_CNTR_PDWN   (1u<<1)
#define USB_CNTR_FSUSP  (1u<<3)
#define USB_CNTR_RESETM (1u<<10)
#define USB_CNTR_CTRM   (1u<<15)
#define USB_CNTR_SUSPM  (1u<<11)
#define USB_CNTR_WKUPM  (1u<<12)

#define USB_ISTR_CTR    (1u<<15)
#define USB_ISTR_RESET  (1u<<10)
#define USB_ISTR_SUSP   (1u<<11)
#define USB_ISTR_WKUP   (1u<<12)
#define USB_ISTR_DIR    (1u<<4)
#define USB_ISTR_EPID   (0x0Fu)

#define USB_DADDR_EF    (1u<<7)
#define USB_BCDR_DPPU   (1u<<15)

/* EPnR is a register with toggle-only (t) and rc_w0 bits mixed together; the macros
 * in usb_core.c manage that correctly. Bit definitions: */
#define USB_EP_CTR_RX   (1u<<15)
#define USB_EP_DTOG_RX  (1u<<14)
#define USB_EP_STAT_RX  (3u<<12)
#define USB_EP_SETUP    (1u<<11)
#define USB_EP_TYPE     (3u<<9)
#define USB_EP_KIND     (1u<<8)
#define USB_EP_CTR_TX   (1u<<7)
#define USB_EP_DTOG_TX  (1u<<6)
#define USB_EP_STAT_TX  (3u<<4)
#define USB_EP_EA       (0x0Fu)

#define USB_EP_TYPE_BULK    (0u<<9)
#define USB_EP_TYPE_CONTROL (1u<<9)
#define USB_EP_TYPE_ISO     (2u<<9)
#define USB_EP_TYPE_INTR    (3u<<9)

#define USB_EP_STAT_DISABLED 0u
#define USB_EP_STAT_STALL    1u
#define USB_EP_STAT_NAK      2u
#define USB_EP_STAT_VALID    3u

/* EPnR writes go through one macro so the host unit test can model the
 * register's mixed toggle / rc_w0 / read-only bit semantics. */
#ifndef OPENDMO_HOST_TEST
#define USB_EPR_WRITE(n, v) (USB->EPR[(n)] = (uint16_t)(v))
#else
void host_epr_write(int n, uint16_t v);
#define USB_EPR_WRITE(n, v) host_epr_write((n), (uint16_t)(v))
#endif

static inline void irq_disable(void){ __asm volatile("cpsid i":::"memory"); }
static inline void irq_enable(void){ __asm volatile("cpsie i":::"memory"); }

/* Host unit test only: point the USB peripheral, its packet memory and the
 * UID at RAM owned by test/test_usb.c. Placed last so the inline helpers above
 * keep the real addresses (the test never calls them). */
#ifdef OPENDMO_HOST_TEST
#undef USB
#undef USB_PMA_BASE
#undef UID_BASE
extern USB_Type host_usb;
extern uint16_t host_pma[512];
extern uint32_t host_uid[3];
#define USB          (&host_usb)
#define USB_PMA_BASE ((uintptr_t)host_pma)
#define UID_BASE     ((uintptr_t)host_uid)

/* The peripherals the printer-side modules touch, so their arithmetic can be
 * tested natively (test/test_thermal.c). Only the tests that actually use a
 * given peripheral define its storage, so the USB tests are unaffected.
 *
 * The ADC goes through a function rather than a plain struct: a conversion is
 * several register accesses, and a test that wants a DIFFERENT sample per
 * conversion (a median filter has three) needs a hook. host_adc() is called on
 * every access, so a test can load the next sample when it sees ADSTART set. */
#undef ADC1
#undef RCC
#undef GPIOA
#undef GPIOB
#undef GPIOC
#undef TIM3
#undef IWDG
#undef SysTick
ADC_Type *host_adc(void);
extern RCC_Type     host_rcc;
extern GPIO_Type    host_gpioa, host_gpiob, host_gpioc;
extern TIM_Type     host_tim3;
/* IWDG and SysTick joined the list when system.c got its first host harness:
 * sys_pin_toggle() calls wdt_kick() and delay_ms(), which are IN system.c, so
 * they cannot be stubbed out by the test - they have to hit storage instead of
 * a hardcoded peripheral address. Before this, a host build of system.c
 * segfaulted on the first watchdog kick, which is why the file had no harness
 * and why its hot-pin guard could be deleted with every suite still green. */
extern IWDG_Type    host_iwdg;
extern SysTick_Type host_systick;
#define ADC1    host_adc()
#define RCC     (&host_rcc)
#define GPIOA   (&host_gpioa)
#define GPIOB   (&host_gpiob)
#define GPIOC   (&host_gpioc)
#define TIM3    (&host_tim3)
#define IWDG    (&host_iwdg)
#define SysTick (&host_systick)
/* I2C1 goes through a function for the same reason the ADC does: a transfer is
 * a sequence of register accesses whose result depends on what a PART on the
 * bus does, so a test needs a hook on every access rather than plain storage.
 * test/i2c_eeprom_model.h defines host_i2c() over a 24Cxx model that can be
 * told to misbehave - write-protected, absent, NAKing mid-data, or losing
 * power between page writes. */
#undef I2C1
I2C_Type *host_i2c(void);
#define I2C1 host_i2c()
#endif

#endif /* OP57_MCU_H */
