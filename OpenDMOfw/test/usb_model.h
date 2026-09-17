/* OpenDMOfw - software model of the STM32F0 USB device peripheral plus a
 * scripted USB host, shared by test/test_usb.c and test/test_e2e.c. See the
 * header comment of test_usb.c for the register semantics it implements.
 * Include once per test program (it defines the peripheral's backing RAM). */
#ifndef OPENDMO_USB_MODEL_H
#define OPENDMO_USB_MODEL_H
/* Not every test uses every helper. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include <string.h>
#include "mcu.h"
#include "usb/usb_core.h"
#include "host_periph.h"

/* ---- peripheral model --------------------------------------------------- */
USB_Type host_usb;
uint16_t host_pma[512];
uint32_t host_uid[3] = { 0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu };

void USB_IRQHandler(void);

#define RW_BITS     (USB_EP_TYPE | USB_EP_KIND | USB_EP_EA)
#define TOGGLE_BITS (USB_EP_DTOG_RX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_STAT_TX)
#define CTR_BITS    (USB_EP_CTR_RX | USB_EP_CTR_TX)

void host_epr_write(int n, uint16_t v)
{
    uint16_t old = (uint16_t)host_usb.EPR[n];
    uint16_t nv  = (uint16_t)((v & RW_BITS)
                 | (old & USB_EP_SETUP)
                 | (old & v & CTR_BITS)
                 | ((old ^ (v & TOGGLE_BITS)) & TOGGLE_BITS));
    host_usb.EPR[n] = nv;
}

static uint16_t epr(int n)          { return (uint16_t)host_usb.EPR[n]; }
static int stat_tx(int n)           { return (epr(n) >> 4) & 3; }
static int stat_rx(int n)           { return (epr(n) >> 12) & 3; }
static int dtog_tx(int n)           { return !!(epr(n) & USB_EP_DTOG_TX); }
static int dtog_rx(int n)           { return !!(epr(n) & USB_EP_DTOG_RX); }
/* Hardware-side register update (not subject to the write semantics). */
static void hw_set(int n, uint16_t clear, uint16_t set)
{
    host_usb.EPR[n] = (uint16_t)((epr(n) & ~clear) | set);
}
static void hw_stat_rx(int n, int s) { hw_set(n, USB_EP_STAT_RX, (uint16_t)(s << 12)); }
static void hw_stat_tx(int n, int s) { hw_set(n, USB_EP_STAT_TX, (uint16_t)(s << 4)); }

static uint16_t bt_tx_addr(int n) { return host_pma[n * 4 + 0]; }
static uint16_t bt_tx_cnt (int n) { return host_pma[n * 4 + 1]; }
static uint16_t bt_rx_addr(int n) { return host_pma[n * 4 + 2]; }

static void pma_put(uint16_t off, const uint8_t *d, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint16_t *w = &host_pma[(off + i) / 2];
        if ((off + i) & 1) *w = (uint16_t)((*w & 0x00FF) | (d[i] << 8));
        else               *w = (uint16_t)((*w & 0xFF00) | d[i]);
    }
}
static void pma_get(uint16_t off, uint8_t *d, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint16_t w = host_pma[(off + i) / 2];
        d[i] = (uint8_t)(((off + i) & 1) ? (w >> 8) : w);
    }
}

static void irq(int ep, uint16_t flags)
{
    host_usb.ISTR = (uint16_t)(flags | (ep & USB_ISTR_EPID));
    USB_IRQHandler();
    host_usb.ISTR = 0;
}

/* RX capacity from COUNTn_RX: BL_SIZE=1 -> 32-byte blocks, NUM_BLOCK+1 blocks
 * ... RM0091 encodes NUM_BLOCK blocks for BL_SIZE=1 as (NUM_BLOCK+1)*32. */
static int rx_capacity(int n)
{
    uint16_t c = host_pma[n * 4 + 3];
    int nb = (c >> 10) & 0x1F;
    return (c & 0x8000) ? (nb + 1) * 32 : nb * 2;
}

