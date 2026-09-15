/* OpenDMO-FW - USB device core for the STM32F0 USB peripheral.
 *
 * Implements: PMA buffer management, EPnR toggle/rc_w0 logic, control transfers
 * (standard requests) and bulk data EPs. Class-specific requests and the
 * print-data stream go via the callbacks in usb_core.h.
 *
 * BRING-UP NOTE (see DECISIONS.md): the PMA access here is 1:1 (STM32F0x2). The
 * EP toggle macros follow the known ST pattern; verify on hardware with a USB
 * analyzer that enumeration and toggles are correct before driving the head.
 */
#include "usb_core.h"
#include "../mcu.h"
#include "../system.h"

/* ---- PMA (1:1 access on F0x2) ------------------------------------------- */
#define PMA ((volatile uint16_t*)USB_PMA_BASE)

/* BTABLE @ offset 0; 4 x 16-bit per EP. Bufferoffsets in PMA (bytes): */
#define BUF_EP0_TX 0x40
#define BUF_EP0_RX 0x80
#define BUF_EP1_TX 0xC0
#define BUF_EP1_RX 0x100
#define RX_COUNT_64 0x8400u   /* BLSIZE=1, NUM_BLOCK=1 -> 64 bytes */

static volatile uint16_t *btable_tx_addr(int ep){ return &PMA[ep*4 + 0]; }
static volatile uint16_t *btable_tx_cnt (int ep){ return &PMA[ep*4 + 1]; }
static volatile uint16_t *btable_rx_addr(int ep){ return &PMA[ep*4 + 2]; }
static volatile uint16_t *btable_rx_cnt (int ep){ return &PMA[ep*4 + 3]; }

static void pma_write(uint16_t off, const uint8_t *src, uint16_t n)
{
    volatile uint16_t *d = &PMA[off/2];
    for (uint16_t i = 0; i < n; i += 2) {
        uint16_t w = src[i];
        if (i + 1 < n) w |= (uint16_t)src[i+1] << 8;
        *d++ = w;
    }
}
static void pma_read(uint16_t off, uint8_t *dst, uint16_t n)
{
    volatile uint16_t *s = &PMA[off/2];
    for (uint16_t i = 0; i < n; i += 2) {
        uint16_t w = *s++;
        dst[i] = (uint8_t)w;
        if (i + 1 < n) dst[i+1] = (uint8_t)(w >> 8);
    }
}

/* ---- EPnR helpers (rc_w0 CTR-bits + toggle STAT/DTOG) -------------------
 *
 * The EPR register mixes three kinds of bit:
 *   - normal R/W : EA(3:0), STAT_TX(5:4), KIND(8), TYPE(10:9), SETUP(11), STAT_RX(13:12)
 *   - write-1-to-clear (rc_w0): CTR_TX(7), CTR_RX(15)  -- writing 0 leaves them alone
 *   - toggle     : DTOG_TX(6), DTOG_RX(14)             -- only a written 1 changes them
 *
 * So to change ONLY the two STAT bits of one direction we must: keep every normal
 * R/W bit, and write 0 to all the W1C/toggle bits (which is a no-op for them).
 * That is exactly what TinyUSB's fsdev_common.h does -- ep_change_status() XORs the
 * new value into the STAT field and ep_write() masks with U_EPREG_MASK, never
 * disturbing the other direction's STAT or the hardware-maintained DTOG bits.
 * (Earlier versions masked with EPREG_MASK|STAT_x here, which silently cleared the
 * OTHER direction's STAT to DISABLED -- e.g. after SET_CONFIGURATION the bulk OUT
 * endpoint was left disabled and could never receive print data.)
 */
#define EP_NORM_MASK 0x0F3Fu   /* EA | STAT_TX | KIND | TYPE | SETUP : normal R/W bits to keep */

/* Read-modify-write of EPR must be atomic against the USB IRQ. But these helpers
 * run from BOTH the main loop and inside USB_IRQHandler -- a blind `cpsie i` would
 * re-enable interrupts prematurely in the ISR. So save PRIMASK and only disable
 * when we are actually in thread context (PRIMASK==0). */
static inline uint32_t ep_crit_enter(void)
{
    uint32_t pm;
    __asm volatile("mrs %0, primask" : "=r"(pm));
    if (pm == 0u) __asm volatile("cpsid i" ::: "memory");
    return pm;
}
static inline void ep_crit_exit(uint32_t pm)
{
    if (pm == 0u) __asm volatile("cpsie i" ::: "memory");
}

