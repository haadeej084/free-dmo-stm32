/* OpenDMO-FW - host-unittest for the protocol parser (src/printer/protocol.c).
 *
 * Compiles and runs on the PC (no board): the hardware-dependent calls are
 * mocked. Exercises the genuine LabelWriter 550/5XL wire protocol per
 * LW550_TECHREF.txt: ESC s (job), ESC D (raster), ESC A (32-byte status),
 * ESC L (paper), ESC n (index), ESC G/E (feed), ESC U (SKU record), plus the
 * backdoor GS C (config). Also re-tests the resumable-across-ring-underflow
 * property: a raster split across many feed/task rounds must not corrupt.
 *
 * Build/run (host compiler):
 *   cc -I../src -o test_protocol test_protocol.c ../src/printer/protocol.c && ./test_protocol
 */
#include <stdio.h>
#include <string.h>
#include "printer/protocol.h"
#include "config/store.h"
#include "pins.h"

/* ---- mocks of the hardware/subsystem layer ------------------------------ */
static int   g_lines;              /* head_print_line calls */
static int   g_feed;               /* sum of feed lines */
static int   g_density = -1;
static int   g_reply_len = -1;
static unsigned char g_reply[256];
static op_config_t g_cfg;

void head_init(void){}
void head_reset(void){}
void head_print_line(const unsigned char *b, unsigned short n){ (void)b;(void)n; g_lines++; }
void head_set_density(unsigned char d){ g_density = d; }
void motor_init(void){}
void motor_enable(int on){ (void)on; }
void motor_step_lines(unsigned short n){ g_feed += n; }
void thermal_init(void){}
unsigned short thermal_read_raw(void){ return 0; }
int thermal_ok(void){ return 1; }
unsigned short thermal_dwell_scale(void){ return 256; }
void store_init(void){}
const op_config_t *store_get(void){ return &g_cfg; }
op_config_t *store_get_mut(void){ return &g_cfg; }
int store_save(void){ return 0; }
int store_selftest(void){ return 1; }
void store_load(void){}
void usb_ep_rx_ready(unsigned char ep){ (void)ep; }
int  usb_is_configured(void){ return 1; }
int  usbp_send_reply(const unsigned char *d, unsigned short n){
    g_reply_len = n; if (n <= sizeof g_reply) memcpy(g_reply, d, n); return n; }
static int g_paper_present = 1;
void usbp_set_paper_present(int p){ g_paper_present = p; }
int  usbp_paper_present(void){ return g_paper_present; }
int  gpio_get(pin_t p){ (void)p; return PAPER_PRESENT_LEVEL; }  /* paper present */
void delay_ms(unsigned int ms){ (void)ms; }
void wdt_kick(void){}

