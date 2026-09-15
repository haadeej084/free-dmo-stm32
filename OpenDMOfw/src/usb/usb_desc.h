/* OpenDMOfw - USB descriptors (generic, own identity). */
#ifndef OP57_USB_DESC_H
#define OP57_USB_DESC_H
#include <stdint.h>
/* Requested by usb_core; here only the declaration of the
 * IEEE-1284 device ID (also used by the printer class layer). */
extern const char OP57_IEEE1284_ID[];
extern const uint16_t OP57_IEEE1284_ID_LEN;
/* Builds the serial descriptor from the 96-bit MCU UID (unique per chip). Call
 * before usb_init() so that GET_DESCRIPTOR(string 3) returns a valid value. */
void usb_desc_init_serial(void);
#endif
