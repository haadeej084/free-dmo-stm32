/* OpenDMOfw - host unit test for the USB device stack (src/usb/).
 *
 * Runs the real usb_core.c, usb_desc.c and usb_printer.c against a software
 * model of the STM32F0 USB peripheral, and plays the host side of every
 * transfer. Nothing here needs a board; what it proves is that the firmware's
 * register handling, control-transfer state machine and descriptors behave as
 * RM0091 and USB 2.0 require, so a first enumeration failure on hardware points
 * at clock, wiring or PMA addressing rather than at this code.
 *
 * The peripheral model follows RM0091 (DocID018940) section 30.6.2, EPnR:
 *   CTR_RX, CTR_TX              rc_w0  - writing 0 clears, 1 leaves unchanged
 *   DTOG_RX, STAT_RX, DTOG_TX,
 *   STAT_TX                     t      - writing 1 toggles, 0 leaves unchanged
 *   SETUP                       r      - read only
 *   EP_TYPE, EP_KIND, EA        rw
 * and the transaction behaviour of 30.5.2: a completed reception copies the
 * byte count into COUNTn_RX[9:0] leaving BL_SIZE/NUM_BLOCK alone, sets
 * STAT_RX = NAK and CTR_RX; a completed transmission sets STAT_TX = NAK and
 * CTR_TX; a SETUP is accepted whatever STAT_RX says and parks both directions
 * at NAK; each ACKed transaction toggles the matching DTOG bit.
 *
 * Build/run (host compiler, see Makefile `test`):
 *   cc -std=c11 -DOPENDMO_HOST_TEST -Isrc -o test_usb test/test_usb.c \
 *      src/usb/usb_core.c src/usb/usb_desc.c src/usb/usb_printer.c
 */
#include <stdio.h>
#include <string.h>
#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "usb/usb_core.h"
#include "usb/usb_desc.h"
#include "usb/usb_printer.h"

#include "usb_model.h"

/* ---- stubs for the layers above and beside the USB stack ----------------- */
static uint32_t g_ms;
uint32_t millis(void)            { return g_ms++; }   /* advances, so waits end */
void wdt_kick(void)              {}
void delay_us(uint32_t us)       { (void)us; }
void gpio_af(pin_t p, uint8_t a) { (void)p; (void)a; }

static uint8_t g_rx[512];
static int     g_rx_len;
static int     g_resets;
static int     g_hold_rx;                  /* simulate a full ring buffer */
void protocol_feed(const uint8_t *d, uint16_t n)
{
    memcpy(g_rx + g_rx_len, d, n); g_rx_len += n;
    if (!g_hold_rx) usb_ep_rx_ready(EP_DATA);
}
void protocol_reset(void) { g_resets++; }

