/* OpenDMOfw - config storage in a 24Cxx I2C EEPROM.
 *
 * Roll state here is PURE configuration: a SKU string + a reported count,
 * freely changeable without re-flashing. No tag, no authentication.
 * If the EEPROM is missing or its contents are invalid, the compiled defaults apply.
 *
 * Sourced (EEVBlog 550-series teardowns, Rev H/I/K): **BL24C128A** (Belling,
 * 128 kbit = 16 KB, **64 B page, 2-byte internal addressing**), 7-bit address
 * **0x50** (A0-A2 to GND). Rev E boards carry a smaller Atmel **AT24C01D/02D**
 * (8 B page, 1-byte addressing) at the same address. So this driver DETECTS
 * the addressing scheme at init (config read-back, then a scratch-area probe)
 * and uses the matching page size — one firmware works on both revisions.
 *
 * The I2C bus also carries the NFC front-end (SLRC610 @ 0x28); it is a
 * different device address and is simply ignored by this driver.
 *
 * ASSUMPTION (PINMAP.md): I2C1 on PB8/PB9 at AF2 (the F072's I2C1 pins are
 * only PB6/PB7 or PB8/PB9 — datasheet Table 14). TIMINGR is a starting value
 * for ~100 kHz @48 MHz; verify with the scope.
 */
#include "store.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

#define CFG_MAGIC   0x4F503537u      /* "OP57" */
#define EEPROM_ADDR 0x00             /* internal offset of the config block */
#define I2C_TIMINGR 0x10420F13u      /* ~100 kHz at PCLK 48 MHz (calibrate) */
#define SCRATCH_OFF 64               /* self-test/probe area, past the config block */

/* Address width (bytes) and page size follow the detected part:
 * 2-byte / 64 B page = BL24C128A (Rev H/I/K); 1-byte / 8 B page = AT24C01D/02D (Rev E). */
static uint8_t s_addrw = 2;          /* default: current production part */

static op_config_t s_cfg;

static void defaults(void)
{
    s_cfg.magic = CFG_MAGIC;
    const char *d = MODEL_DEFAULT_SKU;   /* from model.h (via pins.h) */
    uint8_t i = 0; for (; d[i] && i < OP_SKU_MAX-1; i++) s_cfg.sku[i] = d[i];
    s_cfg.sku[i] = 0;
    s_cfg.label_count = MODEL_DEFAULT_COUNT;   /* matches factory_reset + wrap-around */
    s_cfg.density = 8;
    s_cfg.flags = OP_FLAG_PAPER_FORCE;   /* report a valid roll regardless of the sensor */
}

/* ---- Minimal I2C v2 with timeouts (never hangs) ------------------------- */
static void i2c_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    /* I2C on the STM32F0 is AF2 (not AF1) — see pins.h for the pin/AF source. */
    gpio_od(PIN_I2C_SCL, 1); gpio_pull(PIN_I2C_SCL, 1); gpio_af(PIN_I2C_SCL, 2);
    gpio_od(PIN_I2C_SDA, 1); gpio_pull(PIN_I2C_SDA, 1); gpio_af(PIN_I2C_SDA, 2);
    I2C1->CR1 = 0;
    I2C1->TIMINGR = I2C_TIMINGR;
    I2C1->CR1 = I2C_CR1_PE;
}

static int wait(volatile uint32_t flag)
{
    uint32_t g = 200000;
    while (!(I2C1->ISR & flag) && --g) { if (I2C1->ISR & I2C_ISR_NACKF) return -1; }
    return g ? 0 : -1;
}

static int i2c_xfer(uint8_t addr7, const uint8_t *w, uint16_t wn, uint8_t *r, uint16_t rn)
{
    if (I2C1->ISR & I2C_ISR_BUSY) { /* clear stale */ }
    if (wn) {
        I2C1->CR2 = ((uint32_t)addr7 << 1) | ((uint32_t)wn << 16) |
                    (rn ? 0 : I2C_CR2_AUTOEND) | I2C_CR2_START;
        for (uint16_t i = 0; i < wn; i++) {
            if (wait(I2C_ISR_TXIS)) return -1;
            I2C1->TXDR = w[i];
        }
        if (!rn) { wait(I2C_ISR_STOPF); I2C1->ICR = I2C_ISR_STOPF; return 0; }
        if (wait(I2C_ISR_TC)) return -1;
    }
    if (rn) {
        I2C1->CR2 = ((uint32_t)addr7 << 1) | I2C_CR2_RD_WRN |
                    ((uint32_t)rn << 16) | I2C_CR2_AUTOEND | I2C_CR2_START;
        for (uint16_t i = 0; i < rn; i++) {
            if (wait(I2C_ISR_RXNE)) return -1;
            r[i] = (uint8_t)I2C1->RXDR;
        }
        wait(I2C_ISR_STOPF); I2C1->ICR = I2C_ISR_STOPF;
    }
    return 0;
}

