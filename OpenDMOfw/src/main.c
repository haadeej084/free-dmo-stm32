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
 * fast blink = head overheated OR heat locked out; double blink = paper out.
 *
 * The 5 Hz branch covers every state in which this printer CANNOT PUT A DOT ON
 * A LABEL. Until cycle 4 it covered only over-temperature, and the other such
 * state - OP_FLAG_VH_INHIBIT set, where vh_enable() is a no-op and no strobe
 * can heat anything - showed a SOLID READY LIGHT. The printer would accept the
 * job, feed the label, advance to the tear bar and eject it blank, with the
 * panel saying "ready" throughout.
 *
 * That state is not exotic. D32 makes it the AUTOMATIC outcome of a config
 * record that fails its checksum, and it is the compiled default of the
 * OPENDMO_SAFE_BRINGUP image FIELDWORK tells a fieldworker to build first - so
 * the person most likely to meet it is the one with the least to go on.
 *
 * 5 Hz is also what the LW550 User Guide documents as "an error has occurred",
 * so this makes the panel more conformant, not less.
 *
 * Deliberately NOT included: thermal_sensor_fault(). A printer with an
 * unbelievable thermistor still prints, at 0.625x dwell (D33) - it is degraded,
 * not stopped, and the host is told through status byte 8. Blinking an error at
 * an operator whose printer is working would train them to ignore the light. */
static void led_update(void)
{
    uint32_t now = millis();
    int paper = (gpio_get(PIN_PAPER_SENSE) == PAPER_PRESENT_LEVEL);
    int locked = (store_get()->flags & OP_FLAG_VH_INHIBIT) != 0;
    if (!thermal_ok() || locked) {       /* cannot print: 5 Hz */
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
    /* SystemInit() (48 MHz system clock) and wdt_init() were both already done
     * by Reset_Handler - the watchdog deliberately before SystemInit(), so that
     * a clock that never comes up ends in a reset instead of a silent hang.
     * Kick it here rather than re-initialising: writes to IWDG_PR/RLR are
     * ignored while the previous ones are still being synchronised to the LSI
     * domain, so a second wdt_init() would be a no-op that merely looks like
     * configuration. */
    wdt_kick();
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
