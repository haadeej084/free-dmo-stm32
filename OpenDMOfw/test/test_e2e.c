/* OpenDMOfw - end-to-end host test: USB stack + printer class + protocol parser.
 *
 * The real usb_core.c, usb_desc.c, usb_printer.c and printer/protocol.c run
 * together against the USB peripheral model in usb_model.h. Only the hardware
 * below the parser (head, motor, thermal, EEPROM, GPIO) is mocked. The host side
 * does what a driver does: enumerate, then push a whole print job as 64-byte
 * bulk packets and read replies from the IN endpoint, while the "main loop"
 * (protocol_task) runs only when the host is NAKed or waits for a reply - the
 * worst case for flow control. What this proves that the two unit tests cannot:
 * the ring buffer's pause/resume and the endpoint's NAK/re-arm agree with each
 * other, replies reach the host through the real bulk-IN path, and a raster
 * split across dozens of packets arrives at the head byte for byte.
 *
 * Build/run (see Makefile `test`):
 *   cc -std=c11 -DOPENDMO_HOST_TEST -Isrc -o test_e2e test/test_e2e.c \
 *      src/usb/usb_core.c src/usb/usb_desc.c src/usb/usb_printer.c \
 *      src/printer/protocol.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mcu.h"
#include "model.h"
#include "pins.h"
#include "config/store.h"
#include "printer/protocol.h"
#include "usb/usb_core.h"
#include "usb/usb_desc.h"
#include "usb/usb_printer.h"
#include "usb_model.h"

/* ---- mocks below the parser --------------------------------------------- */
#define MAX_LINES 400
static uint8_t  g_line_data[MAX_LINES][HEAD_BYTES];
static int      g_lines;
static int      g_feed;
static op_config_t g_cfg;

void head_init(void) {}
void head_reset(void) {}
void head_print_line(const uint8_t *b, uint16_t n)
{
    if (g_lines < MAX_LINES) memcpy(g_line_data[g_lines], b, n < HEAD_BYTES ? n : HEAD_BYTES);
    g_lines++;
}
void head_set_density(uint8_t d) { (void)d; }
uint32_t head_last_strobe_us(void) { return 0; }
void head_idle_tick(uint32_t ms) { (void)ms; }
int  head_vh_is_on(void) { return 0; }
void head_vh_off(void) {}
void motor_init(void) {}
void motor_enable(int on) { (void)on; }
void motor_step_lines(uint16_t n) { g_feed += n; }
void motor_step_line_after(uint32_t us) { (void)us; g_feed += 1; }
void motor_idle_tick(uint32_t ms) { (void)ms; }
void thermal_init(void) {}
uint16_t thermal_read_raw(void) { return 1638; }
int thermal_ok(void) { return 1; }
int thermal_sensor_fault(void){ return 0; }   /* a believable sensor */
int paper_present(void) { return 1; }
uint16_t thermal_dwell_scale(void) { return 256; }
void thermal_scan_adc(uint16_t *out) { for (int i = 0; i < 10; i++) out[i] = 0; }
void store_init(void) {}
const op_config_t *store_get(void) { return &g_cfg; }
op_config_t *store_get_mut(void) { return &g_cfg; }
int store_save(void) { return 0; }
int store_selftest(void) { return 1; }
void store_load(void) {}
uint16_t sys_port_idr(uint8_t port) { (void)port; return 0; }
int sys_pin_toggle(uint8_t port, uint8_t pin, uint8_t n) { (void)port; (void)pin; (void)n; return 0; }
void sys_enter_bootloader(void) { abort(); }
int  gpio_get(pin_t p) { (void)p; return PAPER_PRESENT_LEVEL; }
void gpio_af(pin_t p, uint8_t a) { (void)p; (void)a; }
static uint32_t g_ms;
uint32_t millis(void) { return g_ms++; }
void delay_ms(uint32_t ms) { g_ms += ms; }
void delay_us(uint32_t us) { (void)us; }
void wdt_kick(void) {}