/* ---- checks ------------------------------------------------------------- */
static int fails, checks;
#define CHECK(c) do { checks++; if (c) printf("ok   %s\n", #c); \
                      else { printf("FAIL %s  (line %d)\n", #c, __LINE__); fails++; } } while (0)

static void bus_reset(void)
{
    irq(0, USB_ISTR_RESET);
}

static void enumerate(void)
{
    uint8_t b[256];
    bus_reset();
    ctrl_in(0x80, 6, 0x0100, 0, 64, b);
    ctrl_nodata(0x00, 5, 7, 0);
    ctrl_nodata(0x00, 9, 1, 0);
}

int main(void)
{
    uint8_t b[512];
    int r;
    usb_desc_init_serial();

    /* 1) Bus reset: EP0 control, RX armed, TX NAK, address 0 with EF set. */
    bus_reset();
    CHECK((epr(0) & USB_EP_TYPE) == USB_EP_TYPE_CONTROL);
    CHECK(stat_rx(0) == USB_EP_STAT_VALID && stat_tx(0) == USB_EP_STAT_NAK);
    CHECK(host_usb.DADDR == USB_DADDR_EF);
    CHECK(rx_capacity(0) == 64);

    /* 2) Device descriptor, byte-identical to a genuine unit's `lsusb -v`
     *    (bcdUSB 2.00, class 0, MPS0 64, VID 0922, bcdDevice 1.00, strings
     *    1/2/3, one configuration) apart from the per-model PID. */
    r = ctrl_in(0x80, 6, 0x0100, 0, 64, b);
    {
        const uint8_t want[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64,
            0x22, 0x09, (uint8_t)MODEL_PID, (uint8_t)(MODEL_PID >> 8),
            0x00, 0x01, 1, 2, 3, 1 };
        CHECK(r == 18 && memcmp(b, want, 18) == 0);
    }
    /* Windows asks for 8 bytes first on some stacks: honour wLength. */
    r = ctrl_in(0x80, 6, 0x0100, 0, 8, b);
    CHECK(r == 8 && b[7] == 64);

    /* 3) SET_ADDRESS takes effect only after the status stage (USB 2.0 9.4.6). */
    {
        uint8_t s[8]; mk_setup(s, 0x00, 5, 0x25, 0, 0);
        host_setup(s);
        CHECK(host_usb.DADDR == USB_DADDR_EF);          /* not yet */
        r = host_in(0, b, 64);
        CHECK(r == 0);
        CHECK(host_usb.DADDR == (USB_DADDR_EF | 0x25)); /* now */
    }

    /* 4) Configuration descriptor, short read then full: matches the genuine
     *    0922:0028 dump - self-powered 4 mA, printer 7/1/2, 0x82 IN first. */
    r = ctrl_in(0x80, 6, 0x0200, 0, 9, b);
    CHECK(r == 9 && b[2] == 32 && b[3] == 0);
    r = ctrl_in(0x80, 6, 0x0200, 0, 255, b);
    {
        const uint8_t want[32] = {
            9, 2, 32, 0, 1, 1, 0, 0xC0, 0x02,
            9, 4, 0, 0, 2, 7, 1, 2, 0,
            7, 5, 0x82, 0x02, 64, 0, 0,
            7, 5, 0x02, 0x02, 64, 0, 0 };
        CHECK(r == 32 && memcmp(b, want, 32) == 0);
    }

    /* 5) Strings: language, manufacturer, product with vendor prefix, a
     *    12-digit serial without a leading zero; unknown index stalls. */
    r = ctrl_in(0x80, 6, 0x0300, 0, 255, b);
    CHECK(r == 4 && b[2] == 0x09 && b[3] == 0x04);
    r = ctrl_in(0x80, 6, 0x0302, 0x0409, 255, b);
    {
        char prod[40]; int n = (r - 2) / 2;
        for (int i = 0; i < n && i < 39; i++) prod[i] = (char)b[2 + 2 * i];
        prod[n < 39 ? n : 39] = 0;
        CHECK(r == b[0] && strcmp(prod, MODEL_USB_PRODUCT) == 0);
    }
    r = ctrl_in(0x80, 6, 0x0303, 0x0409, 255, b);
    {
        int digits = 1;
        for (int i = 0; i < 12; i++)
            if (b[2 + 2 * i] < '0' || b[2 + 2 * i] > '9' || b[3 + 2 * i] != 0) digits = 0;
        CHECK(r == 26 && digits && b[2] != '0');
    }
    CHECK(ctrl_in(0x80, 6, 0x0307, 0x0409, 255, b) == STALL);
    /* EP0 recovers from that STALL at the next SETUP (RM0091 30.5.2). */
    r = ctrl_in(0x80, 0, 0, 0, 2, b);
    CHECK(r == 2 && b[0] == 0x01 && b[1] == 0x00);      /* self-powered */

    /* 6) SET_CONFIGURATION opens EP2 as bulk: OUT armed with 64 bytes, IN NAK,
     *    both data toggles at DATA0, class layer told (protocol_reset). */
    g_resets = 0;
    CHECK(ctrl_nodata(0x00, 9, 1, 0) == 0);
    CHECK((epr(EP_DATA) & (USB_EP_TYPE | USB_EP_EA)) == (USB_EP_TYPE_BULK | EP_DATA));
    CHECK(stat_rx(EP_DATA) == USB_EP_STAT_VALID && stat_tx(EP_DATA) == USB_EP_STAT_NAK);
    CHECK(dtog_rx(EP_DATA) == 0 && dtog_tx(EP_DATA) == 0);
    CHECK(rx_capacity(EP_DATA) == 64);
    CHECK(g_resets == 1 && usb_is_configured());
    r = ctrl_in(0x80, 8, 0, 0, 1, b);
    CHECK(r == 1 && b[0] == 1);

    /* 7) Bulk OUT reaches the parser, and the endpoint re-arms at full
     *    capacity for a maximum-size packet. */
    {
        uint8_t pkt[64];
        for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)i;
        g_rx_len = 0;
        CHECK(host_out(EP_DATA, (const uint8_t *)"\x1b" "A\x00", 3) == 0);
        CHECK(g_rx_len == 3 && g_rx[0] == 0x1B && g_rx[1] == 'A');
        CHECK(host_out(EP_DATA, pkt, 64) == 0);
        CHECK(g_rx_len == 67 && g_rx[3 + 63] == 63);
        CHECK(dtog_rx(EP_DATA) == 0);                   /* two packets: 0->1->0 */
    }

    /* 8) Flow control: while the parser holds the endpoint, the host is NAKed
     *    and nothing is lost; usb_ep_rx_ready() re-opens it. */
    g_hold_rx = 1; g_rx_len = 0;
    CHECK(host_out(EP_DATA, (const uint8_t *)"abc", 3) == 0);
    CHECK(host_out(EP_DATA, (const uint8_t *)"def", 3) == NAK);
    g_hold_rx = 0;
    usb_ep_rx_ready(EP_DATA);
    CHECK(host_out(EP_DATA, (const uint8_t *)"def", 3) == 0);
    CHECK(g_rx_len == 6 && memcmp(g_rx, "abcdef", 6) == 0);

    /* 9) Bulk IN: a reply is collected intact; a second reply while the first
     *    is uncollected is refused rather than overwriting it. */
    {
        uint8_t rep[32];
        for (int i = 0; i < 32; i++) rep[i] = (uint8_t)(0xA0 + i);
        CHECK(usbp_send_reply(rep, 32) == 32);
        CHECK(usbp_send_reply((const uint8_t *)"XX", 2) == 0);   /* busy */
        r = host_in(EP_DATA, b, 64);
        CHECK(r == 32 && memcmp(b, rep, 32) == 0);
        CHECK(host_in(EP_DATA, b, 64) == NAK);
        CHECK(usbp_send_reply((const uint8_t *)"OK", 2) == 2);
        r = host_in(EP_DATA, b, 64);
        CHECK(r == 2 && b[0] == 'O');
        CHECK(dtog_tx(EP_DATA) == 0);                   /* two packets */
    }

    /* 10) Register discipline: changing one STAT field must not clear a
     *     pending CTR flag of the other direction, nor flip a DTOG. */
    hw_set(EP_DATA, 0, USB_EP_CTR_TX | USB_EP_DTOG_TX);
    g_hold_rx = 1;
    host_out(EP_DATA, (const uint8_t *)"z", 1);          /* RX now NAK */
    hw_set(EP_DATA, 0, USB_EP_CTR_TX);                   /* re-assert pending TX */
    usb_ep_rx_ready(EP_DATA);
    CHECK(epr(EP_DATA) & USB_EP_CTR_TX);
    CHECK(stat_rx(EP_DATA) == USB_EP_STAT_VALID);
    CHECK(dtog_tx(EP_DATA) == 1);
    g_hold_rx = 0;
    hw_set(EP_DATA, USB_EP_CTR_TX | USB_EP_DTOG_TX, 0);

    /* 11) Printer class GET_DEVICE_ID: 2-byte big-endian length including
     *     itself, then MODEL_IEEE_ID; spans two control packets; a short
     *     wLength truncates. */
    r = ctrl_in(0xA1, 0, 0, 0, 1023, b);
    {
        /* 450-family layout: the model string, then SERN:<USB serial>; */
        char want[200], serial[13];
        uint8_t sd[64];
        ctrl_in(0x80, 6, 0x0303, 0x0409, 255, sd);
        for (int i = 0; i < 12; i++) serial[i] = (char)sd[2 + 2 * i];
        serial[12] = 0;
        snprintf(want, sizeof want, "%sSERN:%s;", MODEL_IEEE_ID, serial);
        r = ctrl_in(0xA1, 0, 0, 0, 1023, b);
        int len = (b[0] << 8) | b[1];
        int idl = (int)strlen(want);
        CHECK(r == idl + 2 && len == idl + 2 && r > 64);
        CHECK(memcmp(b + 2, want, (size_t)idl) == 0);
    }
    r = ctrl_in(0xA1, 0, 0, 0, 2, b);
    CHECK(r == 2);

    /* 12) GET_PORT_STATUS: select + no-error; paper-empty bit when out. */
    r = ctrl_in(0xA1, 1, 0, 0, 1, b);
    CHECK(r == 1 && b[0] == 0x18);
    usbp_set_paper_present(0);
    r = ctrl_in(0xA1, 1, 0, 0, 1, b);
    CHECK(r == 1 && b[0] == 0x38);
    usbp_set_paper_present(1);

    /* 13) SOFT_RESET, both recipients (0x21 per spec, 0x23 as Linux sends):
     *     queued IN reply dropped, parser reset, data toggles untouched. */
    {
        int rec[2] = { 0x21, 0x23 };
        for (int k = 0; k < 2; k++) {
            CHECK(usbp_send_reply((const uint8_t *)"stale", 5) == 5);
            host_usb.EPR[EP_DATA] |= USB_EP_DTOG_TX;      /* mid-stream toggle */
            g_resets = 0;
            CHECK(ctrl_nodata((uint8_t)rec[k], 2, 0, 0) == 0);
            CHECK(g_resets == 1);
            CHECK(host_in(EP_DATA, b, 64) == NAK);
            CHECK(dtog_tx(EP_DATA) == 1);
            host_usb.EPR[EP_DATA] &= ~USB_EP_DTOG_TX;
        }
    }

    /* 14) ENDPOINT_HALT on 0x82: SET_FEATURE stalls IN, GET_STATUS reports it,
     *     CLEAR_FEATURE un-stalls and resets the toggle to DATA0. */
    CHECK(usbp_send_reply((const uint8_t *)"a", 1) == 1);
    host_in(EP_DATA, b, 64);                             /* DTOG_TX -> 1 */
    CHECK(ctrl_nodata(0x02, 3, 0, 0x82) == 0);
    CHECK(host_in(EP_DATA, b, 64) == STALL);
    r = ctrl_in(0x82, 0, 0, 0x82, 2, b);
    CHECK(r == 2 && b[0] == 1);
    CHECK(ctrl_nodata(0x02, 1, 0, 0x82) == 0);
    CHECK(host_in(EP_DATA, b, 64) == NAK && dtog_tx(EP_DATA) == 0);
    r = ctrl_in(0x82, 0, 0, 0x82, 2, b);
    CHECK(r == 2 && b[0] == 0);
    /* same for OUT 0x02 */
    CHECK(ctrl_nodata(0x02, 3, 0, 0x02) == 0);
    CHECK(host_out(EP_DATA, (const uint8_t *)"q", 1) == STALL);
    CHECK(ctrl_nodata(0x02, 1, 0, 0x02) == 0);
    CHECK(host_out(EP_DATA, (const uint8_t *)"q", 1) == 0);

    /* 14b) An endpoint-recipient request may only name an endpoint that exists.
     *      EPR[] has eight entries and an endpoint that was never given an
     *      address would answer for endpoint 0 (RM0091 30.6.2, EA), so a bad
     *      wIndex is a Request Error, not something to act on. USB 2.0 9.4.5
     *      also says Halt is not recommended for the default control pipe. */
    CHECK(ctrl_nodata(0x02, 3, 0, 0x81) == STALL);    /* no such endpoint */
    CHECK(ctrl_nodata(0x02, 3, 0, 0x03) == STALL);
    CHECK(ctrl_nodata(0x02, 3, 0, 0x0F) == STALL);    /* would index past EPR[] */
    CHECK(ctrl_nodata(0x02, 1, 0, 0x81) == STALL);
    CHECK(ctrl_in(0x82, 0, 0, 0x81, 2, b) == STALL);
    CHECK(ctrl_nodata(0x02, 3, 0, 0x00) == STALL);    /* halt on EP0: refused */
    CHECK(ctrl_nodata(0x02, 1, 0, 0x80) == 0);        /* clear on EP0: harmless ack */
    r = ctrl_in(0x82, 0, 0, 0x00, 2, b);
    CHECK(r == 2 && b[0] == 0);                       /* EP0 is never halted */
    /* the bulk pair still works, and the endpoints are untouched by the above */
    CHECK(host_out(EP_DATA, (const uint8_t *)"z", 1) == 0);
    CHECK(usbp_send_reply((const uint8_t *)"y", 1) == 1);
    CHECK(host_in(EP_DATA, b, 64) == 1);

    /* 15) A SOFT_RESET clears a host-set IN stall (Printer Class 1.1 4.2.3). */
    CHECK(ctrl_nodata(0x02, 3, 0, 0x82) == 0);
    CHECK(ctrl_nodata(0x21, 2, 0, 0) == 0);
    CHECK(host_in(EP_DATA, b, 64) == NAK);

    /* 16) Re-configuration resets both toggles to DATA0 (RM0091 30.6.2). */
    host_usb.EPR[EP_DATA] |= USB_EP_DTOG_TX | USB_EP_DTOG_RX;
    CHECK(ctrl_nodata(0x00, 9, 1, 0) == 0);
    CHECK(dtog_tx(EP_DATA) == 0 && dtog_rx(EP_DATA) == 0);
    host_usb.EPR[EP_DATA] |= USB_EP_DTOG_TX | USB_EP_DTOG_RX;
    CHECK(ctrl_nodata(0x00, 11, 0, 0) == 0);            /* SET_INTERFACE */
    CHECK(dtog_tx(EP_DATA) == 0 && dtog_rx(EP_DATA) == 0);

    /* 17) Unsupported requests stall, and an unsupported control WRITE keeps
     *     its data stage stalled instead of ACKing and dropping the data. */
    CHECK(ctrl_in(0x80, 0x33, 0, 0, 8, b) == STALL);
    {
        uint8_t s[8]; mk_setup(s, 0x21, 0x09, 0, 0, 4);  /* class OUT, 4 bytes */
        host_setup(s);
        CHECK(stat_rx(0) == USB_EP_STAT_STALL);
        CHECK(host_out(0, (const uint8_t *)"data", 4) == STALL);
    }
    CHECK(ctrl_in(0xC0, 0x01, 0, 0, 8, b) == STALL);     /* vendor request */
    r = ctrl_in(0x80, 0, 0, 0, 2, b);                    /* and EP0 recovers */
    CHECK(r == 2 && b[0] == 0x01);

    /* 18) Control IN of exactly one full packet with a larger wLength ends
     *     with a zero-length packet (USB 2.0 5.5.3). */
    {
        static uint8_t full[64];
        memset(full, 0x5A, sizeof full);
        uint8_t s[8]; mk_setup(s, 0xC0, 0x7F, 0, 0, 255);
        host_setup(s);                                   /* stalls ... */
        usb_ctrl_send(full, 64, 255);                    /* ... then send manually */
        CHECK(host_in(0, b, 64) == 64 && b[63] == 0x5A);
        CHECK(host_in(0, b, 64) == 0);                   /* the ZLP */
        CHECK(host_in(0, b, 64) == NAK);
        usb_ctrl_send(full, 64, 64);                     /* exact wLength: no ZLP */
        CHECK(host_in(0, b, 64) == 64);
        CHECK(host_in(0, b, 64) == NAK);
    }

    /* 19) A bus reset drops the configuration: bulk writes are refused. */
    bus_reset();
    CHECK(!usb_is_configured());
    CHECK(usbp_send_reply((const uint8_t *)"x", 1) == 0);
    CHECK(host_usb.DADDR == USB_DADDR_EF);
    enumerate();
    CHECK(usb_is_configured() && host_usb.DADDR == (USB_DADDR_EF | 7));

    /* 20) Suspend / wake-up toggle FSUSP. */
    irq(0, USB_ISTR_SUSP);
    CHECK(host_usb.CNTR & USB_CNTR_FSUSP);
    irq(0, USB_ISTR_WKUP);
    CHECK(!(host_usb.CNTR & USB_CNTR_FSUSP));

    printf(fails ? "\n%d of %d USB check(s) FAILED\n" : "\nALL %d USB CHECKS PASSED\n",
           fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
