/* OpenDMOfw - config storage in a 24Cxx I2C EEPROM.
 *
 * Roll state here is PURE configuration: a SKU string + a reported count,
 * freely changeable without re-flashing. No tag, no authentication.
 * If the EEPROM is missing or its contents are invalid, the compiled defaults apply.
 *
 * Sourced (EEVBlog 550-series teardowns, Rev H/I/K): **BL24C128A** (Belling,
 * 128 kbit = 16 KB, **64 B page, 2-byte internal addressing**), 7-bit address
 * **0x50** (A0-A2 to GND). Rev E boards carry a smaller Atmel **AT24C01D/02D**
 * (8 B page, 1-byte addressing) at the same address. Detection uses the
 * config magic as the external reference (a write+read cannot tell 1-byte
 * from 2-byte addressing). First boot persists and verifies; if 2-byte
 * fails it retries 1-byte; if both fail, config stays in RAM.
 *
 * The I2C bus also carries the NFC front-end (SLRC610 @ 0x28); it is a
 * different device address and is simply ignored by this driver.
 *
 * ASSUMPTION (PINMAP.md): I2C1 on PB8/PB9 (the F072's I2C1 pins are only
 * PB6/PB7 or PB8/PB9). The alternate function is AF1 - see pins.h, this was
 * wrong once (DECISIONS D38).
 *
 * CLOCK: I2C1 is fed from the 8 MHz HSI, not from PCLK. RCC_CFGR3.I2C1SW is
 * left at its reset value 0 (= HSI), and SystemInit() never switches the HSI
 * off - it only moves SYSCLK to HSI48 (or the PLL). So TIMINGR below is ST's
 * 100 kHz standard-mode value for an 8 MHz kernel clock (PRESC 1, SCLL 0x13,
 * SCLH 0xF, analog filter on), and it is exact. Do NOT "correct" it for 48 MHz:
 * that would make the bus six times too fast.
 */
#include "store.h"
#include "../mcu.h"
#include "../system.h"
#include "../pins.h"

/* "ODM2". Bumped from "ODM1" when the record gained its checksum byte: an
 * ODM1 record is one byte shorter and has no sum, so reading it as an ODM2
 * record would take whatever follows it in the EEPROM as the checksum. A
 * printer flashed with this firmware re-writes its defaults once and carries
 * on; the alternative, silently accepting the old layout, is how a "corrupt"
 * record gets accepted for the second time. */
#define CFG_MAGIC   0x4F444D32u      /* "ODM2" — shared by OP57 and OP104 */
#define I2C_TIMINGR 0x10420F13u      /* 100 kHz with I2C1 on the 8 MHz HSI (I2C1SW = 0) */
/* 2-byte (16 KB) config sits past the first 256 B so a stock image's low
 * EEPROM is left alone; 1-byte parts only have 256 B so they use offset 0. */
#define EEPROM_OFF_2B  0x0100
#define EEPROM_OFF_1B  0x0000
#define SCRATCH_OFF_2B 0x0140
#define SCRATCH_OFF_1B 0x0040

/* Address width (bytes) and page size follow the detected part:
 * 2-byte / 64 B page = BL24C128A (Rev H/I/K); 1-byte / 8 B page = AT24C01D/02D (Rev E). */
static uint8_t s_addrw = 2;          /* default: current production part */

static uint16_t cfg_off(void)     { return (s_addrw == 2) ? EEPROM_OFF_2B  : EEPROM_OFF_1B; }
static uint16_t scratch_off(void) { return (s_addrw == 2) ? SCRATCH_OFF_2B : SCRATCH_OFF_1B; }

static op_config_t s_cfg;

/* Plain 8-bit sum over everything but the sum byte itself. Deliberately not a
 * CRC: this guards against a flipped bit and a torn page write, not against an
 * adversary, and a sum costs a handful of bytes of flash on a part where the
 * whole image has to fit in 64 KB. It catches every single-bit error and every
 * torn write that changes the byte total, which is every torn write that
 * changes anything except a byte swap. */
