/* OpenDMO-FW - USB descriptors.
 *
 * IDENTITY (see DECISIONS.md): the device presents itself as the genuine
 * label printer of its class: VID 0x0922, per-model PID, "DYMO" /
 * "LabelWriter 5XL|550" strings and an IEEE-1284 device ID whose MFG+MDL
 * makes Windows derive the driver-model match ID the vendor's own driver
 * package expects (USBPRINT\DYMOLabelWriter_5XLB920 / ...550C80D).
 * Interface class = USB Printer (7), bidirectional (protocol 2), so the OS
 * usbprint driver binds and the vendor spooler driver + port monitor take
 * over the wire.
 */
#include "usb_desc.h"
#include "usb_core.h"
#include "../model.h"
#include "../mcu.h"     /* UID_BASE */

#define VID MODEL_VID   /* 0x0922 */
#define PID MODEL_PID   /* 0x002A (5XL) or 0x0028 (550), see model.h */

static const uint8_t dev_desc[18] = {
    18, 1,              /* bLength, DEVICE */
    0x00, 0x02,         /* bcdUSB 2.00 */
    0, 0, 0,            /* class/sub/proto per-interface */
    EP_MAXPKT,          /* bMaxPacketSize0 */
    (VID & 0xFF), (VID >> 8),
    (PID & 0xFF), (PID >> 8),
    0x00, 0x01,         /* bcdDevice 1.00 */
    1, 2, 3,            /* iManufacturer, iProduct, iSerial */
    1                   /* bNumConfigurations */
};

static const uint8_t cfg_desc[32] = {
    /* Configuration */
    9, 2, 32, 0, 1, 1, 0, 0x80, 0xFA,   /* 32 bytes total, 1 iface, bus-powered, 500 mA */
    /* Interface: Printer class 7 / subclass 1 / protocol 2 (bidir) */
    9, 4, 0, 0, 2, 7, 1, 2, 0,
    /* EP OUT 0x01 bulk 64 */
    7, 5, 0x01, 0x02, EP_MAXPKT, 0, 0,
    /* EP IN 0x81 bulk 64 */
    7, 5, 0x81, 0x02, EP_MAXPKT, 0, 0
};

/* Strings as UTF-16LE. */
static const uint8_t str_lang[4]  = { 4, 3, 0x09, 0x04 };  /* 0x0409 en-US */
#define USTR(name, ...) static const uint8_t name[] = { sizeof((uint8_t[]){__VA_ARGS__})+2, 3, __VA_ARGS__ }
/* "DYMO" */
USTR(str_mfg, 'D',0,'Y',0,'M',0,'O',0);
/* Product string per model: "LabelWriter 5XL" or "LabelWriter 550". */
#if defined(MODEL_OP57)
USTR(str_prod, 'L',0,'a',0,'b',0,'e',0,'l',0,'W',0,'r',0,'i',0,'t',0,'e',0,'r',0,
     ' ',0,'5',0,'5',0,'0',0);
#else
USTR(str_prod, 'L',0,'a',0,'b',0,'e',0,'l',0,'W',0,'r',0,'i',0,'t',0,'e',0,'r',0,
     ' ',0,'5',0,'X',0,'L',0);
#endif

/* Serial: 12 decimal digits derived from the MCU-UID (unique per chip),
 * matching the numeric format of the genuine device-instance suffix.
 * Length = 2 (header) + 12*2 = 26 bytes. Filled by usb_desc_init_serial(). */
static uint8_t str_serial[26];
void usb_desc_init_serial(void)
{
    volatile uint32_t *uid = (volatile uint32_t*)UID_BASE;
    uint32_t x = uid[0] ^ uid[1] ^ uid[2];
    /* 12 digits from the low 48 bits, biased away from a leading zero. */
    uint64_t v = ((uint64_t)(x ^ 0xA5A5A5A5u) << 16 | (uint64_t)(uid[3] & 0xFFFF));
    v %= 900000000000ULL;              /* keep to 12 digits, no leading zero */
    v += 100000000000ULL;
    str_serial[0] = 26; str_serial[1] = 3;
    for (int i = 11; i >= 0; i--) {
        str_serial[2 + i*2]     = (uint8_t)('0' + (int)(v % 10));
        str_serial[2 + i*2 + 1] = 0;
        v /= 10;
    }
}

/* IEEE-1284 device ID (used by the printer-class GET_DEVICE_ID). MFG+MDL are
 * chosen so the OS derives the genuine driver-model match ID; see model.h. */
const char OP57_IEEE1284_ID[] = MODEL_IEEE_ID;
const uint16_t OP57_IEEE1284_ID_LEN = sizeof(MODEL_IEEE_ID) - 1;

int usb_desc_get(uint8_t type, uint8_t index, uint16_t lang,
                 const uint8_t **data, uint16_t *len)
{
    (void)lang;
    switch (type) {
    case 1: *data = dev_desc; *len = sizeof(dev_desc); return 1;
    case 2: *data = cfg_desc; *len = sizeof(cfg_desc); return 1;
    case 3:
        switch (index) {
        case 0: *data = str_lang;   *len = str_lang[0];   return 1;
        case 1: *data = str_mfg;    *len = str_mfg[0];    return 1;
        case 2: *data = str_prod;   *len = str_prod[0];   return 1;
        case 3: *data = str_serial; *len = str_serial[0]; return 1;
        }
        return 0;
    default:
        return 0;
    }
}