static void ep_set_rx_stat(int n, uint16_t stat /*already in bit12:13*/)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    USB->EPR[n] = (v & EP_NORM_MASK) | (stat & USB_EP_STAT_RX);
    ep_crit_exit(pm);
}
static void ep_set_tx_stat(int n, uint16_t stat /*already in bit4:5*/)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    USB->EPR[n] = (v & EP_NORM_MASK) | (stat & USB_EP_STAT_TX);
    ep_crit_exit(pm);
}
static void ep_clear_ctr_rx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    USB->EPR[n] = (v & EP_NORM_MASK) | USB_EP_CTR_RX;   /* write-1-to-clear CTR_RX only */
    ep_crit_exit(pm);
}
static void ep_clear_ctr_tx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    USB->EPR[n] = (v & EP_NORM_MASK) | USB_EP_CTR_TX;   /* write-1-to-clear CTR_TX only */
    ep_crit_exit(pm);
}
static void ep_init(int n, uint16_t type, uint16_t ea)
{
    /* fresh endpoint: STAT/DTOG = 0, CTR-bits cleared (write 1). */
    uint32_t pm = ep_crit_enter();
    USB->EPR[n] = (type & USB_EP_TYPE) | (ea & USB_EP_EA) | USB_EP_CTR_RX | USB_EP_CTR_TX;
    ep_crit_exit(pm);
}

#define STAT_RX(s) ((uint16_t)((s) << 12))
#define STAT_TX(s) ((uint16_t)((s) << 4))

/* Re-open an RX endpoint to accept a fresh packet. On every receive the hardware
 * overwrites the CNT field with the number of bytes actually received, clobbering
 * the BLSIZE/num_block bits that define the buffer capacity -- so before the next
 * packet we must restore them (TinyUSB does this with btable_set_rx_bufsize() after
 * every RX completion) and then set STAT_RX=VALID. MPS is 64 on both EP0 and EP1. */
static void rx_reopen(int n)
{
    *btable_rx_cnt(n) = RX_COUNT_64;
    ep_set_rx_stat(n, STAT_RX(USB_EP_STAT_VALID));
}

/* DTOG is a toggle bit: write 1 to flip it. To force it back to 0 we only need to
 * toggle when it currently reads 1. Write 0 to the W1C/toggle bits we don't touch. */
static void ep_dtog_clear_tx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    if (v & USB_EP_DTOG_TX)
        USB->EPR[n] = (v & EP_NORM_MASK) | USB_EP_DTOG_TX;
    ep_crit_exit(pm);
}
static void ep_dtog_clear_rx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    if (v & USB_EP_DTOG_RX)
        USB->EPR[n] = (v & EP_NORM_MASK) | USB_EP_DTOG_RX;
    ep_crit_exit(pm);
}

/* Set/clear endpoint HALT on an EP address (bit7 = IN direction). */
static void ep_halt(uint8_t addr, int stall)
{
    int n = addr & 0x0F;
    if (addr & 0x80) ep_set_tx_stat(n, STAT_TX(stall ? USB_EP_STAT_STALL : USB_EP_STAT_NAK));
    else             ep_set_rx_stat(n, STAT_RX(stall ? USB_EP_STAT_STALL : USB_EP_STAT_VALID));
    if (!stall) { if (addr & 0x80) ep_dtog_clear_tx(n); else ep_dtog_clear_rx(n); }
}
static int ep_is_halted(uint8_t addr)
{
    int n = addr & 0x0F; uint16_t r = USB->EPR[n];
    if (addr & 0x80) return ((r & USB_EP_STAT_TX) >> 4)  == USB_EP_STAT_STALL;
    return              ((r & USB_EP_STAT_RX) >> 12) == USB_EP_STAT_STALL;
}

/* ---- state -------------------------------------------------------------- */
static uint8_t  s_pending_addr;      /* SET_ADDRESS: applied only in the status stage */
static int      s_configured;
static const uint8_t *s_ctrl_ptr;    /* in-progress control IN */
static uint16_t s_ctrl_len;

