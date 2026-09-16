/* OpenDMOfw - USB device core for the STM32F0 USB peripheral.
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
#include "../pins.h"

/* ---- PMA (1:1 access on F0x2) ------------------------------------------- */
#define PMA ((volatile uint16_t*)USB_PMA_BASE)

/* BTABLE @ offset 0; 4 x 16-bit per EP. Bufferoffsets in PMA (bytes): */
#define BUF_EP0_TX 0x40
#define BUF_EP0_RX 0x80
#define BUF_DATA_TX 0xC0
#define BUF_DATA_RX 0x100
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

/* ---- EPnR helpers (RM0091: mixed R/W, rc_w0 CTR, toggle STAT/DTOG) ------
 *
 *   R/W     : EA(3:0), KIND(8), TYPE(10:9), SETUP(11)
 *   rc_w0   : CTR_TX(7), CTR_RX(15)  -- write 0 clears, write 1 is a no-op
 *   toggle  : STAT_TX(5:4), DTOG_TX(6), STAT_RX(13:12), DTOG_RX(14)
 *             -- write 1 flips, write 0 leaves unchanged
 *
 * Keep-mask is TinyUSB U_EPREG_MASK: copy R/W + CTR (write 1 preserves CTR),
 * leave STAT/DTOG 0 unless we XOR the bits we intend to toggle. Writing the
 * current STAT value as if it were R/W would toggle it (VALID->DISABLED).
 */
#define EP_KEEP 0x8F8Fu   /* CTR_RX | SETUP | TYPE | KIND | CTR_TX | EA */

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
    uint16_t wr = (uint16_t)((v & EP_KEEP) | USB_EP_CTR_RX | USB_EP_CTR_TX);
    wr ^= (uint16_t)((v & USB_EP_STAT_RX) ^ (stat & USB_EP_STAT_RX));
    USB_EPR_WRITE(n, wr);
    ep_crit_exit(pm);
}
static void ep_set_tx_stat(int n, uint16_t stat /*already in bit4:5*/)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    uint16_t wr = (uint16_t)((v & EP_KEEP) | USB_EP_CTR_RX | USB_EP_CTR_TX);
    wr ^= (uint16_t)((v & USB_EP_STAT_TX) ^ (stat & USB_EP_STAT_TX));
    USB_EPR_WRITE(n, wr);
    ep_crit_exit(pm);
}
static void ep_clear_ctr_rx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    USB_EPR_WRITE(n, (uint16_t)((v & EP_KEEP & ~USB_EP_CTR_RX) | USB_EP_CTR_TX));
    ep_crit_exit(pm);
}
static void ep_clear_ctr_tx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    USB_EPR_WRITE(n, (uint16_t)((v & EP_KEEP & ~USB_EP_CTR_TX) | USB_EP_CTR_RX));
    ep_crit_exit(pm);
}
static void ep_init(int n, uint16_t type, uint16_t ea)
{
    /* Fresh endpoint: STAT/DTOG = 0 (write 0 = no toggle from reset 0), CTR cleared. */
    uint32_t pm = ep_crit_enter();
    USB_EPR_WRITE(n, (type & USB_EP_TYPE) | (ea & USB_EP_EA));
    ep_crit_exit(pm);
}

#define STAT_RX(s) ((uint16_t)((s) << 12))
#define STAT_TX(s) ((uint16_t)((s) << 4))

/* Re-open an RX endpoint to accept a fresh packet.
 *
 * RM0091 (DocID018940 Rev 9, "OUT and SETUP packets"): on a completed reception
 * "the internal COUNT register is copied back in the COUNTn_RX location ...
 * leaving unaffected BL_SIZE and NUM_BLOCK fields, which normally do not require
 * to be re-written", and the endpoint is parked at STAT_RX = NAK. So the one
 * thing actually required here is re-arming STAT_RX = VALID. (An earlier comment
 * claimed reception clobbers BL_SIZE/NUM_BLOCK; it does not.)
 *
 * Restoring RX_COUNT_64 is kept as belt-and-braces, but note what it implies:
 * the 16-bit store also zeroes COUNTn_RX[9:0], so this must never run before
 * the received byte count has been read. on_ctr() reads it first, EP_DATA's
 * reopen is deferred to usb_ep_rx_ready(), and handle_setup() copies the SETUP
 * packet out before reopening. Keep that ordering. MPS is 64 on both EPs. */
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
    uint16_t wr = (uint16_t)((v & EP_KEEP) | USB_EP_CTR_RX | USB_EP_CTR_TX);
    if (v & USB_EP_DTOG_TX)
        wr |= USB_EP_DTOG_TX;          /* write 1 toggles DTOG_TX back to 0 */
    USB_EPR_WRITE(n, wr);
    ep_crit_exit(pm);
}
static void ep_dtog_clear_rx(int n)
{
    uint32_t pm = ep_crit_enter();
    uint16_t v = USB->EPR[n];
    uint16_t wr = (uint16_t)((v & EP_KEEP) | USB_EP_CTR_RX | USB_EP_CTR_TX);
    if (v & USB_EP_DTOG_RX)
        wr |= USB_EP_DTOG_RX;
    USB_EPR_WRITE(n, wr);
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
static int      s_ctrl_zlp;          /* send a ZLP after an exact MPS last packet */

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
    /* Short packet that is an exact multiple of MPS needs a ZLP so the host
     * knows the transfer ended (USB 2.0 5.5.3), when wLength was larger. */
    s_ctrl_zlp = (len < wLength) && (len != 0) && ((len % EP_MAXPKT) == 0);
    ctrl_tx_chunk();
}
static int s_ctrl_stalled;           /* the current SETUP was answered with STALL */

