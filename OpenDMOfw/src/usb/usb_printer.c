/* OpenDMOfw - USB Printer class requests + data stream coupling.
 *
 * Class requests (USB Printer Class 1.1):
 *   0 GET_DEVICE_ID   (bmRequestType 0xA1) -> IEEE-1284 ID with 2-byte length prefix
 *   1 GET_PORT_STATUS (0xA1)               -> 1 status byte
 *   2 SOFT_RESET      (0x21)               -> reset the print pipeline
 */
#include "usb_core.h"
#include "usb_desc.h"
#include "usb_printer.h"
#include "../printer/protocol.h"

static int s_paper_present = 1;

void usbp_set_paper_present(int present) { s_paper_present = present ? 1 : 0; }
int  usbp_paper_present(void)            { return s_paper_present; }

int usbp_send_reply(const uint8_t *data, uint16_t len)
{
    if (!usb_is_configured()) return 0;
    return usb_ep_write(EP_DATA, data, len);
}

/* ---- class SETUP -------------------------------------------------------- */
int usb_class_setup(const usb_setup_t *s)
{
    /* Class requests only. The recipient is NOT always 'interface': the USB
     * Printer Class 1.1 spec lists SOFT_RESET as 0x21, but its own erratum
     * records that devices ship expecting 0x23 (recipient OTHER), and that is
     * what Linux sends. Accept any recipient and let the bRequest switch
     * decide, rather than dropping a reset on the floor. */
    if (((s->bmRequestType >> 5) & 3) != 1) return 0;

    switch (s->bRequest) {
    case 0: { /* GET_DEVICE_ID */
        static uint8_t idbuf[256];
        uint16_t n = OP57_IEEE1284_ID_LEN;
        if (n > sizeof(idbuf) - 2) n = sizeof(idbuf) - 2;  /* clamp BEFORE deriving
                                                            * the length, or a long
                                                            * ID would advertise more
                                                            * than we copied */
        uint16_t total = (uint16_t)(n + 2);
        idbuf[0] = (uint8_t)(total >> 8);   /* length incl. these 2 bytes, BE */
        idbuf[1] = (uint8_t)(total & 0xFF);
        for (uint16_t i = 0; i < n; i++) idbuf[2+i] = (uint8_t)OP57_IEEE1284_ID[i];
        usb_ctrl_send(idbuf, total, s->wLength);
        return 1;
    }
    case 1: { /* GET_PORT_STATUS: bit4 select, bit3 no-error, bit5 paper-empty */
        static uint8_t st;
        st = 0x18;                          /* select + no-error */
        if (!s_paper_present) st |= 0x20;   /* paper empty */
        usb_ctrl_send(&st, 1, s->wLength);
        return 1;
    }
    case 2: /* SOFT_RESET */
        protocol_reset();
        usb_ctrl_ack();
        return 1;
    default:
        return 0;
    }
}

/* ---- bulk OUT: print data -> protocol parser ----------------------------- */
void usb_class_data_out(uint8_t ep, const uint8_t *data, uint16_t len)
{
    protocol_feed(data, len);
    /* protocol_feed handles the flow control itself (usb_ep_rx_ready). */
    (void)ep;
}

void usb_class_set_configured(int on)
{
    if (on) protocol_reset();
}
