/* OpenDMOfw - entrypoint.
 *
 * Initializes all subsystems and runs the main loop. The USB IRQ fills the
 * print-data ring buffer; protocol_task() processes it here synchronously (head + feed
 * must not run in IRQ context due to the long strobe-/step times).
 *
 * Roll state (SKU + label counter) is pure config in the EEPROM; there is no tag and
 * no authentication - every physical roll prints. The label counter counts down per
 * printed label and survives a power cycle. See PROTOCOL.md / DECISIONS.md.
 */
#include "system.h"
#include "pins.h"
#include "config/store.h"
#include "printer/head.h"
#include "printer/motor.h"
#include "printer/thermal.h"
#include "printer/protocol.h"
#include "usb/usb_core.h"
#include "usb/usb_desc.h"

static void io_init(void)
{
    gpio_mode(PIN_LED, GPIO_OUT);
    gpio_set(PIN_LED, 0);
    gpio_mode(PIN_BUTTON, GPIO_IN);
    gpio_pull(PIN_BUTTON, 1);            /* pull-up: button pulls to ground */
    gpio_mode(PIN_PAPER_SENSE, GPIO_IN);
    gpio_pull(PIN_PAPER_SENSE, 1);
}

/* LED status: on = configured/ready; slow blink = unconfigured;
 * fast blink = head overheated; double blink = paper out. */
static void led_update(void)
{
    uint32_t now = millis();
    int paper = (gpio_get(PIN_PAPER_SENSE) == PAPER_PRESENT_LEVEL);
    if (!thermal_ok()) {                 /* overheated: 5 Hz */
        gpio_set(PIN_LED, (int)((now / 100) & 1));
    } else if (!paper) {                 /* paper out: double blink every ~1.2 s */
        uint32_t p = now % 1200;
        gpio_set(PIN_LED, (p < 100) || (p >= 200 && p < 300));
    } else if (usb_is_configured()) {    /* ready: on */
        gpio_set(PIN_LED, 1);
    } else {                             /* waiting for host: 1 Hz */
        gpio_set(PIN_LED, (int)((now / 500) & 1));
    }
}

/* Front-panel button, matching what the genuine printer does with it:
 *   short press  -> form feed (550 TRM p.7: the form-feed button advances the
 *                   stock to the top-of-form tear position)
 *   hold ~10 s   -> built-in self test (550 TRM p.8: "press the form-feed
 *                   button and power button together and hold it down for
 *                   approximately 10 seconds ... a repeating series of test
 *                   patterns"; we have one button, so holding it is the gesture)
 * Both work with no host attached, which is the point during bring-up. */
#define BTN_DEBOUNCE_MS   40u
#define BTN_SELFTEST_MS   10000u

static void button_task(void)
{
    static int      down;
    static uint32_t down_ms;
    static int      fired;               /* self test already run for this press */
    int now_down = (gpio_get(PIN_BUTTON) == BUTTON_PRESSED_LEVEL);
    uint32_t now = millis();

    if (now_down && !down) { down = 1; down_ms = now; fired = 0; return; }
    if (now_down) {
        if (!fired && (now - down_ms) >= BTN_SELFTEST_MS) {
            fired = 1;
            protocol_self_test();
        }
        return;
    }
    if (down) {
        down = 0;
        if (!fired && (now - down_ms) >= BTN_DEBOUNCE_MS)
            protocol_form_feed();
    }
}

int main(void)
{
    /* SystemInit() (48 MHz system clock) was already done by Reset_Handler.
     * The watchdog goes first: from here on any hang, including one inside a
     * peripheral init or a fault handler, ends in a reset rather than a brick. */
    wdt_init();
    systick_init();
    io_init();

    store_init();                        /* config from EEPROM or defaults */
    head_init();
    head_set_density(store_get()->density);
    motor_init();
    thermal_init();
    protocol_init();

    usb_desc_init_serial();              /* unique serial code from MCU UID */
    usb_init();                          /* enumeration starts; host binds printer */
    irq_enable();

    for (;;) {
        protocol_task();                 /* processes print data (head/feed) */
        button_task();
        led_update();
        motor_idle_tick(300);            /* release the motor 300 ms after the last step */
        head_idle_tick(30000);           /* drop the 24 V heat rail after 30 s idle */
        wdt_kick();
        /* Nothing here polls faster than the 1 ms SysTick, and the USB IRQ
         * wakes us the moment print data arrives, so idle in WFI instead of
         * spinning - less self-heating next to a thermal head. */
        __asm volatile("wfi");
    }
}
