/* OpenDMOfw - USB Printer class layer. */
#ifndef OP57_USB_PRINTER_H
#define OP57_USB_PRINTER_H
#include <stdint.h>
/* Called by the protocol layer to send a status reply to the host over the
 * bulk-IN endpoint. */
int  usbp_send_reply(const uint8_t *data, uint16_t len);
/* Port status returned by GET_PORT_STATUS; set by the protocol/sensors. */
void usbp_set_paper_present(int present);
int  usbp_paper_present(void);
#endif
