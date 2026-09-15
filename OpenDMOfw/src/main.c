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

int main(void)
{
    /* SystemInit() (clock 48 MHz + CRS) was already done by Reset_Handler. */
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

    wdt_init();                          /* watchdog on (emit_line kicks during printing) */

    for (;;) {
        protocol_task();                 /* processes print data (head/feed) */
        led_update();
        motor_enable(0);                 /* motor cools/stays quiet when nothing is running */
        wdt_kick();
    }
}