#define NAK   (-1)
#define STALL (-2)
#define OVERRUN (-3)

static int host_setup(const uint8_t s[8])
{
    uint16_t cnt = host_pma[0 * 4 + 3];
    pma_put(bt_rx_addr(0), s, 8);
    host_pma[0 * 4 + 3] = (uint16_t)((cnt & 0xFC00) | 8);
    hw_stat_rx(0, USB_EP_STAT_NAK);
    hw_stat_tx(0, USB_EP_STAT_NAK);
    hw_set(0, 0, USB_EP_CTR_RX | USB_EP_SETUP | USB_EP_DTOG_RX | USB_EP_DTOG_TX);
    irq(0, USB_ISTR_CTR);
    hw_set(0, USB_EP_SETUP, 0);
    return 0;
}

static int host_in(int ep, uint8_t *buf, int max)
{
    int st = stat_tx(ep);
    if (st == USB_EP_STAT_STALL) return STALL;
    if (st != USB_EP_STAT_VALID) return NAK;
    int len = bt_tx_cnt(ep) & 0x3FF;
    if (len > max) len = max;
    pma_get(bt_tx_addr(ep), buf, (uint16_t)len);
    hw_stat_tx(ep, USB_EP_STAT_NAK);
    hw_set(ep, 0, USB_EP_CTR_TX);
    host_usb.EPR[ep] ^= USB_EP_DTOG_TX;
    irq(ep, USB_ISTR_CTR | USB_ISTR_DIR * 0);
    return len;
}

static int host_out(int ep, const uint8_t *data, int len)
{
    int st = stat_rx(ep);
    if (st == USB_EP_STAT_STALL) return STALL;
    if (st != USB_EP_STAT_VALID) return NAK;
    if (len > rx_capacity(ep)) return OVERRUN;
    uint16_t cnt = host_pma[ep * 4 + 3];
    pma_put(bt_rx_addr(ep), data, (uint16_t)len);
    host_pma[ep * 4 + 3] = (uint16_t)((cnt & 0xFC00) | len);
    hw_stat_rx(ep, USB_EP_STAT_NAK);
    hw_set(ep, 0, USB_EP_CTR_RX);
    host_usb.EPR[ep] ^= USB_EP_DTOG_RX;
    irq(ep, USB_ISTR_CTR | USB_ISTR_DIR);
    return 0;
}

static void mk_setup(uint8_t *s, uint8_t type, uint8_t req, uint16_t val,
                     uint16_t idx, uint16_t len)
{
    s[0] = type; s[1] = req;
    s[2] = (uint8_t)val; s[3] = (uint8_t)(val >> 8);
    s[4] = (uint8_t)idx; s[5] = (uint8_t)(idx >> 8);
    s[6] = (uint8_t)len; s[7] = (uint8_t)(len >> 8);
}

/* Control read: SETUP, IN data until short packet or wLength, OUT status ZLP.
 * Returns bytes read, or STALL. */
static int ctrl_in(uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
                   uint16_t wlen, uint8_t *buf)
{
    uint8_t s[8]; mk_setup(s, type, req, val, idx, wlen);
    host_setup(s);
    int total = 0;
    for (;;) {
        int r = host_in(0, buf + total, 64);
        if (r == STALL) return STALL;
        if (r < 0) return -100;               /* device never answered */
        total += r;
        if (r < 64 || total >= wlen) break;
    }
    if (host_out(0, 0, 0) != 0) return -101;  /* status stage not accepted */
    return total;
}

/* Control write without data: SETUP, IN status ZLP. Returns 0 or STALL. */
static int ctrl_nodata(uint8_t type, uint8_t req, uint16_t val, uint16_t idx)
{
    uint8_t s[8]; mk_setup(s, type, req, val, idx, 0);
    host_setup(s);
    uint8_t b[64];
    int r = host_in(0, b, 64);
    if (r == STALL) return STALL;
    return r == 0 ? 0 : -102;
}


#endif
