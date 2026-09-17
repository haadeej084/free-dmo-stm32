/* OpenDMOfw - host tests for src/config/store.c against a register-level
 * 24Cxx part model (test/i2c_eeprom_model.h).
 *
 * store.c holds OP_FLAG_VH_INHIBIT, the one bit that protects an irreplaceable
 * print head, and until now had no host harness at all. Renode's eeprom.py
 * kills 47 of 109 real mutants; the survivor that matters names itself -
 * persist_and_verify()'s `tmp.magic == CFG_MAGIC && cfg_sum(&tmp) == tmp.sum`
 * turned into `||` survives it, so the read-back verify is itself unverified.
 *
 * Every scenario runs in a forked child, because store.c's static state
 * (s_cfg, s_addrw, s_saw_corrupt) has no reset entry point and s_saw_corrupt
 * in particular is never cleared once set.
 *
 * Build:
 *   cc -DOPENDMO_HOST_TEST -DMODEL_OP57 -Isrc -Itest \
 *      test/test_store.c src/config/store.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "system.h"
#include "config/store.h"
#include "host_periph.h"
#include "i2c_eeprom_model.h"

uint32_t   g_ms;
host_i2c_t g_i2c;
static ee_part_t part;

/* ---- the boundary, stubbed; store.c itself is the code under test -------- */
void gpio_od(pin_t p, int open_drain) { (void)p; (void)open_drain; }
void gpio_pull(pin_t p, int pull)     { (void)p; (void)pull; }
static int g_af_b[16];                 /* AF selected per port-B pin, -1 = never */
void gpio_af(pin_t p, uint8_t af)     { if (p.port == GPIOB && p.pin < 16) g_af_b[p.pin] = af; }
void gpio_mode(pin_t p, gpio_mode_t m){ (void)p; (void)m; }
void gpio_set(pin_t p, int high)      { (void)p; (void)high; }
int  gpio_get(pin_t p)                { (void)p; return 0; }
void wdt_kick(void)                   { }
uint32_t millis(void)                 { return g_ms; }
/* The EEPROM write cycle is the only thing store.c delays for, so the virtual
 * clock the part model uses for tWR is advanced here. */
