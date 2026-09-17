/* OpenDMOfw - register-level STM32 I2C-v2 controller + 24Cxx part model.
 *
 * store.c is the only file in the tree that talks to a device on a bus, and
 * the only one whose failures are SILENT: a 24Cxx that is write-protected, or
 * absent, or torn by a power loss, answers the bus in ways that look like
 * success to code that only checks whether the transfers were ACKed.
 *
 * This header models the bus at the register level, so the REAL store.c is
 * compiled and run unmodified (D34: mock the boundary, never the behaviour).
 * The boundary is the I2C peripheral; everything above it - the addressing
 * probe, the page splitting, the checksum, the read-back verify - is the code
 * under test.
 *
 * What the part model implements, from the AT24C01D/02D and BL24C128A
 * datasheets:
 *   - the internal address counter is loaded by the address byte(s) and, in a
 *     page write, its LOW page bits increment on every data byte received;
 *   - data bytes land in a PAGE BUFFER and are committed only on a STOP. A
 *     repeated START discards the buffer. This is what makes store.c's 2-byte
 *     probe harmless on a 1-byte part, and Renode's GenericI2cEeprom - which
 *     commits every byte as it arrives - cannot express it (see the docstring
 *     of test/renode/eeprom.py, which lists that scenario as untestable);
 *   - a write that crosses a page boundary WRAPS inside the page;
 *   - the part NAKs its own address while an internal write cycle is running.
 *
 * Misbehaviours the part can be told to perform: absent, /WP asserted,
 * read-as-0x00, NAK at the Nth byte of a write, a dead bus that never moves a
 * flag, and a torn write that stops committing after N pages.
 */
#ifndef OPENDMO_I2C_EEPROM_MODEL_H
#define OPENDMO_I2C_EEPROM_MODEL_H

#include <stdint.h>
#include <string.h>
#include "mcu.h"

#define EE_MAX 16384

typedef struct {
    /* --- part configuration ------------------------------------------- */
    int      present;      /* 0: nothing on the bus, the address NAKs        */
    int      addr_bytes;   /* internal address width the PART expects (1|2)  */
    uint16_t size;         /* array size in bytes                            */
    uint16_t page;         /* page size in bytes                             */
    int      wp;           /* /WP asserted: ACKs everything, commits nothing */
    int      read_zero;    /* reads return 0x00 whatever is stored           */
    int      bad_cell;     /* address of a cell that reads back flipped
                              (a worn bit); -1 = none                        */
    int      nak_at;       /* NAK the Nth byte of a write phase (address
                              bytes are 0..addr_bytes-1); -1 = never         */
    int      dead;         /* never move a flag (SCL held low by the part)   */
    int      commit_pages; /* commit at most this many page writes, then stop
                              (a power loss mid-record); -1 = unlimited      */
    int      twr_ms;       /* internal write cycle; the part NAKs its address
                              until this many virtual ms have passed. 0 = off*/
    int      ptr_datainc;  /* 1 = a received data byte advances the address
                              counter inside the page (datasheet behaviour);
                              0 = only the address byte sets the counter      */

    /* --- observation --------------------------------------------------- */
    int      pages_committed;
    int      stops;
    int      addr_naks;
    int      data_naks;
    int      starts;

    uint8_t  mem[EE_MAX];
} ee_part_t;

/* Virtual millisecond clock; the harness' delay_ms() advances it. */
extern uint32_t g_ms;

typedef struct {
    I2C_Type   r;         /* the registers the firmware sees                */
    I2C_Type   shadow;    /* last value we handed out, to detect writes     */
    ee_part_t *p;

    int      active;      /* a transfer is in progress                      */
    int      dir;         /* 0 = write, 1 = read                            */
    int      autoend;
    int      nbytes;
    int      wcount;      /* bytes received so far in this write phase      */
    int      selected;    /* the part ACKed its address in this transfer    */
    int      rx_presented;/* RXDR holds a byte the firmware has not taken   */

    uint16_t ptr;         /* the part's internal address counter            */
    int      buf_open;    /* a page buffer is pending                       */
    uint16_t buf_base;
    uint8_t  buf[64];
    uint64_t buf_mask;
    uint32_t busy_until;  /* virtual ms at which the write cycle ends       */
} host_i2c_t;

extern host_i2c_t g_i2c;

/* ---- part helpers -------------------------------------------------------- */

static void ee_reset_part(ee_part_t *p, int addr_bytes, uint16_t size, uint16_t page)
{
    memset(p, 0, sizeof(*p));
    p->present = 1;
    p->addr_bytes = addr_bytes;
    p->size = size;
    p->page = page;
    p->nak_at = -1;
    p->bad_cell = -1;
    p->commit_pages = -1;
    p->ptr_datainc = 1;
    memset(p->mem, 0xFF, sizeof(p->mem));   /* an erased 24Cxx reads 0xFF */
}