/* ---- harness ------------------------------------------------------------- */
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
                     else printf("ok   %s\n", #c); }while(0)

static void reset_state(void){ g_lines=0; g_feed=0; g_density=-1; g_reply_len=-1;
                               memset(&g_cfg,0,sizeof g_cfg); protocol_init(); }

/* Feed one byte at a time with a task round between: forces underflow resume. */
static void feed_bytewise(const unsigned char *d, int n){
    for (int i=0;i<n;i++){ protocol_feed(&d[i],1); protocol_task(); }
}

/* ESC D header: BPP=1, Align=0, W(lines) u32 LE, H(dots) u32 LE. */
static void esc_d(unsigned char *o, unsigned lines, unsigned dots){
    o[0]=0x1B; o[1]='D'; o[2]=1; o[3]=0;
    o[4]=lines&0xFF; o[5]=(lines>>8)&0xFF; o[6]=(lines>>16)&0xFF; o[7]=(lines>>24)&0xFF;
    o[8]=dots&0xFF;  o[9]=(dots>>8)&0xFF;  o[10]=(dots>>16)&0xFF; o[11]=(dots>>24)&0xFF;
}

int main(void){
    /* 1) Job: ESC s + ESC D (3 lines x 2 bytes = 16 dots/line) -> 3 head lines. */
    reset_state();
    unsigned char job[64]; int n=0;
    job[n++]=0x1B; job[n++]='s'; job[n++]=1; job[n++]=0; job[n++]=0; job[n++]=0; /* JobID=1 */
    esc_d(&job[n], 3, 16); n+=12;
    for (int i=0;i<6;i++) job[n++] = 0xFF;               /* 3 lines x 2 bytes */
    protocol_feed(job, n); protocol_task();
    CHECK(g_lines == 3);

    /* 2) Same job byte-by-byte across underflow. */
    reset_state();
    feed_bytewise(job, n);
    CHECK(g_lines == 3);

    /* 3) Status ESC A -> 32 bytes; byte9=density(100), byte10=bay present(8). */
    reset_state();
    unsigned char q[] = { 0x1B, 'A', 0x00 };
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply_len == 32);
    CHECK(g_reply[9] == 100);        /* default density 100 % */
    CHECK(g_reply[10] == 8);         /* media present ok */

    /* 4) Density ESC C 150 -> status byte9 = 150. */
    reset_state();
    unsigned char d[] = { 0x1B, 'C', 150 };
    protocol_feed(d, sizeof d); protocol_task();
    CHECK(g_reply_len == -1);        /* ESC C does not reply */
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[9] == 150);

    /* 5) Label index ESC n 7 -> status byte5/6 = 7. */
    reset_state();
    unsigned char idx[] = { 0x1B, 'n', 7, 0 };
    protocol_feed(idx, sizeof idx); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 7 && g_reply[6] == 0);

    /* 6) Job ID ESC s 0x1234 -> status byte1..4 = 0x1234 LE. */
    reset_state();
    unsigned char js[] = { 0x1B, 's', 0x34, 0x12, 0, 0 };
    protocol_feed(js, sizeof js); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[1] == 0x34 && g_reply[2] == 0x12 && g_reply[0] == 1); /* printing */

    /* 7) Feed ESC G -> motor advances (gap + pitch). */
    reset_state();
    unsigned char g[] = { 0x1B, 'G' };
    protocol_feed(g, sizeof g); protocol_task();
    CHECK(g_feed > 0);

    /* 8) SKU record ESC U -> 63 bytes, magic 0xCAB6 LE at [0..1]. */
    reset_state(); strcpy(g_cfg.sku, "S0904980"); g_cfg.label_count = 220;
    unsigned char u[] = { 0x1B, 'U' };
    protocol_feed(u, sizeof u); protocol_task();
    CHECK(g_reply_len == 63);
    CHECK(g_reply[0] == 0xB6 && g_reply[1] == 0xCA);

    /* 9) Backdoor GS C: count=500, SKU="ABC". */
    reset_state();
    unsigned char cfg[] = { 0x1D, 'C', 3, 0xF4, 0x01, 'A','B','C' };
    protocol_feed(cfg, sizeof cfg); protocol_task();
    CHECK(g_cfg.label_count == 500);
    CHECK(strcmp(g_cfg.sku, "ABC") == 0);

    /* 10) ESC Q ends job -> status byte0 back to idle (0). */
    reset_state();
    unsigned char end[] = { 0x1B, 'Q' };
    protocol_feed(end, sizeof end); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[0] == 0);

    /* 11) ESC M media-type: the 8 payload bytes are skipped even when they look
     *     like commands, and the parser returns to S_CMD (a following ESC n works). */
    reset_state();
    unsigned char m[] = { 0x1B, 'M',
                          0x1B,'s',1,2,3,4, 0x1B,'A' };   /* 8 adversarial bytes */
    protocol_feed(m, sizeof m); protocol_task();
    CHECK(g_reply_len == -1);                       /* no reply from within payload */
    unsigned char idx5[] = { 0x1B, 'n', 5, 0 };
    protocol_feed(idx5, sizeof idx5); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 5 && g_reply[6] == 0);      /* ESC n parsed after the skip */

    /* 12) GS D snapshot (0x04): 24-byte 'D'-prefixed reply, model id at [2]. */
    reset_state();
    unsigned char d4[] = { 0x1D, 'D', 0x04 };
    protocol_feed(d4, sizeof d4); protocol_task();
    CHECK(g_reply_len == 24);
    CHECK(g_reply[0] == 0x44 && g_reply[1] == 0x04);
    CHECK(g_reply[2] == (MODEL_PID & 0xFF));

    /* 13) GS D head strobe (0x01 n): 4-byte reply, head fires n lines. */
    reset_state();
    unsigned char d1[] = { 0x1D, 'D', 0x01, 5 };
    protocol_feed(d1, sizeof d1); protocol_task();
    CHECK(g_reply_len == 4);
    CHECK(g_reply[2] == 5 && g_lines == 5);

    /* 14) GS D motor step (0x02 n): 3-byte reply, feed advances n lines. */
    reset_state();
    unsigned char d2[] = { 0x1D, 'D', 0x02, 30 };
    protocol_feed(d2, sizeof d2); protocol_task();
    CHECK(g_reply_len == 3);
    CHECK(g_reply[2] == 30 && g_feed == 30);

    /* 15) GS D EEPROM self-test (0x03): 3-byte reply, match flag at [2]. */
    reset_state();
    unsigned char d3[] = { 0x1D, 'D', 0x03 };
    protocol_feed(d3, sizeof d3); protocol_task();
    CHECK(g_reply_len == 3);
    CHECK(g_reply[2] == 1);                        /* mock store_selftest -> 1 */

    /* 16) ESC W control command: the 4-byte header + N payload bytes are consumed
     *     (payload looks like commands), then the parser RESUMES (regression for
     *     the old S_ESC_W that never returned to S_CMD). */
    reset_state();
    unsigned char w[] = { 0x1B, 'W', 4, 0, 0, 0,   /* len=4 dir=0 obj=0 */
                          0x1B, 'A', 0x00, 0x1B }; /* 4 adversarial payload bytes */
    protocol_feed(w, sizeof w); protocol_task();
    CHECK(g_reply_len == -1);                      /* no reply from within the payload */
    unsigned char idx7[] = { 0x1B, 'n', 7, 0 };
    protocol_feed(idx7, sizeof idx7); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 7 && g_reply[6] == 0);     /* ESC n parsed after the W skip */

    /* 17) ESC V version: 34-byte reply, PID (LE) at [32-33]. */
    reset_state();
    unsigned char v[] = { 0x1B, 'V' };
    protocol_feed(v, sizeof v); protocol_task();
    CHECK(g_reply_len == 34);
    CHECK(g_reply[32] == (MODEL_PID & 0xFF) && g_reply[33] == ((MODEL_PID >> 8) & 0xFF));

    /* 18) ESC o set count: label_count updated in config. */
    reset_state();
    unsigned char oc[] = { 0x1B, 'o', 0xF4, 0x01 };   /* 500 LE */
    protocol_feed(oc, sizeof oc); protocol_task();
    CHECK(g_cfg.label_count == 500);

    /* 19) ESC * factory reset: config restored to the model defaults. */
    reset_state(); strcpy(g_cfg.sku, "XYZ"); g_cfg.label_count = 5;
    unsigned char fr[] = { 0x1B, '*' };
    protocol_feed(fr, sizeof fr); protocol_task();
    CHECK(strcmp(g_cfg.sku, MODEL_DEFAULT_SKU) == 0);
    CHECK(g_cfg.label_count == MODEL_DEFAULT_COUNT);

    /* 20) ESC @ pipeline reset: an in-progress job is cleared. */
    reset_state();
    unsigned char js2[] = { 0x1B, 's', 1, 0, 0, 0 };  /* start a job */
    protocol_feed(js2, sizeof js2); protocol_task();
    unsigned char at[] = { 0x1B, '@' };
    protocol_feed(at, sizeof at); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[0] == 0);                        /* job no longer active */

    /* 21) Back-to-back jobs: job1 (2 lines) + ESC Q, then job2 (3 lines) + ESC Q.
     *     The parser must handle two complete jobs in one stream. */
    reset_state();
    unsigned char jb[128]; int mj = 0;
    jb[mj++]=0x1B; jb[mj++]='s'; jb[mj++]=1; jb[mj++]=0; jb[mj++]=0; jb[mj++]=0;
    esc_d(&jb[mj], 2, 16); mj += 12;
    for (int i=0;i<4;i++) jb[mj++] = 0xFF;         /* 2 lines x 2 bytes */
    jb[mj++]=0x1B; jb[mj++]='Q';                   /* end job 1 */
    jb[mj++]=0x1B; jb[mj++]='s'; jb[mj++]=2; jb[mj++]=0; jb[mj++]=0; jb[mj++]=0;
    esc_d(&jb[mj], 3, 16); mj += 12;
    for (int i=0;i<6;i++) jb[mj++] = 0xFF;         /* 3 lines x 2 bytes */
    jb[mj++]=0x1B; jb[mj++]='Q';                   /* end job 2 */
    protocol_feed(jb, mj); protocol_task();
    CHECK(g_lines == 5);                           /* 2 + 3 lines across two jobs */

    /* 22) Full-width raster: H = HEAD_DOTS so bpl = HEAD_BYTES (per-model width).
     *     Verifies the wide-raster path (no centering offset) for the real head. */
    reset_state();
    unsigned char fw[18 + 3*HEAD_BYTES]; int k = 0;
    fw[k++]=0x1B; fw[k++]='s'; fw[k++]=1; fw[k++]=0; fw[k++]=0; fw[k++]=0;
    esc_d(&fw[k], 3, HEAD_DOTS); k += 12;
    for (int i=0;i<3*HEAD_BYTES;i++) fw[k++] = 0xFF;
    protocol_feed(fw, k); protocol_task();
    CHECK(g_lines == 3);                           /* full-width lines print cleanly */

    /* 23) ESC W length clamp: len=255 clamps to 250, so a command after 250
     *     padding bytes is still parsed (not eaten). */
    reset_state();
    unsigned char wb[6 + 250 + 4];
    wb[0]=0x1B; wb[1]='W'; wb[2]=0xFF; wb[3]=0; wb[4]=0; wb[5]=0;   /* len=255 */
    for (int i=0;i<250;i++) wb[6+i] = 0;          /* 250 zero payload bytes */
    wb[6+250]=0x1B; wb[7+250]='n'; wb[8+250]=9; wb[9+250]=0;        /* ESC n 9 */
    protocol_feed(wb, sizeof wb); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 9 && g_reply[6] == 0);     /* parsed after the clamped skip */

    printf(fails ? "\n%d test(s) FAILED\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

#if defined(__arm__) || defined(__ARM_ARCH)
/* newlib freestanding syscall stubs so the test links under arm-none-eabi
 * (the firmware's own startup.c provides these on-target). */
#include <sys/types.h>
#include <sys/stat.h>
int     _close(int f){ (void)f; return 0; }
int     _fstat(int f, struct stat *st){ (void)f; st->st_mode = S_IFCHR; return 0; }
int     _isatty(int f){ (void)f; return 1; }
off_t   _lseek(int f, off_t o, int w){ (void)f; (void)w; return o; }
int     _open(const char *n, int fl, int m){ (void)n;(void)fl;(void)m; return -1; }
int     _read(int f, void *b, unsigned n){ (void)f;(void)b;(void)n; return 0; }
int     _write(int f, const void *b, unsigned n){ (void)f;(void)b;(void)n; return (int)n; }
void   *_sbrk(ptrdiff_t i){ static char heap[16384]; static char *p = heap;
    char *r = p; if (i > 0 && p + i <= heap + sizeof heap) p += i; return r; }
void    _exit(int s){ (void)s; for(;;); }
int     _kill(int pid, int sig){ (void)pid;(void)sig; return 0; }
int     _getpid(void){ return 1; }
#endif