static void ctrl_tx_chunk(void)
{
    uint16_t n = s_ctrl_len > EP_MAXPKT ? EP_MAXPKT : s_ctrl_len;
    pma_write(BUF_EP0_TX, s_ctrl_ptr, n);
    *btable_tx_cnt(EP_CTRL) = n;
    s_ctrl_ptr += n;
    s_ctrl_len -= n;
    ep_set_tx_stat(EP_CTRL, STAT_TX(USB_EP_STAT_VALID));
}

void usb_ctrl_send(const uint8_t *data, uint16_t len, uint16_t wLength)
{
    if (len > wLength) len = wLength;   /* never more than requested */
    s_ctrl_ptr = data;
    s_ctrl_len = len;
    ctrl_tx_chunk();
}
void usb_ctrl_stall(void)
{
    ep_set_tx_stat(EP_CTRL, STAT_TX(USB_EP_STAT_STALL));
    ep_set_rx_stat(EP_CTRL, STAT_RX(USB_EP_STAT_STALL));
}
void usb_ctrl_ack(void)
{
    *btable_tx_cnt(EP_CTRL) = 0;
    ep_set_tx_stat(EP_CTRL, STAT_TX(USB_EP_STAT_VALID));
}

/* ---- standard requests -------------------------------------------------- */
static void handle_get_descriptor(const usb_setup_t *s)
{
    uint8_t type  = s->wValue >> 8;
    uint8_t index = s->wValue & 0xFF;
    const uint8_t *d; uint16_t len;
    if (usb_desc_get(type, index, s->wIndex, &d, &len))
        usb_ctrl_send(d, len, s->wLength);
    else
        usb_ctrl_stall();
}

static void handle_standard_setup(const usb_setup_t *s)
{
    switch (s->bRequest) {
    case 6: /* GET_DESCRIPTOR */
        handle_get_descriptor(s);
        break;
    case 5: /* SET_ADDRESS - address activates only after the status stage */
        s_pending_addr = (uint8_t)(s->wValue & 0x7F);
        usb_ctrl_ack();
        break;
    case 9: /* SET_CONFIGURATION */
        s_configured = (s->wValue != 0);
        if (s_configured) {
            /* open bulk EP1: IN (TX) and OUT (RX). */
            *btable_tx_addr(EP_DATA) = BUF_EP1_TX;
            *btable_tx_cnt (EP_DATA) = 0;
            *btable_rx_addr(EP_DATA) = BUF_EP1_RX;
            *btable_rx_cnt (EP_DATA) = RX_COUNT_64;
            ep_init(EP_DATA, USB_EP_TYPE_BULK, EP_DATA);
            ep_set_rx_stat(EP_DATA, STAT_RX(USB_EP_STAT_VALID));
            ep_set_tx_stat(EP_DATA, STAT_TX(USB_EP_STAT_NAK));
        }
        usb_class_set_configured(s_configured);
        usb_ctrl_ack();
        break;
    case 8: { /* GET_CONFIGURATION */
        static uint8_t cfg; cfg = s_configured ? 1 : 0;
        usb_ctrl_send(&cfg, 1, s->wLength);
        break; }
    case 0: { /* GET_STATUS: device -> {0,0}; endpoint -> halt-bit */
        static uint8_t st[2];
        st[0] = 0; st[1] = 0;
        if ((s->bmRequestType & 0x1F) == 2)      /* recipient = endpoint */
            st[0] = ep_is_halted((uint8_t)s->wIndex) ? 1 : 0;
        usb_ctrl_send(st, 2, s->wLength);
        break; }
    case 10: /* GET_INTERFACE */ {
        static const uint8_t alt = 0;
        usb_ctrl_send(&alt, 1, s->wLength);
        break; }
    case 11: /* SET_INTERFACE */
        usb_ctrl_ack();
        break;
    case 1: /* CLEAR_FEATURE: clear ENDPOINT_HALT (feature 0) */
        if ((s->bmRequestType & 0x1F) == 2 && s->wValue == 0)
            ep_halt((uint8_t)s->wIndex, 0);
        usb_ctrl_ack();
        break;
    case 3: /* SET_FEATURE: set ENDPOINT_HALT */
        if ((s->bmRequestType & 0x1F) == 2 && s->wValue == 0)
            ep_halt((uint8_t)s->wIndex, 1);
        usb_ctrl_ack();
        break;
    default:
        usb_ctrl_stall();
    }
}