/* ---- harness ------------------------------------------------------------- */
static int fails, checks;
#define CHECK(c) do { checks++; if (c) printf("ok   %s\n", #c); \
                      else { printf("FAIL %s  (line %d)\n", #c, __LINE__); fails++; } } while (0)

static int g_naks;

/* Push a byte stream as a driver does: 64-byte bulk packets. The main loop
 * only runs when the device NAKs, so the ring buffer fills up completely. */
static int send_stream(const uint8_t *d, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int len = (n - off) > 64 ? 64 : (int)(n - off);
        int tries = 0;
        for (;;) {
            int r = host_out(EP_DATA, d + off, len);
            if (r == 0) break;
            if (r != NAK || ++tries > 10000) return -1;
            g_naks++;
            protocol_task();
        }
        off += (size_t)len;
    }
    return 0;
}

/* Let the main loop run, then collect one reply from the IN endpoint. */
static int read_reply(uint8_t *buf)
{
    for (int i = 0; i < 50; i++) {
        protocol_task();
        int r = host_in(EP_DATA, buf, 64);
        if (r >= 0) return r;
    }
    return -1;
}

static void enumerate(void)
{
    uint8_t b[64];
    irq(0, USB_ISTR_RESET);
    ctrl_in(0x80, 6, 0x0100, 0, 64, b);
    ctrl_nodata(0x00, 5, 3, 0);
    ctrl_nodata(0x00, 9, 1, 0);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int main(void)
{
    uint8_t r[64];
    const char *sku = MODEL_DEFAULT_SKU;
    memset(&g_cfg, 0, sizeof g_cfg);
    memcpy(g_cfg.sku, sku, strlen(sku));
    g_cfg.label_count = 123;
    g_cfg.density = 8;
    g_cfg.flags = OP_FLAG_PAPER_FORCE;

    usb_desc_init_serial();
    protocol_init();
    enumerate();
    CHECK(usb_is_configured());

    /* 1) Status over the real bulk path: 32 bytes, bay OK, our SKU and count. */
    {
        const uint8_t q[] = { 0x1B, 'A', 0x00 };
        CHECK(send_stream(q, sizeof q) == 0);
        int n = read_reply(r);
        CHECK(n == 32);
        CHECK(r[0] == 0 && r[10] == 8);
        CHECK(memcmp(&r[11], sku, strlen(sku)) == 0);
        CHECK((r[27] | (r[28] << 8)) == 123);
    }

    /* 2) A whole job in the Windows driver's order, one stream, far larger than
     *    the 2048-byte ring: 120 full-width lines, every line different. */
    enum { LINES = 120 };
    static uint8_t job[64 + 12 + LINES * HEAD_BYTES + 16];
    size_t n = 0;
    job[n++] = 0x1B; job[n++] = 's'; put32(&job[n], 0x12345678u); n += 4;
    job[n++] = 0x1B; job[n++] = 'C'; job[n++] = 0x64;
    job[n++] = 0x1B; job[n++] = 'h';
    job[n++] = 0x1B; job[n++] = 'T'; job[n++] = 0x10;
    job[n++] = 0x1B; job[n++] = 'L'; job[n++] = 0x05; job[n++] = 0x46;
    job[n++] = 0x1B; job[n++] = 'n'; job[n++] = 1; job[n++] = 0;
    job[n++] = 0x1B; job[n++] = 'D'; job[n++] = 1; job[n++] = 2;
    put32(&job[n], LINES); n += 4;
    put32(&job[n], HEAD_DOTS); n += 4;
    for (int l = 0; l < LINES; l++)
        for (int b = 0; b < HEAD_BYTES; b++)
            job[n++] = (uint8_t)((l * 31 + b * 7 + 5) ^ (b >> 2));
    const size_t raster_end = n;
    job[n++] = 0x1B; job[n++] = 'G';

    g_naks = 0; g_lines = 0; g_feed = 0;
    CHECK(send_stream(job, raster_end) == 0);
    for (int i = 0; i < 20; i++) protocol_task();
    CHECK(g_naks > 0);                          /* flow control really engaged */
    CHECK(g_lines == LINES);
    {
        int bad = 0;
        for (int l = 0; l < LINES && l < MAX_LINES; l++)
            for (int b = 0; b < HEAD_BYTES; b++)
                if (g_line_data[l][b] != (uint8_t)((l * 31 + b * 7 + 5) ^ (b >> 2))) bad++;
        CHECK(bad == 0);
    }

    /* mid-job status: printing, job id and label index echoed */
    {
        const uint8_t q[] = { 0x1B, 'A', 0x00 };
        CHECK(send_stream(q, sizeof q) == 0);
        int m = read_reply(r);
        CHECK(m == 32 && r[0] == 1);
        CHECK((r[1] | (r[2] << 8) | (r[3] << 16) | ((uint32_t)r[4] << 24)) == 0x12345678u);
        /* ESC n set index 1; the completed raster block advanced it to 2 */
        CHECK(r[5] == 2 && r[9] == 100);
    }
    {
        const uint8_t tail[] = { 0x1B, 'G', 0x1B, 'E', 0x1B, 'Q' };
        CHECK(send_stream(tail, sizeof tail) == 0);
        for (int i = 0; i < 5; i++) protocol_task();
        CHECK(g_feed > LINES);                      /* lines + inter-label + tear feed */
        const uint8_t q[] = { 0x1B, 'A', 0x00 };
        send_stream(q, sizeof q);
        CHECK(read_reply(r) == 32 && r[0] == 0);    /* idle after ESC Q */
    }

    /* 3) ESC U and ESC V through the IN endpoint. */
    {
        const uint8_t u[] = { 0x1B, 'U' };
        send_stream(u, sizeof u);
        CHECK(read_reply(r) == 63 && r[0] == 0xB6 && r[1] == 0xCA && r[3] == 0x3C);
        const uint8_t v[] = { 0x1B, 'V' };
        send_stream(v, sizeof v);
        CHECK(read_reply(r) == 34 && memcmp(r + 16, "FWAP", 4) == 0);
        CHECK((r[32] | (r[33] << 8)) == MODEL_PID);
    }

    /* 4) SOFT_RESET in the middle of a raster: the pipeline drops the rest of
     *    the label, a reply queued before it is discarded, and the very next
     *    command is parsed normally. */
    {
        uint8_t part[12 + 5 * HEAD_BYTES];
        size_t k = 0;
        part[k++] = 0x1B; part[k++] = 'D'; part[k++] = 1; part[k++] = 2;
        put32(&part[k], 50); k += 4;
        put32(&part[k], HEAD_DOTS); k += 4;
        memset(part + k, 0xFF, 5 * HEAD_BYTES); k += 5 * HEAD_BYTES;
        g_lines = 0;
        send_stream(part, k);
        for (int i = 0; i < 5; i++) protocol_task();
        CHECK(g_lines == 5);
        const uint8_t q[] = { 0x1B, 'A', 0x00 };
        send_stream(q, sizeof q);                    /* eaten as raster bytes */
        CHECK(ctrl_nodata(0x21, 2, 0, 0) == 0);      /* SOFT_RESET */
        const uint8_t idx[] = { 0x1B, 'n', 9, 0, 0x1B, 'A', 0x00 };
        send_stream(idx, sizeof idx);
        CHECK(read_reply(r) == 32 && r[5] == 9);
        CHECK(g_lines == 5);                         /* nothing printed after the reset */
    }

    /* 4b) The printer-class SOFT_RESET must also clear the job id, or DYMO's
     *     language monitor can never re-acquire the lock after a host crash. */
    {
        const uint8_t job[] = { 0x1B, 's', 0x78, 0x56, 0x34, 0x12 };
        const uint8_t q[] = { 0x1B, 'A', 0x00 };
        send_stream(job, sizeof job);
        send_stream(q, sizeof q);
        CHECK(read_reply(r) == 32 && r[0] == 1 && r[1] == 0x78);
        CHECK(ctrl_nodata(0x21, 2, 0, 0) == 0);
        send_stream(q, sizeof q);
        CHECK(read_reply(r) == 32 && r[0] == 0 && r[1] == 0 && r[2] == 0 &&
              r[3] == 0 && r[4] == 0);
    }

    /* 5) Backdoor over the same path: GS D 0x05 build id. */
    {
        const uint8_t d[] = { 0x1D, 'D', 0x05 };
        send_stream(d, sizeof d);
        int m = read_reply(r);
        CHECK(m >= 3 && r[0] == 'D' && r[1] == 0x05);
    }

    /* 6) A firmware-update attempt is refused through the real IN endpoint. */
    {
        uint8_t up[6 + 128];
        up[0] = 0x1B; up[1] = 'R'; up[2] = 0; up[3] = 1; up[4] = 0x00; up[5] = 0xF1;
        memset(up + 6, 0xA5, 128);
        send_stream(up, sizeof up);
        CHECK(read_reply(r) == 3 && r[0] == 0x1B && r[1] == 'r' && r[2] != 0);
    }

    printf(fails ? "\n%d of %d END-TO-END check(s) FAILED\n" : "\nALL %d END-TO-END CHECKS PASSED\n",
           fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
