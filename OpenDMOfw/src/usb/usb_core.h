/* OpenDMOfw - USB full-speed device core (STM32F0 USB peripheral). */
#ifndef OP57_USB_CORE_H
#define OP57_USB_CORE_H

#include <stdint.h>

/* Endpoint layout (see PROTOCOL.md). */
#define EP_CTRL   0
/* Bulk pair on endpoint NUMBER 2, as on the genuine 550: a published
 * `lsusb -v` of 0922:0028 lists 0x82 (IN) first, then 0x02 (OUT). The number
 * is both the EPnR register index and the address, so it moves together. */
#define EP_DATA   2          /* bulk: OUT = print data, IN = status/reply */
#define EP_MAXPKT 64

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

void usb_init(void);

/* Control stage helpers (called from the descriptor/class layer). */
void usb_ctrl_send(const uint8_t *data, uint16_t len, uint16_t wLength);
void usb_ctrl_stall(void);
void usb_ctrl_ack(void);        /* zero-length status IN */

/* Bulk IN to the host (status/replies). Returns the number of bytes placed in the FIFO. */
int  usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len);
/* Reopen bulk OUT reception after a packet has been processed. */
void usb_ep_rx_ready(uint8_t ep);
/* Drop a queued-but-uncollected bulk IN reply and clear an IN-side STALL.
 * Used by the printer-class SOFT_RESET. */
void usb_ep_flush_in(uint8_t ep);

int  usb_is_configured(void);

/* ---- Callbacks provided by other layers ---- */
/* Descriptor request: fill in the out parameters; return 1 = known, 0 = unknown. */
int  usb_desc_get(uint8_t type, uint8_t index, uint16_t lang,
                  const uint8_t **data, uint16_t *len);
/* Class/interface-specific SETUP (printer class). Return 1 = handled. */
int  usb_class_setup(const usb_setup_t *s);
/* Bulk OUT data received (print data stream). */
void usb_class_data_out(uint8_t ep, const uint8_t *data, uint16_t len);
/* Configuration set/cleared. */
void usb_class_set_configured(int on);

#endif