static void i2c_buf_discard(host_i2c_t *h)
{
    h->buf_open = 0;
    h->buf_mask = 0;
}

static void i2c_buf_commit(host_i2c_t *h)
{
    ee_part_t *p = h->p;
    if (!h->buf_open) return;
    if (!p->wp &&
        (p->commit_pages < 0 || p->pages_committed < p->commit_pages)) {
        for (unsigned i = 0; i < (unsigned)p->page && i < 64u; i++) {
            if (h->buf_mask & (1ull << i)) {
                uint16_t a = (uint16_t)((h->buf_base + i) % p->size);
                p->mem[a] = h->buf[i];
            }
        }
        if (p->twr_ms) h->busy_until = g_ms + (uint32_t)p->twr_ms;
    }
    if (!p->wp) p->pages_committed++;
    i2c_buf_discard(h);
}

static void i2c_stop(host_i2c_t *h)
{
    if (h->active && h->dir == 0) i2c_buf_commit(h);
    else i2c_buf_discard(h);
    h->p->stops++;
    h->active = 0;
    h->selected = 0;
    h->rx_presented = 0;
    h->r.ISR |= I2C_ISR_STOPF;
    h->r.ISR &= ~(uint32_t)(I2C_ISR_BUSY | I2C_ISR_TXIS | I2C_ISR_RXNE | I2C_ISR_TC);
}

/* A data or address byte was NAKed: AUTOEND makes the peripheral generate the
 * STOP itself, which is what store.c's transfers all request. */
static void i2c_nak(host_i2c_t *h)
{
    h->r.ISR |= I2C_ISR_NACKF;
    h->r.ISR &= ~(uint32_t)(I2C_ISR_TXIS | I2C_ISR_RXNE | I2C_ISR_TC);
    i2c_buf_discard(h);
    h->active = 0;
    h->selected = 0;
    h->r.ISR &= ~(uint32_t)I2C_ISR_BUSY;
    h->r.ISR |= I2C_ISR_STOPF;      /* AUTOEND: STOP after a NAK */
    h->p->stops++;
}

static void i2c_present_rx(host_i2c_t *h)
{
    ee_part_t *p = h->p;
    uint16_t a = (uint16_t)(h->ptr % p->size);
    uint8_t v = p->read_zero ? 0x00u : p->mem[a];
    if (p->bad_cell >= 0 && a == (uint16_t)p->bad_cell) v = (uint8_t)(v ^ 0xFFu);
    h->r.RXDR = v;
    h->r.ISR |= I2C_ISR_RXNE;
    h->rx_presented = 1;
}

static void i2c_start(host_i2c_t *h, uint32_t cr2)
{
    ee_part_t *p = h->p;
    int repeated = h->active;

    h->p->starts++;
    if (repeated) i2c_buf_discard(h);   /* a repeated START voids a page write */

    h->dir     = (cr2 & I2C_CR2_RD_WRN) ? 1 : 0;
    h->autoend = (cr2 & I2C_CR2_AUTOEND) ? 1 : 0;
    h->nbytes  = (int)((cr2 >> 16) & 0xFFu);
    h->wcount  = 0;
    h->active  = 1;
    h->rx_presented = 0;
    h->r.ISR |= I2C_ISR_BUSY;
    h->r.ISR &= ~(uint32_t)(I2C_ISR_TXIS | I2C_ISR_RXNE | I2C_ISR_TC | I2C_ISR_STOPF);

    uint8_t addr7 = (uint8_t)((cr2 >> 1) & 0x7Fu);
    int busy = (p->twr_ms && (int32_t)(h->busy_until - g_ms) > 0);
    if (!p->present || addr7 != 0x50u || busy) {
        p->addr_naks++;
        i2c_nak(h);
        return;
    }
    h->selected = 1;
    if (h->dir) {
        if (h->nbytes > 0) i2c_present_rx(h);
        else i2c_stop(h);
    } else {
        if (h->nbytes > 0) h->r.ISR |= I2C_ISR_TXIS;
        else if (h->autoend) i2c_stop(h);
        else h->r.ISR |= I2C_ISR_TC;
    }
}