void delay_ms(uint32_t ms)            { g_ms += ms; }
void delay_us(uint32_t us)            { (void)us; }

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
                     else printf("  ok   %s\n", #c); }while(0)

#define REC_LEN ((int)sizeof(op_config_t))
#define MAGIC   0x4F444D32u

static uint8_t sum8(const uint8_t *p, int n)
{
    uint8_t s = 0; for (int i = 0; i < n; i++) s = (uint8_t)(s + p[i]); return s;
}

/* Build a record the way the firmware would, with a correct checksum. */
static void rec_build(uint8_t *out, const char *sku, uint16_t count,
                      uint8_t density, uint8_t flags, int good_sum)
{
    memset(out, 0, (size_t)REC_LEN);
    out[0] = 0x32; out[1] = 0x4D; out[2] = 0x44; out[3] = 0x4F;   /* "ODM2" LE */
    memcpy(out + 4, sku, strlen(sku));
    out[4 + OP_SKU_MAX]     = (uint8_t)(count & 0xFF);
    out[4 + OP_SKU_MAX + 1] = (uint8_t)(count >> 8);
    out[4 + OP_SKU_MAX + 2] = density;
    out[4 + OP_SKU_MAX + 3] = flags;
    uint8_t s = sum8(out, REC_LEN - 1);
    out[REC_LEN - 1] = good_sum ? s : (uint8_t)(s ^ 0xFF);
}

static void place(uint16_t off, const uint8_t *rec)
{
    memcpy(part.mem + off, rec, (size_t)REC_LEN);
}

static void big(void)   { ee_reset_part(&part, 2, 0x4000, 64); i2c_model_init(&part); }
static void small(void) { ee_reset_part(&part, 1, 0x0100,  8); i2c_model_init(&part); }

static void dump(const char *tag, uint16_t off, int n)
{
    printf("     %s @%#06x:", tag, off);
    for (int i = 0; i < n; i++) printf(" %02x", part.mem[off + i]);
    printf("\n");
}

/* ------------------------------------------------------------------------- */
static void s01_big_blank(void)
{
    puts("S01 big part (2-byte / 64 B page / 16 KB), blank -> defaults persisted at 0x0100");
    big();
    store_init();
    const op_config_t *c = store_get();
    CHECK(c->magic == MAGIC);
    CHECK(c->flags == OP_FLAG_PAPER_FORCE);
    CHECK(memcmp(part.mem + 0x100, (const void*)c, (size_t)REC_LEN) == 0);
    CHECK(part.pages_committed == 1);             /* 33 B inside one 64 B page */
    /* the stock image's low EEPROM must be left alone */
    int touched = 0;
    for (int i = 0; i < 0x100; i++) if (part.mem[i] != 0xFF) touched++;
    CHECK(touched == 0);
    printf("     starts=%d stops=%d pages=%d\n", part.starts, part.stops, part.pages_committed);
}

static void s02_big_stored(void)
{
    puts("S02 big part with a valid record at 0x0100 -> loaded, nothing written");
    big();
    uint8_t r[64]; rec_build(r, "99999", 42, 5, OP_FLAG_VH_INHIBIT, 1);
    place(0x100, r);
    store_init();
    const op_config_t *c = store_get();
    CHECK(strcmp(c->sku, "99999") == 0);
    CHECK(c->label_count == 42);
    CHECK(c->density == 5);
    CHECK((c->flags & OP_FLAG_VH_INHIBIT) != 0);
    CHECK(part.pages_committed == 0);
    /* A save AFTER a successful load must go back to the same place with the
     * same addressing width. Nothing asserted this for the 16 KB part, so
     * corrupting s_addrw on the success path of rung 1 or rung 2 was invisible. */
    store_get_mut()->label_count = 43;
    CHECK(store_save() == 0);
    CHECK(part.mem[0x100 + 4 + OP_SKU_MAX] == 43);
    store_load();
    CHECK(store_get()->label_count == 43);
}

static void s03_big_legacy_offset0(void)
{
    puts("S03 big part, legacy record at offset 0 -> found by rung 2");
    big();
    uint8_t r[64]; rec_build(r, "LEGACY", 7, 3, OP_FLAG_PAPER_FORCE, 1);
    place(0x0000, r);
    store_init();
    CHECK(strcmp(store_get()->sku, "LEGACY") == 0);
    CHECK(store_get()->label_count == 7);
    CHECK(part.pages_committed == 0);
}

static void s04_small_blank(void)
{
    puts("S04 small part (1-byte / 8 B page / 256 B), blank -> defaults at offset 0");
    small();
    store_init();
    const op_config_t *c = store_get();
    CHECK(c->magic == MAGIC);
    CHECK(memcmp(part.mem + 0, (const void*)c, (size_t)REC_LEN) == 0);
    printf("     pages committed=%d (a 33 B record spans 5 pages of 8)\n", part.pages_committed);
    dump("rec", 0, 16);
}

static void s05_small_stored(void)
{
    puts("S05 small part with a valid record ALREADY at 0 - the case Renode cannot model");
    small();
    uint8_t r[64]; rec_build(r, "SMALL1", 11, 9, OP_FLAG_PAPER_FORCE, 1);
    place(0x0000, r);
    uint8_t before[64]; memcpy(before, part.mem, 64);
    store_init();
    const op_config_t *c = store_get();
    printf("     loaded sku='%s' count=%u pages_committed=%d\n",
           c->sku, (unsigned)c->label_count, part.pages_committed);
    CHECK(strcmp(c->sku, "SMALL1") == 0);
    CHECK(c->label_count == 11);
    /* and the stored record must survive the 2-byte probes untouched */
    CHECK(memcmp(before, part.mem, 64) == 0);
    /* a save afterwards must use 1-byte addressing, i.e. land at offset 0 */
    store_get_mut()->label_count = 12;
    CHECK(store_save() == 0);
    CHECK(part.mem[4 + OP_SKU_MAX] == 12);
}

static void s06_small_stored_altptr(void)
{
    puts("S06 same, with the alternative address-counter model (a data byte does");
    puts("    NOT advance the counter) - does rung 2 still reject the 1-byte part?");
    small();
    part.ptr_datainc = 0;
    uint8_t r[64]; rec_build(r, "SMALL2", 13, 9, OP_FLAG_PAPER_FORCE, 1);
    place(0x0000, r);
    store_init();
    store_get_mut()->label_count = 77;
    int sv = store_save();
    printf("     loaded sku='%s'  store_save=%d\n", store_get()->sku, sv);
    dump("record", 0, 16);
    /* Reported, not asserted: which of the two counter models a real part uses
     * is the one thing this harness cannot settle. Under the datasheet model
     * (S05) store.c is correct; under this one rung 2 matches with s_addrw
     * left at 2 and every later save goes to 0x0100 on a 256-byte part. */
    CHECK(strcmp(store_get()->sku, "SMALL2") == 0);   /* the RAM copy is right */
    printf("     >> rung 2 matched with 2-byte addressing on a 1-byte part: %s\n",
           sv == 0 ? "no" : "yes - saves are lost and page 0 is overwritten");
}

static void s07_wp(void)
{
    puts("S07 /WP asserted: the part ACKs everything and stores nothing");
    big();
    part.wp = 1;
    store_init();
    CHECK(store_get()->magic == MAGIC);            /* RAM defaults */
    int touched = 0;
    for (int i = 0; i < 0x4000; i++) if (part.mem[i] != 0xFF) touched++;
    CHECK(touched == 0);
    CHECK(store_save() == -1);                     /* must NOT report success */
    CHECK(store_selftest() == 0);
}

static void s08_read_zero(void)
{
    puts("S08 the part ACKs and returns 0x00 for every read (a dead/blank die).");
    puts("    cfg_sum(all zero) == 0 == the zero sum byte, so the CHECKSUM ALONE");
    puts("    accepts it - only the magic test rejects it. This is the scenario");
    puts("    that kills the `magic && sum` -> `||` mutant.");
    big();
    store_init();
    CHECK(store_get()->magic == MAGIC);            /* RAM defaults, not zeros */
    CHECK(store_get()->density == 8);
    part.read_zero = 1;
    store_get_mut()->flags |= OP_FLAG_VH_INHIBIT;
    CHECK(store_save() == -1);
    CHECK(store_selftest() == 0);
}

static void s09_absent(void)
{
    puts("S09 no part on the bus: the address NAKs");
    big();
    part.present = 0;
    store_init();
    CHECK(store_get()->magic == MAGIC);
    CHECK(strcmp(store_get()->sku, MODEL_DEFAULT_SKU) == 0);
    CHECK(store_save() == -1);
    CHECK(store_selftest() == 0);
    printf("     address NAKs seen: %d\n", part.addr_naks);
}

static void s10_nak_middata(void)
{
    puts("S10 the part NAKs a byte in the MIDDLE of the data phase");
    big();
    part.nak_at = 10;                 /* 2 address bytes + 8 data bytes */
    CHECK(store_save() == -1);
    CHECK(part.pages_committed == 0);
}

static void s11_nak_lastbyte(void)
{
    puts("S11 the part NAKs the LAST byte of the data phase. i2c_xfer() waits for");
    puts("    TXIS before each byte but never checks the acknowledge of the byte");
    puts("    it just wrote, so this NAK is only visible as NACKF - which the");
    puts("    STOPF wait ignores and the ICR write does not clear.");
    big();
    part.nak_at = 2 + REC_LEN - 1;    /* the final data byte of the record */
    int sv = store_save();
    printf("     store_save=%d  committed=%d  data NAKs=%d  NACKF still set=%d\n",
           sv, part.pages_committed, part.data_naks,
           (g_i2c.r.ISR & I2C_ISR_NACKF) ? 1 : 0);
    CHECK(sv == -1);
}

static void s12_dead_bus(void)
{
    puts("S12 dead bus: SCL held low, no flag ever moves. The timeout guards must");
    puts("    return, not hang.");
    big();
    part.dead = 1;
    CHECK(store_save() == -1);
    CHECK(store_selftest() == 0);
    store_init();
    CHECK(store_get()->magic == MAGIC);
}

static void s13_torn_write(void)
{
    puts("S13 TORN WRITE on the 8-byte-page part: power lost after the FIRST page,");
    puts("    so the magic is new and the rest of the record is old. This is the");
    puts("    exact failure D32's checksum exists for.");
    small();
    uint8_t old[64]; rec_build(old, "OLDSKU", 200, 8, OP_FLAG_PAPER_FORCE, 1);
    place(0x0000, old);
    store_init();
    part.commit_pages = part.pages_committed + 1;  /* one more page, then power off */

    /* The change is in the LAST page (density is byte 30 of 33), so the torn
     * record on the part is byte-for-byte the OLD record - still valid. */
    store_get_mut()->density = 3;
    int sv = store_save();
    printf("     torn store_save=%d  density on the part=%u (wrote 3)\n",
           sv, part.mem[4 + OP_SKU_MAX + 2]);
    dump("torn", 0, 16);
    CHECK(part.mem[4 + OP_SKU_MAX + 2] != 3);      /* the write did not land */
    CHECK(sv == -1);                                /* ...so it must not say 0 */
}

static void s21_wp_over_existing_record(void)
{
    puts("S21 /WP asserted on a part that ALREADY HOLDS A VALID RECORD.");
    puts("    Cycle 4 gave store_save() a read-back precisely for the /WP case,");
    puts("    but it verifies `magic && sum`, i.e. that the part holds SOME valid");
    puts("    record - not THE record just written. With a valid record already");
    puts("    there, the read-back returns the OLD one and verifies.");
    big();
    uint8_t old[64]; rec_build(old, "OLD", 50, 8, OP_FLAG_PAPER_FORCE, 1);
    place(0x100, old);
    store_init();
    CHECK(strcmp(store_get()->sku, "OLD") == 0);
    part.wp = 1;                                    /* /WP goes high from here */

    /* exactly what GS D 0x08 does: arm the heat interlock and persist it */
    store_get_mut()->flags |= OP_FLAG_VH_INHIBIT;
    int sv = store_save();
    printf("     store_save=%d  (GS D 0x08 reports persisted=%d)\n", sv, sv == 0);
    printf("     flags byte on the part: %02x   in RAM: %02x\n",
           part.mem[0x100 + 4 + OP_SKU_MAX + 3], store_get()->flags);
    CHECK((part.mem[0x100 + 4 + OP_SKU_MAX + 3] & OP_FLAG_VH_INHIBIT) == 0);
    CHECK(sv == -1);          /* nothing was stored, so success is a lie */
}

static void s22_stale_nackf_poisons_next(void)
{
    puts("S22 after a NAK on the LAST data byte, NACKF is left set. wait() checks");
    puts("    NACKF before the flag it is waiting for, so the NEXT transfer - on a");
    puts("    part that is behaving perfectly - aborts on the stale flag.");
    big();
    part.nak_at = 2 + REC_LEN - 1;
    (void)store_save();
    printf("     NACKF after the failed save: %d\n", (g_i2c.r.ISR & I2C_ISR_NACKF) ? 1 : 0);
    part.nak_at = -1;                  /* the part is healthy again */
    int starts0 = part.starts, pages0 = part.pages_committed;
    int sv = store_save();
    printf("     healthy retry: store_save=%d starts=%d pages=%d\n",
           sv, part.starts - starts0, part.pages_committed - pages0);
    CHECK(sv == 0);
}

static void s23_smallpart_2byte_write_damage(void)
{
    puts("S23 what the 2-byte probe WRITE does to a 1-byte part: store_load() tries");
    puts("    persist_and_verify() at s_addrw=2 first, which clocks address byte");
    puts("    0x01 and then 34 data bytes into an 8-byte page buffer - they wrap");
    puts("    inside page 0 and are committed by the AUTOEND STOP.");
    small();
    uint8_t old[64]; rec_build(old, "KEEPME", 77, 8, OP_FLAG_PAPER_FORCE, 1);
    place(0x0000, old);
    part.mem[20] ^= 0x01;              /* make it fail its checksum (D32 path) */
    uint8_t before[16]; memcpy(before, part.mem, 16);
    store_init();
    printf("     pages committed during boot: %d\n", part.pages_committed);
    dump("after", 0, 16);
    printf("     was ");
    for (int i = 0; i < 16; i++) printf(" %02x", before[i]);
    printf("\n");
    CHECK(store_get()->flags & OP_FLAG_VH_INHIBIT);
}

static void s14_torn_reboot(void)
{
    puts("S14 ...and the reboot after it: a record whose magic verifies and whose");
    puts("    sum does not must lock the heat rail out (D32), not fall back to");
    puts("    the compiled defaults as if the part were blank.");
    small();
    uint8_t rec[64]; rec_build(rec, "TORN", 5, 8, OP_FLAG_PAPER_FORCE, 1);
    place(0x0000, rec);
    part.mem[20] ^= 0x01;             /* one flipped bit past the first page */
    store_init();
    const op_config_t *c = store_get();
    printf("     sku='%s' flags=%02x\n", c->sku, c->flags);
    CHECK((c->flags & OP_FLAG_VH_INHIBIT) != 0);
    CHECK(strcmp(c->sku, MODEL_DEFAULT_SKU) == 0);
}

static void s15_flag_bitflip(void)
{
    puts("S15 one flipped bit in the flags byte alone (the VH_INHIBIT bit itself)");
    big();
    uint8_t rec[64]; rec_build(rec, "FLIP", 5, 8,
                               (uint8_t)(OP_FLAG_PAPER_FORCE | OP_FLAG_VH_INHIBIT), 1);
    rec[4 + OP_SKU_MAX + 3] &= (uint8_t)~OP_FLAG_VH_INHIBIT;   /* disarm it */
    place(0x100, rec);
    store_init();
    CHECK((store_get()->flags & OP_FLAG_VH_INHIBIT) != 0);     /* refused, relocked */
}

static void s16_sanitise(void)
{
    puts("S16 a record that verifies but holds values this firmware never writes");
    big();
    uint8_t rec[64];
    rec_build(rec, "SAN", 60000, 99, 0xFF, 1);
    memset(rec + 4, 'A', OP_SKU_MAX);        /* no NUL anywhere in the sku */
    rec[REC_LEN - 1] = sum8(rec, REC_LEN - 1);
    place(0x100, rec);
    store_init();
    const op_config_t *c = store_get();
    printf("     density=%u count=%u flags=%02x sku='%s'\n",
           c->density, (unsigned)c->label_count, c->flags, c->sku);
    CHECK(c->density == 8);
    CHECK(c->label_count == MODEL_DEFAULT_COUNT);
    CHECK(c->flags == (OP_FLAG_PAPER_FORCE | OP_FLAG_VH_INHIBIT));
    CHECK(c->sku[OP_SKU_MAX - 1] == 0);
}

static void s17_selftest_scratch(void)
{
    puts("S17 store_selftest() uses the scratch area and must not touch the config");
    big();
    store_init();
    uint8_t before[64]; memcpy(before, part.mem + 0x100, 64);
    CHECK(store_selftest() == 1);
    CHECK(memcmp(before, part.mem + 0x100, 64) == 0);
    CHECK(part.mem[0x140] == 0xA5 && part.mem[0x143] == 0x3C);

    small();
    store_init();
    uint8_t b2[64]; memcpy(b2, part.mem, 64);
    CHECK(store_selftest() == 1);
    CHECK(memcmp(b2, part.mem, (size_t)REC_LEN) == 0);
    CHECK(part.mem[0x40] == 0xA5 && part.mem[0x43] == 0x3C);
}

static void s18_page_boundaries(void)
{
    puts("S18 page-write splitting: how many write cycles does one record cost?");
    big();
    store_init();
    int big_pages = part.pages_committed;
    small();
    store_init();
    int small_pages = part.pages_committed;
    printf("     64 B page: %d write cycle(s); 8 B page: %d write cycle(s)\n",
           big_pages, small_pages);
    CHECK(big_pages == 1);
    /* 5 pages for the record, plus ONE bogus page write: store_load() always
     * tries s_addrw=2 first, and on a 1-byte part that write lands (wrapped)
     * in page 0 before the 1-byte retry rewrites it. */
    CHECK(small_pages == 6);
}

static void s19_write_cycle_time(void)
{
    puts("S19 the internal write cycle. store.c waits a FIXED delay_ms(10) and");
    puts("    never ACK-polls, so a part slower than that NAKs the next chunk.");
    small();
    part.twr_ms = 5;
    store_init();
    CHECK(part.addr_naks == 0);
    CHECK(part.pages_committed == 6);

    /* exactly 10 ms: this is what pins the delay_ms(10) constant from below */
    small();
    part.twr_ms = 10;
    store_init();
    CHECK(part.addr_naks == 0);
    CHECK(part.pages_committed == 6);

    small();
    part.twr_ms = 15;                 /* a part outside the assumed 5-10 ms */
    store_init();
    printf("     tWR 15 ms: address NAKs=%d pages=%d, RAM defaults kept=%d\n",
           part.addr_naks, part.pages_committed, store_get()->magic == MAGIC);
    CHECK(part.addr_naks > 0);
}

static void s20_save_roundtrip(void)
{
    puts("S20 store_save() round-trip on both parts");
    big();
    store_init();
    store_get_mut()->label_count = 321;
    store_get_mut()->density = 11;
    CHECK(store_save() == 0);
    store_load();
    CHECK(store_get()->label_count == 321);
    CHECK(store_get()->density == 11);
}

static void s24_persist_verify_on_zero_reader(void)
{
    puts("S24 the SAME read-back, but on store_load()'s boot-time write path.");
    puts("    persist_and_verify() is the site of Renode survivor #0028. A part");
    puts("    that ACKs writes and reads back 0x00 defeats the checksum half of");
    puts("    the test on its own - cfg_sum(all zero) == 0 == the zero sum byte -");
    puts("    so only the magic half rejects it. store_load() must therefore go on");
    puts("    to try the 1-byte fallback instead of concluding the record is");
    puts("    stored; with `||` in place of `&&` it stops after the first attempt.");
    big();
    part.read_zero = 1;
    store_init();
    printf("     page writes during boot: %d  (1 = it stopped after the first\n"
           "     persist attempt, i.e. it believed the record was stored)\n",
           part.pages_committed);
    CHECK(part.pages_committed > 1);      /* the 1-byte fallback must still run */
    CHECK(store_get()->magic == MAGIC);   /* and RAM defaults are kept */
}

static void s25_persist_verify_torn_first_page(void)
{
    puts("S25 persist_and_verify() against a TORN boot-time write: the page with");
    puts("    the magic commits and the rest does not, so magic verifies and the");
    puts("    sum does not - the other half of #0028.");
    small();
    part.commit_pages = 2;   /* the bogus 2-byte page, then one 1-byte page */
    store_init();
    /* With the verify intact the record never persists, so the scratch area of
     * store_selftest() shows which addressing width store_load() settled on:
     * 0x40 means it fell through to 1-byte, i.e. both attempts were refused. */
    (void)store_selftest();
    printf("     pages=%d  scratch at 0x40=%02x  at 0x01=%02x\n",
           part.pages_committed, part.mem[0x40], part.mem[0x01]);
    CHECK(part.mem[0x40] != 0xA5);        /* s_addrw ended at 2, not 1 */
}

static void s26_sanitise_boundaries(void)
{
    puts("S26 the sanitising BOUNDARIES: density 16 is legal and 17 is not;");
    puts("    label_count 10x the default is legal and one more is not.");
    uint8_t rec[64];
    for (int d = 16; d <= 17; d++) {
        big();
        rec_build(rec, "B", 5, (uint8_t)d, OP_FLAG_PAPER_FORCE, 1);
        place(0x100, rec);
        store_init();
        printf("     density %d -> %u\n", d, store_get()->density);
        if (d == 16) CHECK(store_get()->density == 16);
        else         CHECK(store_get()->density == 8);
    }
    const uint16_t lim = (uint16_t)(MODEL_DEFAULT_COUNT * 10u);
    for (int k = 0; k <= 1; k++) {
        big();
        rec_build(rec, "B", (uint16_t)(lim + k), 8, OP_FLAG_PAPER_FORCE, 1);
        place(0x100, rec);
        store_init();
        printf("     count %u -> %u\n", (unsigned)(lim + k), (unsigned)store_get()->label_count);
        if (k == 0) CHECK(store_get()->label_count == lim);
        else        CHECK(store_get()->label_count == MODEL_DEFAULT_COUNT);
    }
}

static void s27_selftest_one_bad_cell(void)
{
    puts("S27 store_selftest() with exactly ONE worn cell in the scratch area.");
    puts("    Every byte of the pattern has to be compared, including the first.");
    for (int k = 0; k < 4; k++) {
        big();
        store_init();
        part.bad_cell = 0x140 + k;
        int r = store_selftest();
        printf("     bad cell at scratch+%d -> store_selftest()=%d\n", k, r);
        CHECK(r == 0);
    }
}

static void s28_i2c_alternate_function(void)
{
    puts("S28 I2C1 on PB8/PB9 is ALTERNATE FUNCTION 1 on the F072: ST's own");
    puts("    stm32f0xx_hal_gpio_ex.h, STM32F072xB block, defines GPIO_AF1_I2C1");
    puts("    (and GPIO_AF3_I2C1); its AF2 is GPIO_AF2_USB and TIM16/TIM17. The");
    puts("    driver selected AF2, which muxes the pins to TIM16_CH1/TIM17_CH1 on");
    puts("    silicon: no EEPROM, ever, and Renode's GPIO model does not enforce");
    puts("    the mux so no emulator test could see it (D38). The literal 1 is");
    puts("    deliberate (D34): reading PIN_I2C_AF back would agree with anything.");
    big();
    store_init();
    CHECK(g_af_b[PIN_I2C_SCL.pin] == 1);
    CHECK(g_af_b[PIN_I2C_SDA.pin] == 1);
}

typedef void (*scen_fn)(void);
static const struct { const char *name; scen_fn fn; } SCEN[] = {
    {"s01", s01_big_blank},        {"s02", s02_big_stored},
    {"s03", s03_big_legacy_offset0}, {"s04", s04_small_blank},
    {"s05", s05_small_stored},     {"s06", s06_small_stored_altptr},
    {"s07", s07_wp},               {"s08", s08_read_zero},
    {"s09", s09_absent},           {"s10", s10_nak_middata},
    {"s11", s11_nak_lastbyte},     {"s12", s12_dead_bus},
    {"s13", s13_torn_write},       {"s14", s14_torn_reboot},
    {"s15", s15_flag_bitflip},     {"s16", s16_sanitise},
    {"s17", s17_selftest_scratch}, {"s18", s18_page_boundaries},
    {"s19", s19_write_cycle_time}, {"s20", s20_save_roundtrip},
    {"s21", s21_wp_over_existing_record},
    {"s22", s22_stale_nackf_poisons_next},
    {"s23", s23_smallpart_2byte_write_damage},
    {"s24", s24_persist_verify_on_zero_reader},
    {"s25", s25_persist_verify_torn_first_page},
    {"s26", s26_sanitise_boundaries},
    {"s27", s27_selftest_one_bad_cell},
    {"s28", s28_i2c_alternate_function},
};
#define NSCEN ((int)(sizeof(SCEN)/sizeof(SCEN[0])))

int main(int argc, char **argv)
{
    host_rcc.AHBENR = 0xFFFFFFFFu;
    for (int i = 0; i < 16; i++) g_af_b[i] = -1;

    if (argc > 1) {                       /* child: run one scenario */
        int i = atoi(argv[1]);
        SCEN[i].fn();
        return fails ? 1 : 0;
    }

    int bad = 0;
    for (int i = 0; i < NSCEN; i++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            char buf[8]; snprintf(buf, sizeof buf, "%d", i);
            execl(argv[0], argv[0], buf, (char*)NULL);
            _exit(127);
        }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("  ** scenario %s FAILED (status %d)\n", SCEN[i].name, st);
            bad++;
        }
    }
    printf("test_store: %d/%d scenarios passed\n", NSCEN - bad, NSCEN);
    return bad ? 1 : 0;
}