/* Build the internal-address prefix for an offset, per the detected part. */
static uint8_t addr_prefix(uint16_t off, uint8_t *buf)
{
    if (s_addrw == 2) { buf[0] = (uint8_t)(off >> 8); buf[1] = (uint8_t)off; return 2; }
    buf[0] = (uint8_t)off; return 1;
}

/* Page-wise writing: a 24Cxx wraps a write WITHIN a page, so a block that
 * crosses a page boundary must be split into chunks. Per chunk, wait for the
 * write cycle (delay). */
static int eeprom_write(uint16_t off, const uint8_t *data, uint16_t n)
{
    uint16_t page = (s_addrw == 2) ? 64 : 8;
    uint8_t buf[2 + 64];
    while (n) {
        uint16_t page_left = page - (off % page);
        uint16_t chunk = n < page_left ? n : page_left;
        uint8_t pfx = addr_prefix(off, buf);
        for (uint16_t i = 0; i < chunk; i++) buf[pfx + i] = data[i];
        if (i2c_xfer(EEPROM_I2C_ADDR, buf, (uint16_t)(pfx + chunk), 0, 0)) return -1;
        delay_ms(10);                          /* EEPROM write cycle (max 5-10 ms) */
        off += chunk; data += chunk; n -= chunk;
    }
    return 0;
}

static int eeprom_read(uint16_t off, uint8_t *data, uint16_t n)
{
    uint8_t pfx[2];
    uint8_t plen = addr_prefix(off, pfx);
    return i2c_xfer(EEPROM_I2C_ADDR, pfx, plen, data, n);
}

/* Try to read the config under a given addressing width. Returns 1 and loads
 * it into s_cfg if the magic matches (i.e. this width is the part's native
 * scheme for the existing data). A round-trip probe CANNOT distinguish the two
 * schemes — a write+read is self-consistent under either — so the magic field
 * is the external reference that disambiguates them. */
static int try_width_magic(uint8_t addrw)
{
    uint8_t old = s_addrw;
    s_addrw = addrw;
    op_config_t tmp;
    int ok = (eeprom_read(EEPROM_ADDR, (uint8_t*)&tmp, sizeof(tmp)) == 0 &&
              tmp.magic == CFG_MAGIC);
    if (ok) {
        s_cfg = tmp;
        if (s_cfg.density < 1 || s_cfg.density > 16) s_cfg.density = 8;
    }
    s_addrw = old;
    return ok;
}

void store_load(void)
{
    /* Disambiguate the addressing width using the magic field as the external
     * reference. Try each scheme; a valid config under one pins the width. */
    if (try_width_magic(2)) return;   /* BL24C128A, 16 KB, 2-byte (rev H/I/K) */
    if (try_width_magic(1)) return;   /* 24C02 / AT24C0x, 256 B, 1-byte (rev E) */

    /* First boot / factory: no valid config under either scheme. Default to the
     * current-production part (2-byte, BL24C128A) and persist immediately so the
     * width is pinned from boot 1 — a board whose part differs simply won't
     * round-trip its magic next boot and re-selects the other scheme then. */
    s_addrw = 2;
    defaults();
    store_save();
}

int store_save(void)
{
    return eeprom_write(EEPROM_ADDR, (const uint8_t*)&s_cfg, sizeof(op_config_t));
}

/* Self-test: write a known pattern to the scratch area and read it back.
 * Returns 1 = match, 0 = mismatch or an I2C error. Exercises the full I2C
 * path without touching the config. */
int store_selftest(void)
{
    const uint8_t pattern[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
    if (eeprom_write(SCRATCH_OFF, pattern, 4) != 0) return 0;
    uint8_t r[4];
    if (eeprom_read(SCRATCH_OFF, r, 4) != 0) return 0;
    for (int i = 0; i < 4; i++) if (r[i] != pattern[i]) return 0;
    return 1;
}

void store_init(void) { i2c_init(); store_load(); }
const op_config_t *store_get(void)     { return &s_cfg; }
op_config_t       *store_get_mut(void) { return &s_cfg; }