static void i2c_rx_byte(host_i2c_t *h, uint8_t b)
{
    ee_part_t *p = h->p;

    if (p->nak_at >= 0 && h->wcount == p->nak_at) {
        p->data_naks++;
        i2c_nak(h);
        return;
    }

    if (h->wcount < p->addr_bytes) {
        if (p->addr_bytes == 2)
            h->ptr = (uint16_t)((h->wcount == 0) ? ((uint32_t)b << 8)
                                                 : ((uint32_t)(h->ptr & 0xFF00u) | b));
        else
            h->ptr = b;
        h->ptr = (uint16_t)(h->ptr % p->size);
    } else {
        uint16_t base = (uint16_t)(h->ptr - (h->ptr % p->page));
        if (!h->buf_open || base != h->buf_base) {
            i2c_buf_discard(h);
            h->buf_open = 1;
            h->buf_base = base;
            memset(h->buf, 0, sizeof(h->buf));
        }
        unsigned off = (unsigned)(h->ptr % p->page);
        h->buf[off] = b;
        h->buf_mask |= (1ull << off);
        if (p->ptr_datainc)
            h->ptr = (uint16_t)(base + ((off + 1u) % p->page));
    }
    h->wcount++;

    h->nbytes--;
    if (h->nbytes <= 0) {
        h->r.ISR &= ~(uint32_t)I2C_ISR_TXIS;
        if (h->autoend) i2c_stop(h);
        else h->r.ISR |= I2C_ISR_TC;
    } else {
        h->r.ISR |= I2C_ISR_TXIS;
    }
}

static void i2c_take_rx(host_i2c_t *h)
{
    ee_part_t *p = h->p;
    h->rx_presented = 0;
    h->ptr = (uint16_t)((h->ptr + 1u) % p->size);
    h->nbytes--;
    h->r.ISR &= ~(uint32_t)I2C_ISR_RXNE;
    if (h->nbytes > 0) i2c_present_rx(h);
    else if (h->autoend) i2c_stop(h);
    else h->r.ISR |= I2C_ISR_TC;
}

/* Called on EVERY access to I2C1. Writes are detected by diffing the block
 * against the copy handed out last time; TXDR carries a sentinel above 0xFF so
 * that writing the same byte twice is still seen as two writes. */
I2C_Type *host_i2c(void)
{
    host_i2c_t *h = &g_i2c;

    /* RXDR was presented; the model has no latency, so wait(RXNE) always
     * returns on its first ISR read and the access after it is the fetch
     * `r[i] = I2C1->RXDR`. State 1 = presented, this call IS the fetch (leave
     * RXDR alone); state 2 = fetched, the next access is the following ISR
     * poll, so advance the part now. */
    if (h->rx_presented == 2) {
        h->rx_presented = 0;
        i2c_take_rx(h);
        h->shadow = h->r;
        return &h->r;
    }
    if (h->rx_presented == 1) {
        h->rx_presented = 2;
        h->shadow = h->r;
        return &h->r;
    }

    if (h->r.CR1 != h->shadow.CR1) {
        if (!(h->r.CR1 & I2C_CR1_PE)) {
            /* PE=0 resets the peripheral: every ISR flag clears. */
            h->r.ISR = 0;
            h->active = 0; h->selected = 0; h->rx_presented = 0;
            i2c_buf_discard(h);
        }
    }

    if (h->r.ICR != h->shadow.ICR && h->r.ICR != 0) {
        if (h->r.ICR & I2C_ICR_NACKCF) h->r.ISR &= ~(uint32_t)I2C_ISR_NACKF;
        if (h->r.ICR & I2C_ICR_STOPCF) h->r.ISR &= ~(uint32_t)I2C_ISR_STOPF;
        h->r.ICR = 0;
    }

    if (h->r.TXDR <= 0xFFu) {
        uint8_t b = (uint8_t)h->r.TXDR;
        h->r.TXDR = 0x10000u;               /* sentinel: "empty" */
        if (h->active && h->selected && h->dir == 0 && !h->p->dead) i2c_rx_byte(h, b);
    }

    if (h->r.CR2 != h->shadow.CR2) {
        uint32_t cr2 = h->r.CR2;
        h->r.CR2 = cr2 & ~(uint32_t)(I2C_CR2_START | I2C_CR2_STOP);
        if (!h->p->dead) {
            if (cr2 & I2C_CR2_START) i2c_start(h, cr2);
            else if (cr2 & I2C_CR2_STOP) { if (h->active) i2c_stop(h); }
        } else if (cr2 & I2C_CR2_START) {
            h->r.ISR |= I2C_ISR_BUSY;       /* the bus hangs: no flag ever moves */
        }
    }

    h->shadow = h->r;
    return &h->r;
}

static void i2c_model_init(ee_part_t *p)
{
    memset(&g_i2c, 0, sizeof(g_i2c));
    g_i2c.p = p;
    g_i2c.r.TXDR = 0x10000u;
    g_i2c.shadow = g_i2c.r;
    g_ms = 1;
}

#endif /* OPENDMO_I2C_EEPROM_MODEL_H */