static void handle_setup(void)
{
    usb_setup_t s;
    pma_read(BUF_EP0_RX, (uint8_t*)&s, sizeof(s));

    uint8_t typ = (s.bmRequestType >> 5) & 3;   /* 0=standard 1=class 2=vendor */
    if (typ == 0)
        handle_standard_setup(&s);
    else if (!usb_class_setup(&s))
        usb_ctrl_stall();

    /* Reopen OUT for the next SETUP/OUT (restore buffer capacity + VALID). */
    rx_reopen(EP_CTRL);
}

/* ---- bulk-EP API -------------------------------------------------------- */
int usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len)
{
    if (len > EP_MAXPKT) len = EP_MAXPKT;
    pma_write(BUF_EP1_TX, data, len);
    *btable_tx_cnt(ep) = len;
    ep_set_tx_stat(ep, STAT_TX(USB_EP_STAT_VALID));
    return len;
}
void usb_ep_rx_ready(uint8_t ep)
{
    rx_reopen(ep);
}
int usb_is_configured(void) { return s_configured; }

/* ---- IRQ ---------------------------------------------------------------- */
static void on_ctr(void)
{
    uint16_t istr = USB->ISTR;
    uint8_t  ep   = istr & USB_ISTR_EPID;
    uint16_t epr  = USB->EPR[ep];

    if (ep == EP_CTRL) {
        if (epr & USB_EP_CTR_RX) {
            int setup = epr & USB_EP_SETUP;
            ep_clear_ctr_rx(EP_CTRL);
            if (setup) handle_setup();
            else       rx_reopen(EP_CTRL);
        }
        if (epr & USB_EP_CTR_TX) {
            ep_clear_ctr_tx(EP_CTRL);
            /* SET_ADDRESS takes effect as soon as the status IN completes. */
            if (s_pending_addr) {
                USB->DADDR = USB_DADDR_EF | s_pending_addr;
                s_pending_addr = 0;
            }
            if (s_ctrl_len) ctrl_tx_chunk();     /* next descriptor chunk */
        }
    } else {
        if (epr & USB_EP_CTR_RX) {
            uint16_t cnt = *btable_rx_cnt(ep) & 0x3FF;
            static uint8_t buf[EP_MAXPKT];
            pma_read(BUF_EP1_RX, buf, cnt);
            ep_clear_ctr_rx(ep);
            usb_class_data_out(ep, buf, cnt);    /* lower layer decides when rx_ready */
        }
        if (epr & USB_EP_CTR_TX) {
            ep_clear_ctr_tx(ep);
            ep_set_tx_stat(ep, STAT_TX(USB_EP_STAT_NAK));
        }
    }
}

void USB_IRQHandler(void)
{
    uint16_t istr = USB->ISTR;

    if (istr & USB_ISTR_RESET) {
        USB->ISTR = (uint16_t)~USB_ISTR_RESET;
        USB->BTABLE = 0;
        /* Set up EP0 control. */
        *btable_tx_addr(EP_CTRL) = BUF_EP0_TX;
        *btable_tx_cnt (EP_CTRL) = 0;
        *btable_rx_addr(EP_CTRL) = BUF_EP0_RX;
        *btable_rx_cnt (EP_CTRL) = RX_COUNT_64;
        ep_init(EP_CTRL, USB_EP_TYPE_CONTROL, EP_CTRL);
        ep_set_rx_stat(EP_CTRL, STAT_RX(USB_EP_STAT_VALID));
        ep_set_tx_stat(EP_CTRL, STAT_TX(USB_EP_STAT_NAK));
        USB->DADDR = USB_DADDR_EF | 0;
        s_configured = 0;
        s_pending_addr = 0;
        return;
    }
    if (istr & USB_ISTR_CTR) on_ctr();

    if (istr & USB_ISTR_SUSP) USB->ISTR = (uint16_t)~USB_ISTR_SUSP;
    if (istr & USB_ISTR_WKUP) USB->ISTR = (uint16_t)~USB_ISTR_WKUP;
}

/* ---- init --------------------------------------------------------------- */
void usb_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;

    USB->CNTR = USB_CNTR_FRES;      /* force reset */
    delay_us(2);
    USB->CNTR = 0;                  /* out of power-down + reset */
    USB->ISTR = 0;
    USB->CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SUSPM | USB_CNTR_WKUPM;

    nvic_enable(USB_IRQn);

    /* Internal pull-up on D+ enabled -> host sees a full-speed device. */
    USB->BCDR |= USB_BCDR_DPPU;
}