static uint8_t cfg_sum(const op_config_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    uint8_t s = 0;
    for (unsigned i = 0; i < sizeof(op_config_t) - 1u; i++) s = (uint8_t)(s + p[i]);
    return s;
}

/* Byte-for-byte comparison of a read-back against what we meant to write.
 *
 * "magic is right and the checksum agrees" asks whether the part holds SOME
 * valid record - not whether it holds THE record just written, and those come
 * apart in exactly the case that matters. A configured board whose /WP is tied
 * high, a footprint with the wrong device, or a worn cell already HOLDS a valid
 * record; the new write goes nowhere and the read-back verifies the old one.
 * Measured on a register-level part model: GS D 0x08 reported persisted = 1
 * with the interlock byte still 0x01 on the part and 0x03 in RAM. A torn write
 * does the same thing - a power loss between page writes on the 8-byte-page
 * part, with the changed byte in a later page, leaves the old record intact.
 *
 * This was cycle 4's own fix, correct only for a BLANK part - which is the only
 * case the Renode write-protect scenario preloads. Comparing the whole record
 * subsumes both the magic and the checksum, and costs 4 bytes LESS of flash. */
static int cfg_same(const op_config_t *a, const op_config_t *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (unsigned i = 0; i < sizeof(op_config_t); i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* Set when a record was found whose magic matched but whose sum did not - i.e.
 * an EEPROM that answers and holds something corrupt, as opposed to no EEPROM
 * at all. The two call for different defaults; see store_load(). */
static int s_saw_corrupt;

static void defaults(void)
{
    s_cfg.magic = CFG_MAGIC;
    const char *d = MODEL_DEFAULT_SKU;   /* from model.h (via pins.h) */
    uint8_t i = 0; for (; i < OP_SKU_MAX-1 && d[i]; i++) s_cfg.sku[i] = d[i];
    /* Erase to the END of the field, not just terminate. The whole 24-byte
     * array is persisted, and the ESC U roll record and the GS D 0x04 snapshot
     * both copy a FIXED WIDTH out of it - so a shorter SKU replacing a longer
     * one used to leave the old tail behind and report the field as two SKUs
     * concatenated. */
    for (; i < OP_SKU_MAX; i++) s_cfg.sku[i] = 0;
    s_cfg.label_count = MODEL_DEFAULT_COUNT;   /* matches factory_reset + wrap-around */
    s_cfg.density = 8;
    s_cfg.flags = OP_FLAG_PAPER_FORCE;   /* report a valid roll regardless of the sensor */
#if defined(OPENDMO_SAFE_BRINGUP) && OPENDMO_SAFE_BRINGUP
    /* Exploration image: the heat rail starts locked out. FIELDWORK's fast
     * route builds this variant, pokes at unknown pins freely, and only clears
     * the bit once the strobe lines have been seen idling high. */
    s_cfg.flags |= OP_FLAG_VH_INHIBIT;
#endif
}

/* ---- Minimal I2C v2 with timeouts (never hangs) ------------------------- */
static void i2c_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    /* I2C1 is AF1 on the F072 (PIN_I2C_AF, pins.h; ST's hal_gpio_ex.h). */
    gpio_od(PIN_I2C_SCL, 1); gpio_pull(PIN_I2C_SCL, 1); gpio_af(PIN_I2C_SCL, PIN_I2C_AF);
    gpio_od(PIN_I2C_SDA, 1); gpio_pull(PIN_I2C_SDA, 1); gpio_af(PIN_I2C_SDA, PIN_I2C_AF);
    I2C1->CR1 = 0;
    I2C1->TIMINGR = I2C_TIMINGR;
    I2C1->CR1 = I2C_CR1_PE;
}

static int wait(volatile uint32_t flag)
{
    uint32_t g = 200000;
    while (!(I2C1->ISR & flag) && --g) {
        wdt_kick();
        if (I2C1->ISR & I2C_ISR_NACKF) {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            I2C1->CR2 |= I2C_CR2_STOP;
            return -1;
        }
    }
    if (!g) {
        I2C1->CR2 |= I2C_CR2_STOP;
        return -1;
    }
    return 0;
}

static void i2c_bus_recover(void)
{
    uint32_t g = 10000;
    /* RM0091: PE must be held low for at least 3 APB cycles before it is set
     * again. The old spin read CR1 back and exited immediately - PE is a plain
     * software bit, so it reads 0 on the first load and one APB read is not a
     * guarantee of three. 2 us is ~96 APB cycles at 48 MHz, and this path runs
     * only after a bus hang, so the cost is irrelevant. */
    I2C1->CR1 &= ~I2C_CR1_PE;
    delay_us(2);
    while ((I2C1->CR1 & I2C_CR1_PE) && --g) {}
    I2C1->CR1 = I2C_CR1_PE;
}

static int i2c_xfer(uint8_t addr7, const uint8_t *w, uint16_t wn, uint8_t *r, uint16_t rn)
{
    if (I2C1->ISR & I2C_ISR_BUSY)
        i2c_bus_recover();
    if (wn) {
    /* A NACKF or STOPF left over from a previous transfer would make wait()
     * abort this one before it starts. */
    I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
        I2C1->CR2 = ((uint32_t)addr7 << 1) | ((uint32_t)wn << 16) |
                    (rn ? 0 : I2C_CR2_AUTOEND) | I2C_CR2_START;
        for (uint16_t i = 0; i < wn; i++) {
            if (wait(I2C_ISR_TXIS)) return -1;
            I2C1->TXDR = w[i];
        }
        if (!rn) {
        /* Two bugs lived in the one line this replaces.
         *
         * (a) THE LAST BYTE'S ACKNOWLEDGE WAS NEVER CHECKED. wait() runs for
         *     TXIS BEFORE each byte, so nothing covered the acknowledge of the
         *     final one - and with AUTOEND the peripheral raises STOPF on a NAK
         *     too, so STOPF alone does not mean the part took the data. The
         *     return value of wait() was discarded as well.
         *
         * (b) ICR WAS WRITTEN WITH STOPF ONLY, so NACKF stayed set - and wait()
         *     tests NACKF before the flag it is waiting for. So one NAKed byte
         *     poisoned the NEXT transfer: measured, a healthy part right after
         *     a last-byte NAK returned -1 from store_save() with the page
         *     already committed, i.e. a good save reported as failed, and
         *     GS D 0x08 would have answered persisted = 0 for it. */
        if (wait(I2C_ISR_STOPF)) return -1;
        int nak = (I2C1->ISR & I2C_ISR_NACKF) != 0;
        I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
        return nak ? -1 : 0;
    }
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

/* Try to read the config under a given addressing width + offset. Returns 1
 * and loads it into s_cfg if the magic matches. A write+read round-trip cannot
 * tell 1-byte from 2-byte addressing (self-consistent either way), so magic
 * is the external reference. */
static int try_magic(uint8_t addrw, uint16_t off)
{
    uint8_t old = s_addrw;
    s_addrw = addrw;
    op_config_t tmp;
    int ok = 0;
    if (eeprom_read(off, (uint8_t*)&tmp, sizeof(tmp)) == 0 &&
        tmp.magic == CFG_MAGIC) {
        if (cfg_sum(&tmp) != tmp.sum) {
            /* There IS a record here and it does not verify. Remember that:
             * store_load() must not treat this the same as a blank part. */
            s_saw_corrupt = 1;
        } else {
            /* Sanitise every field that carries policy, not just density. A
             * record can verify and still hold values this firmware never
             * writes - an older layout, a bench tool, a partially erased part.
             * The sku terminator matters because s_cfg.sku is handed to string
             * code; the flags mask matters because six undefined bits going
             * live is six behaviours nobody designed. */
            tmp.sku[OP_SKU_MAX - 1] = 0;
            if (tmp.density > 16) tmp.density = 8;
            if (tmp.label_count > (uint16_t)(MODEL_DEFAULT_COUNT * 10u))
                tmp.label_count = MODEL_DEFAULT_COUNT;
            tmp.flags &= (uint8_t)(OP_FLAG_PAPER_FORCE | OP_FLAG_VH_INHIBIT);
            s_cfg = tmp;
            ok = 1;
        }
    }
    s_addrw = old;
    return ok;
}

static int persist_and_verify(void)
{
    s_cfg.sum = cfg_sum(&s_cfg);
    if (eeprom_write(cfg_off(), (const uint8_t*)&s_cfg, sizeof(op_config_t)) != 0)
        return 0;
    op_config_t tmp;
    if (eeprom_read(cfg_off(), (uint8_t*)&tmp, sizeof(tmp)) != 0)
        return 0;
    /* Verify the whole record, not just its first four bytes: the read-back is
     * the only chance to notice a part that acknowledged a write it did not
     * complete. */
    return cfg_same(&tmp, &s_cfg);
}

void store_load(void)
{
    /* Prefer current-production 2-byte at 0x100, then legacy 2-byte at 0,
     * then 1-byte at 0 (Rev E). */
    if (try_magic(2, EEPROM_OFF_2B)) { s_addrw = 2; return; }
    if (try_magic(2, EEPROM_OFF_1B)) { s_addrw = 2; return; }
    if (try_magic(1, EEPROM_OFF_1B)) { s_addrw = 1; return; }

    defaults();
    /* An EEPROM that answers but holds a record we cannot verify is NOT the same
     * as a bare board. We cannot tell a flipped bit from a write torn by a power
     * loss, and the byte that decides whether the head may be heated is inside
     * that record - so this is the one place where the compiled defaults are not
     * good enough. Lock the heat rail out and let the operator clear it
     * deliberately (GS D 0x08) once they know what happened. A refusal to heat
     * is recoverable; a head is not. */
    if (s_saw_corrupt) s_cfg.flags |= OP_FLAG_VH_INHIBIT;
    s_addrw = 2;
    if (persist_and_verify()) return;
    s_addrw = 1;
    if (persist_and_verify()) return;
    /* WP asserted, missing EEPROM, or wrong I2C pins: keep RAM defaults. */
    s_addrw = 2;
}

/* Read the record back and check it. store_save() used to return 0 whenever the
 * I2C transfers were ACKed, which is a much weaker statement than "it is
 * stored": a 24Cxx with /WP tied high, a footprint fitted with the wrong
 * device, or a worn cell ACKs address and data and performs no write cycle at
 * all. Measured on a register-level part model: only the fully ABSENT part was
 * caught, and only because its address NAKs.
 *
 * That matters most for GS D 0x08. The live interlock is honoured either way -
 * head.c gates on the RAM copy - but PROTOCOL.md promises the subcommand
 * PERSISTS the bit, and an operator who arms it, reads the confirming reply and
 * power-cycles would have found it gone. store_load()'s own boot-time write
 * already verified itself this way (persist_and_verify); store_save() simply
 * did not. One extra 33-byte read per save, and saves happen once per label,
 * not once per line. */
int store_save(void)
{
    s_cfg.sum = cfg_sum(&s_cfg);
    if (eeprom_write(cfg_off(), (const uint8_t*)&s_cfg, sizeof(op_config_t)) != 0)
        return -1;
    op_config_t tmp;
    if (eeprom_read(cfg_off(), (uint8_t*)&tmp, sizeof tmp) != 0)
        return -1;
    if (!cfg_same(&tmp, &s_cfg))
        return -1;
    return 0;
}

/* Self-test: write a known pattern to the scratch area and read it back.
 * Returns 1 = match, 0 = mismatch or an I2C error. Exercises the full I2C
 * path without touching the config. */
int store_selftest(void)
{
    const uint8_t pattern[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
    if (eeprom_write(scratch_off(), pattern, 4) != 0) return 0;
    uint8_t r[4];
    if (eeprom_read(scratch_off(), r, 4) != 0) return 0;
    for (int i = 0; i < 4; i++) if (r[i] != pattern[i]) return 0;
    return 1;
}

void store_init(void) { i2c_init(); store_load(); }
const op_config_t *store_get(void)     { return &s_cfg; }
op_config_t       *store_get_mut(void) { return &s_cfg; }