void usb_ctrl_stall(void)
{
    ep_set_tx_stat(EP_CTRL, STAT_TX(USB_EP_STAT_STALL));
    ep_set_rx_stat(EP_CTRL, STAT_RX(USB_EP_STAT_STALL));
    s_ctrl_stalled = 1;
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
            /* open the bulk pair (EP_DATA): IN (TX) and OUT (RX). */
            *btable_tx_addr(EP_DATA) = BUF_DATA_TX;
            *btable_tx_cnt (EP_DATA) = 0;
            *btable_rx_addr(EP_DATA) = BUF_DATA_RX;
            *btable_rx_cnt (EP_DATA) = RX_COUNT_64;
            ep_init(EP_DATA, USB_EP_TYPE_BULK, EP_DATA);
            /* RM0091 30.6.2 on DTOG_RX/DTOG_TX: "This bit can also be toggled
             * by the software to initialize its value (mandatory when the
             * endpoint is not a control one)." A re-configuration without this
             * leaves the toggle wherever the previous session left it, and the
             * host restarts from DATA0. */
            ep_dtog_clear_tx(EP_DATA);
            ep_dtog_clear_rx(EP_DATA);
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
    case 0: { /* GET_STATUS: device -> self-powered bit; endpoint -> halt-bit */
        static uint8_t st[2];
        uint8_t recip = s->bmRequestType & 0x1F;
        st[0] = 0; st[1] = 0;
        if (recip == 0)      st[0] = 0x01;       /* device: bit0 = self-powered */
        else if (recip == 2) st[0] = ep_is_halted((uint8_t)s->wIndex) ? 1 : 0;
        /* recipient = interface: always {0,0} */
        usb_ctrl_send(st, 2, s->wLength);
        break; }
    case 10: /* GET_INTERFACE */ {
        static const uint8_t alt = 0;
        usb_ctrl_send(&alt, 1, s->wLength);
        break; }
    case 11: /* SET_INTERFACE - same DTOG reset rule as SET_CONFIGURATION */
        if (s_configured) {
            ep_dtog_clear_tx(EP_DATA);
            ep_dtog_clear_rx(EP_DATA);
        }
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
    s_ctrl_stalled = 0;

    uint8_t typ = (s.bmRequestType >> 5) & 3;   /* 0=standard 1=class 2=vendor */
    if (typ == 0)
        handle_standard_setup(&s);
    else if (!usb_class_setup(&s))
        usb_ctrl_stall();

    /* Reopen OUT for the next SETUP/OUT - but not after a STALL. rx_reopen()
     * would toggle EP0's RX side straight back out of the STALL just set, so an
     * unsupported control-OUT request would have its data ACKed and dropped
     * instead of stalled. Leaving RX stalled is safe: RM0091 30.5.2 - a SETUP is
     * accepted whatever STAT_RX says, and the hardware resets both directions
     * to NAK on its arrival. */
    *btable_rx_cnt(EP_CTRL) = RX_COUNT_64;
    if (!s_ctrl_stalled) rx_reopen(EP_CTRL);
}

/* ---- bulk-EP API -------------------------------------------------------- */
/* There is one PMA TX buffer per endpoint, so a second reply written before the
 * host has collected the first would silently overwrite it (e.g. "ESC A ESC U"
 * arriving in one 64-byte packet: protocol_task() answers both back-to-back).
 * STAT_TX stays VALID until the IN transfer completes, so wait for that first.
 * The wait is bounded: a host that stops polling must not stall the print loop.
 * Returns the number of bytes queued, or 0 if the previous reply was still
 * outstanding (dropping one reply beats sending a corrupted one). */
#define EP_TX_WAIT_MS 50u

static int ep_tx_busy(int n)
{
    return ((USB->EPR[n] & USB_EP_STAT_TX) >> 4) == USB_EP_STAT_VALID;
}

int usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len)
{
    uint32_t t0 = millis();
    if (len > EP_MAXPKT) len = EP_MAXPKT;
    while (ep_tx_busy(ep)) {
        wdt_kick();
        if ((millis() - t0) > EP_TX_WAIT_MS) return 0;
    }
    pma_write(BUF_DATA_TX, data, len);
    *btable_tx_cnt(ep) = len;
    ep_set_tx_stat(ep, STAT_TX(USB_EP_STAT_VALID));
    return len;
}
void usb_ep_rx_ready(uint8_t ep)
{
    rx_reopen(ep);
}
int usb_is_configured(void) { return s_configured; }

/* Printer-class SOFT_RESET, USB Printer Class 1.1 section 4.2.3: "flushes all
 * buffers and resets the Bulk OUT and Bulk IN pipes to their default states.
 * This request clears all stall conditions." protocol_reset() already re-arms
 * the OUT side; this is the IN side: drop a queued reply (else the first IN
 * after the reset returns pre-reset data) and clear a host-set IN STALL.
 * Deliberately NOT touching DTOG: a class request is not a configuration event
 * (USB 2.0 8.5.2), and Linux usblp issues SOFT_RESET without resetting its own
 * toggle, so zeroing ours would desynchronise it. */
void usb_ep_flush_in(uint8_t ep)
{
    if (!s_configured) return;            /* BTABLE entry not set up yet */
    ep_set_tx_stat(ep, STAT_TX(USB_EP_STAT_NAK));
    *btable_tx_cnt(ep) = 0;
}

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
            else if (s_ctrl_zlp) {
                s_ctrl_zlp = 0;
                *btable_tx_cnt(EP_CTRL) = 0;
                ep_set_tx_stat(EP_CTRL, STAT_TX(USB_EP_STAT_VALID));
            }
        }
    } else {
        if (epr & USB_EP_CTR_RX) {
            uint16_t cnt = *btable_rx_cnt(ep) & 0x3FF;
            static uint8_t buf[EP_MAXPKT];
            pma_read(BUF_DATA_RX, buf, cnt);
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
        s_ctrl_len = 0;
        s_ctrl_zlp = 0;
        return;
    }
    if (istr & USB_ISTR_CTR) on_ctr();

    /* ES0223 Rev 6 section 2.15.2 "ESOF interrupt timing desynchronized after
     * resume signaling" (all F072 revisions): after the DEVICE signals resume,
     * the core allows only 2 ms instead of 3 ms before the first SOF and can
     * raise a spurious SUSP. Not reachable here: bmAttributes 0xC0 leaves the
     * remote-wakeup bit clear and USB_CNTR.RESUME is never set, so only
     * host-initiated resume occurs, which the erratum does not cover.
     * If remote wakeup is ever added, mask SUSP for 3 ms after driving RESUME.
     * Careful: ST's text says "set SUSPM to mask", but in RM0091 SUSPM is the
     * interrupt ENABLE, so in these registers that means CLEAR SUSPM for 3 ms,
     * then set it again. Remote wakeup would also need bmAttributes 0xE0,
     * GET_STATUS bit 1, and SET/CLEAR_FEATURE DEVICE_REMOTE_WAKEUP honoured. */
    if (istr & USB_ISTR_WKUP) {
        USB->CNTR &= ~USB_CNTR_FSUSP;
        USB->ISTR = (uint16_t)~USB_ISTR_WKUP;
    }
    if (istr & USB_ISTR_SUSP) {
        USB->ISTR = (uint16_t)~USB_ISTR_SUSP;
        USB->CNTR |= USB_CNTR_FSUSP;
    }
}

/* ---- init --------------------------------------------------------------- */
void usb_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;

    /* PA11/PA12 carry USB_DM/USB_DP as *additional* functions (DocID025004
     * Rev 2, Table 13), which the datasheet legend defines as "directly
     * selected/enabled through peripheral registers" - they are not in Table 14,
     * where AF2 on PA11/PA12 is TIM1_CH4/TIM1_ETR. The transceiver is really
     * enabled by clearing PDWN and setting BCDR.DPPU below; this AF write
     * mirrors ST's HAL, which calls it "optional, and maintained only for user
     * guidance". Harmless: TIM1 is never clocked in this firmware. */
    gpio_af((pin_t){GPIOA, 11}, 2);
    gpio_af((pin_t){GPIOA, 12}, 2);
    GPIOA->OSPEEDR |= (3u << (11 * 2)) | (3u << (12 * 2));

    /* RM0091: come out of PDWN, stay in reset, then release FRES. */
    USB->CNTR = USB_CNTR_FRES | USB_CNTR_PDWN;
    USB->CNTR = USB_CNTR_FRES;
    delay_us(100);
    USB->CNTR = 0;
    USB->ISTR = 0;
    USB->CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SUSPM | USB_CNTR_WKUPM;

    nvic_enable(USB_IRQn);

    /* Internal pull-up on D+ enabled -> host sees a full-speed device. */
    USB->BCDR |= USB_BCDR_DPPU;
}
