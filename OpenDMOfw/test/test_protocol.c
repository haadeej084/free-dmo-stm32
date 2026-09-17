/* OpenDMOfw - host-unittest for the protocol parser (src/printer/protocol.c).
 *
 * Compiles and runs on the PC (no board): the hardware-dependent calls are
 * mocked. Exercises the genuine LabelWriter 550/5XL wire protocol per
 * the D.mo tech ref: ESC s (job), ESC D (raster), ESC A (32-byte status),
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
void motor_step_line_after(unsigned int us){ (void)us; g_feed += 1; }
void motor_idle_tick(unsigned int ms){ (void)ms; }
unsigned int head_last_strobe_us(void){ return 0; }
void head_idle_tick(unsigned int ms){ (void)ms; }
static int g_vh_on = 0;
int  head_vh_is_on(void){ return g_vh_on; }
void head_vh_off(void){ g_vh_on = 0; }
static unsigned short g_adc[10] = {10,20,30,40,50,60,70,80,90,100};
void thermal_scan_adc(unsigned short *out){ for (int i=0;i<10;i++) out[i]=g_adc[i]; }
static unsigned short g_idr[3] = {0x1111, 0x2222, 0x4444};
unsigned short sys_port_idr(unsigned char port){ return port < 3 ? g_idr[port] : 0; }
static int g_toggle_port = -1, g_toggle_pin = -1, g_toggle_n = -1;
int sys_pin_toggle(unsigned char port, unsigned char pin, unsigned char n){
    if (port > 2 || pin > 15) return 0;
    if (port == 0 && (pin == 11 || pin == 12 || pin == 13 || pin == 14)) return 0;
    g_toggle_port = port; g_toggle_pin = pin; g_toggle_n = n; return 1; }
void thermal_init(void){}
unsigned short thermal_read_raw(void){ return 0; }
/* The D7 thermal gate is the one safety interlock that is pure software, so
 * the mock has to be able to express both shapes it can be in: over the limit
 * for a while and then cool (the normal case, where the firmware waits and
 * then prints), and over the limit indefinitely (where the two call sites take
 * DELIBERATELY OPPOSITE decisions - see cases 61 and 62). A fixed flag can
 * only ever express one of them, which is why the gate had no coverage.
 *   g_hot_polls > 0: report not-ok for the next N polls, then ok.
 *   g_hot_polls < 0: never ok. */
static int g_hot_polls = 0;
static int g_delays = 0;           /* delay_ms() calls, i.e. cool-down polls */
int thermal_ok(void){
    if (g_hot_polls < 0) return 0;
    if (g_hot_polls > 0) { g_hot_polls--; return 0; }
    return 1;
}
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
void delay_ms(unsigned int ms){ (void)ms; g_delays++; }
#include <setjmp.h>
static jmp_buf g_dfu_jmp;
static int     g_dfu_calls;
void sys_enter_bootloader(void){ g_dfu_calls++; longjmp(g_dfu_jmp, 1); }
void wdt_kick(void){}

/* ---- harness ------------------------------------------------------------- */
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
                     else printf("ok   %s\n", #c); }while(0)

static void reset_state(void){ g_lines=0; g_feed=0; g_density=-1; g_reply_len=-1;
                               g_hot_polls=0; g_delays=0; g_paper_present=1; g_vh_on=0;
                               g_toggle_port=-1; g_toggle_pin=-1; g_toggle_n=-1;
                               memset(&g_cfg,0,sizeof g_cfg); protocol_init(); }

/* Feed one byte at a time with a task round between: forces underflow resume. */
static void feed_bytewise(const unsigned char *d, int n){
    for (int i=0;i<n;i++){ protocol_feed(&d[i],1); protocol_task(); }
}

/* ESC D header: BPP=1, Align=2, W(lines) u32 LE, H(dots) u32 LE. */
static void esc_d(unsigned char *o, unsigned lines, unsigned dots){
    o[0]=0x1B; o[1]='D'; o[2]=1; o[3]=2;      /* Align 2 = bottom (tech ref p.12) */
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
     *     (payload looks like commands), then the parser RESUMES at the command
     *     state — it must not stay inside the payload. */
    reset_state();
    unsigned char w[] = { 0x1B, 'W', 8, 0, 0, 0,   /* len=8: 4 header + 4 payload */
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

    /* 18) ESC o takes ONE count byte (tech ref p.20: 'ESC' 'o' Count). A host
     *     that sends a u16 instead leaves a 0x00 high byte behind, which S_CMD
     *     ignores as a stray - so a following command still parses. */
    reset_state();
    unsigned char oc[] = { 0x1B, 'o', 200 };
    protocol_feed(oc, sizeof oc); protocol_task();
    CHECK(g_cfg.label_count == 200);
    reset_state();
    unsigned char oc16[] = { 0x1B, 'o', 220, 0x00, 0x1B, 'n', 21, 0 };
    protocol_feed(oc16, sizeof oc16); protocol_task();
    CHECK(g_cfg.label_count == 220);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 21);                       /* stray high byte did not desync */

    /* 19) ESC * factory reset: config restored to the model defaults. */
    reset_state(); strcpy(g_cfg.sku, "XYZ"); g_cfg.label_count = 5; g_cfg.flags = 0;
    unsigned char fr[] = { 0x1B, '*' };
    protocol_feed(fr, sizeof fr); protocol_task();
    CHECK(strcmp(g_cfg.sku, MODEL_DEFAULT_SKU) == 0);
    CHECK(g_cfg.label_count == MODEL_DEFAULT_COUNT);
    CHECK(g_cfg.flags == OP_FLAG_PAPER_FORCE);

    /* 19b) Counter wrap: last remaining label -> MODEL_DEFAULT_COUNT. */
    reset_state(); g_cfg.label_count = 1;
    unsigned char wrap[64]; int nw = 0;
    wrap[nw++]=0x1B; wrap[nw++]='s'; wrap[nw++]=1; wrap[nw++]=0; wrap[nw++]=0; wrap[nw++]=0;
    esc_d(&wrap[nw], 1, 16); nw += 12;
    wrap[nw++] = 0xFF; wrap[nw++] = 0xFF;
    protocol_feed(wrap, nw); protocol_task();
    CHECK(g_lines == 1);
    CHECK(g_cfg.label_count == MODEL_DEFAULT_COUNT);

    /* 19c) ESC L u16 LE paper code 0x0867 is bytes 67 08 (5XL shipping). */
    reset_state();
    unsigned char el[] = { 0x1B, 'L', 0x08, 0x67 };   /* ESC L is BIG-endian */
    protocol_feed(el, sizeof el); protocol_task();
    unsigned char n1[] = { 0x1B, 'n', 3, 0 };
    protocol_feed(n1, sizeof n1); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 3);

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

    /* 23) ESC W maximum length: len=255 is 4 header bytes + 251 payload bytes
     *     (decompiled ControlCommand: len = payload + 6 - 2), so a command
     *     right after 251 payload bytes is parsed, not eaten. */
    reset_state();
    unsigned char wb[6 + 251 + 4];
    wb[0]=0x1B; wb[1]='W'; wb[2]=0xFF; wb[3]=0; wb[4]=0; wb[5]=0;   /* len=255 */
    for (int i=0;i<251;i++) wb[6+i] = 0;          /* 251 zero payload bytes */
    wb[6+251]=0x1B; wb[7+251]='n'; wb[8+251]=9; wb[9+251]=0;        /* ESC n 9 */
    protocol_feed(wb, sizeof wb); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 9 && g_reply[6] == 0);     /* parsed right after the payload */

    /* 24) ESC $ (0x24) is the tech-ref byte for "restore factory settings" and
     *     the one tools/opsend.py sends. It must reset the config AND leave the
     *     parser at S_CMD — the old code only knew 0x2A and fell into the
     *     unknown-command branch, which swallowed the next byte. */
    reset_state(); strcpy(g_cfg.sku, "XYZ"); g_cfg.label_count = 5;
    unsigned char fr24[] = { 0x1B, 0x24, 0x1B, 'n', 4, 0 };   /* ESC $ then ESC n 4 */
    protocol_feed(fr24, sizeof fr24); protocol_task();
    CHECK(strcmp(g_cfg.sku, MODEL_DEFAULT_SKU) == 0);
    CHECK(g_cfg.label_count == MODEL_DEFAULT_COUNT);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 4);                        /* next command NOT swallowed */

    /* 25) Over-wide raster: H = HEAD_DOTS + 64 means the host puts
     *     HEAD_BYTES + 8 bytes on the wire per line. The surplus must be
     *     consumed (not printed, not re-read as the next line), so 2 lines
     *     print and a following ESC n still parses. */
    reset_state();
    {
        int ow_bpl = HEAD_BYTES + 8;
        static unsigned char ow[18 + 2 * (HEAD_BYTES + 8) + 4];
        int j = 0;
        ow[j++]=0x1B; ow[j++]='s'; ow[j++]=1; ow[j++]=0; ow[j++]=0; ow[j++]=0;
        esc_d(&ow[j], 2, HEAD_DOTS + 64); j += 12;
        for (int i=0;i<2*ow_bpl;i++) ow[j++] = 0xFF;
        ow[j++]=0x1B; ow[j++]='n'; ow[j++]=6; ow[j++]=0;
        protocol_feed(ow, j); protocol_task();
        CHECK(g_lines == 2);                       /* exactly 2 lines, no drift */
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 6);                    /* stream stayed in sync */
    }

    /* 26) Absurd ESC D header (H > 0xFFFF): the block length is unknowable, so
     *     it is dropped rather than half-consumed, and the parser resyncs. */
    reset_state();
    {
        unsigned char bad[6 + 12 + 4]; int j = 0;
        bad[j++]=0x1B; bad[j++]='s'; bad[j++]=1; bad[j++]=0; bad[j++]=0; bad[j++]=0;
        esc_d(&bad[j], 2, 0x20000); j += 12;
        bad[j++]=0x1B; bad[j++]='n'; bad[j++]=8; bad[j++]=0;
        protocol_feed(bad, j); protocol_task();
        CHECK(g_lines == 0);                       /* nothing printed */
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 8);                    /* ESC n after it still parsed */
    }

    /* 27) DECISIONS D7: GS D 0x01 runs all dots on at maximum dwell, so it must
     *     refuse to fire while the head is over its limit and report 0 lines. */
    reset_state(); g_hot_polls = -1;
    unsigned char hot[] = { 0x1D, 'D', 0x01, 5 };
    protocol_feed(hot, sizeof hot); protocol_task();
    CHECK(g_reply_len == 4);
    CHECK(g_lines == 0 && g_reply[2] == 0);        /* nothing fired */
    CHECK(g_reply[3] == 0);                        /* thermal_ok reported false */

    /* 28) ESC U geometry is in TENTHS of a millimetre, proven against 37 genuine
     *     roll-tag dumps in this repo's own Src/main.c (SKU 30256 carries
     *     1016 x 587 = exactly 4" x 2.3125"). The liner at [48-49] is the head
     *     width in tenths: OP104 1248 dots -> 1057, OP57 672 -> 569. */
    reset_state(); strcpy(g_cfg.sku, "S0904980"); g_cfg.label_count = 220;
    {
        unsigned liner = ((unsigned)HEAD_DOTS * 254u + MODEL_DPI / 2u) / MODEL_DPI;
        protocol_feed(u, sizeof u); protocol_task();
        CHECK(g_reply_len == 63);
        CHECK((unsigned)(g_reply[48] | (g_reply[49] << 8)) == liner);
    }

    /* 29) Density mapping: ESC C duty -> head level 0..16, and the status byte
     *     echoes the percentage. 0 = heat off, 200 = full, >200 clamps to 200. */
    reset_state();
    unsigned char c0[] = { 0x1B, 'C', 0 };
    protocol_feed(c0, sizeof c0); protocol_task();
    CHECK(g_density == 0);                         /* heat off */
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[9] == 0);
    reset_state();
    unsigned char c100[] = { 0x1B, 'C', 100 };
    protocol_feed(c100, sizeof c100); protocol_task();
    CHECK(g_density == 8);                         /* 100 % = reference level */
    reset_state();
    unsigned char c250[] = { 0x1B, 'C', 250 };
    protocol_feed(c250, sizeof c250); protocol_task();
    CHECK(g_density == 16);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[9] == 200);                      /* clamped, not wrapped */

    /* 30) ESC e resets density to the 100 % default. */
    reset_state();
    protocol_feed(c250, sizeof c250); protocol_task();
    unsigned char ce[] = { 0x1B, 'e' };
    protocol_feed(ce, sizeof ce); protocol_task();
    CHECK(g_density == 8);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[9] == 100);

    /* 31) Accept-and-ignore commands must consume exactly their argument and
     *     leave the stream in sync: ESC T <speed>, ESC q <tray>, ESC h, ESC i. */
    reset_state();
    unsigned char misc[] = { 0x1B, 'T', 0x20,      /* speed high        */
                             0x1B, 'q', 0x01,      /* tray              */
                             0x1B, 'h',            /* text mode         */
                             0x1B, 'i',            /* graphics mode     */
                             0x1B, 'n', 11, 0 };   /* must still parse  */
    protocol_feed(misc, sizeof misc); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 11 && g_reply[6] == 0);

    /* 32) ESC W with len=0: header only, no payload, parser back at S_CMD. */
    reset_state();
    unsigned char w0[] = { 0x1B, 'W', 0, 0, 0, 0, 0x1B, 'n', 12, 0 };
    protocol_feed(w0, sizeof w0); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 12 && g_reply[6] == 0);

    /* 33) GS C with a SKU longer than OP_SKU_MAX: stored truncated and always
     *     NUL-terminated, and the parser still resyncs afterwards. */
    reset_state();
    {
        unsigned char gc[5 + 30 + 4]; int j = 0;
        gc[j++]=0x1D; gc[j++]='C'; gc[j++]=30; gc[j++]=0x10; gc[j++]=0x00; /* 16 */
        for (int i=0;i<30;i++) gc[j++] = (unsigned char)('A' + (i % 26));
        gc[j++]=0x1B; gc[j++]='n'; gc[j++]=13; gc[j++]=0;
        protocol_feed(gc, j); protocol_task();
        CHECK(g_cfg.label_count == 16);
        CHECK(strlen(g_cfg.sku) == OP_SKU_MAX - 1);   /* truncated, terminated */
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 13);                      /* stream still in sync */
    }

    /* 34) ESC L 0 (die-cut) must CLEAR a previous raw length override — 0 is
     *     what the stock driver sends for every die-cut job. */
    reset_state();
    unsigned char l1000[] = { 0x1B, 'L', 0x03, 0xE8 };   /* 1000 dots, raw, BE */
    protocol_feed(l1000, sizeof l1000); protocol_task();
    protocol_feed(g, sizeof g); protocol_task();
    CHECK(g_feed == 1000 + 20);                    /* override + gap */
    {
        int after_override = g_feed;
        unsigned char l0[] = { 0x1B, 'L', 0, 0 };
        protocol_feed(l0, sizeof l0); protocol_task();
        protocol_feed(g, sizeof g); protocol_task();
        CHECK((g_feed - after_override) != 1000 + 20);  /* back to the paper table */
    }

    /* 35) The printed height of one job must not leak into the next: a feed
     *     issued before the new job's first ESC D advances a full pitch. */
    reset_state();
    protocol_feed(g, sizeof g); protocol_task();
    {
        int base_feed = g_feed;                    /* gap + full pitch, no raster */
        reset_state();
        unsigned char jj[64]; int j = 0;
        jj[j++]=0x1B; jj[j++]='s'; jj[j++]=1; jj[j++]=0; jj[j++]=0; jj[j++]=0;
        esc_d(&jj[j], 1, 16); j += 12;
        jj[j++]=0xFF; jj[j++]=0xFF;                /* one 16-dot line */
        jj[j++]=0x1B; jj[j++]='Q';                 /* end job 1 */
        jj[j++]=0x1B; jj[j++]='s'; jj[j++]=2; jj[j++]=0; jj[j++]=0; jj[j++]=0;
        protocol_feed(jj, j); protocol_task();
        int before = g_feed;
        protocol_feed(g, sizeof g); protocol_task();
        CHECK(g_feed - before == base_feed);
    }

    /* 36) GS D 0x05 reports the build id stamped at compile time. */
    reset_state();
    unsigned char d5[] = { 0x1D, 'D', 0x05 };
    protocol_feed(d5, sizeof d5); protocol_task();
    {
        size_t bl = strlen(OPENDMO_BUILD);
        if (bl > OP_BUILD_ID_MAX) bl = OP_BUILD_ID_MAX;
        CHECK(g_reply_len == (int)(2 + bl) && g_reply_len <= 2 + OP_BUILD_ID_MAX);
        CHECK(g_reply[0] == 'D' && g_reply[1] == 0x05);
        CHECK(memcmp(&g_reply[2], OPENDMO_BUILD, bl) == 0);
    }

    /* 37) ESC f 1 n - "Skip n Lines" from the LabelWriter 450 tech ref. */
    reset_state();
    unsigned char sk[] = { 0x1B, 'f', 1, 40, 0x1B, 'n', 22, 0 };
    protocol_feed(sk, sizeof sk); protocol_task();
    CHECK(g_feed == 40);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 22);                       /* three-byte form consumed exactly */
    reset_state();
    unsigned char sk2[] = { 0x1B, 'f', 9, 40, 0x1B, 'n', 23, 0 };  /* unknown sub */
    protocol_feed(sk2, sizeof sk2); protocol_task();
    CHECK(g_feed == 0);                            /* ignored, not fed */
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 23);                       /* and still in sync */

    /* 38) A single feed is clamped. Continuous/banner stock in the paper table
     *     has a nominal height of 32000 dots; without the clamp one ESC G would
     *     spool out 2.7 metres of paper. */
    reset_state();
    unsigned char lbig[] = { 0x1B, 'L', 0x7F, 0xFF };   /* 32767 raw dot length, BE */
    protocol_feed(lbig, sizeof lbig); protocol_task();
    protocol_feed(g, sizeof g); protocol_task();
    CHECK(g_feed > 0 && g_feed <= 4000);

    /* 39) ESC U total label count is the ROLL total, not what is left on it
     *     (tech ref p.19); the remaining count lives in the status struct. */
    reset_state();
    strcpy(g_cfg.sku, "S0904980"); g_cfg.label_count = 3;   /* nearly empty */
    protocol_feed(u, sizeof u); protocol_task();
    CHECK(g_reply_len == 63);
    CHECK((g_reply[50] | (g_reply[51] << 8)) == MODEL_DEFAULT_COUNT);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK((g_reply[27] | (g_reply[28] << 8)) == 3);   /* status still says 3 left */

    /* 40) Button actions work with no host: form feed advances, and the
     *     built-in self test prints its canned pattern. */
    reset_state();
    protocol_form_feed();
    CHECK(g_feed > 0);
    reset_state();
    protocol_self_test();
    CHECK(g_lines == 400);                         /* SELFTEST_LINES */
    CHECK(g_feed >= 400);                          /* stepped per line + tear feed */

    /* 41) Neither button action may interrupt a running host job. */
    reset_state();
    protocol_feed(js2, sizeof js2); protocol_task();   /* ESC s: job active */
    {
        int before = g_feed, lines_before = g_lines;
        protocol_form_feed();
        protocol_self_test();
        CHECK(g_feed == before && g_lines == lines_before);
    }

    /* 42) GS D 0x06 scan: 29-byte reply with all ten ADC channels (u16 BE) and
     *     the input levels of ports A/B/C (u16 LE), and the rail is dropped
     *     first because sampling floats pins. */
    reset_state(); g_vh_on = 1;
    unsigned char sc[] = { 0x1D, 'D', 0x06 };
    protocol_feed(sc, sizeof sc); protocol_task();
    CHECK(g_reply_len == 29);
    CHECK(g_reply[0] == 'D' && g_reply[1] == 0x06);
    CHECK(((g_reply[2] << 8) | g_reply[3]) == 10);
    CHECK(((g_reply[20] << 8) | g_reply[21]) == 100);
    CHECK((g_reply[22] | (g_reply[23] << 8)) == 0x1111);
    CHECK((g_reply[26] | (g_reply[27] << 8)) == 0x4444);
    CHECK(g_vh_on == 0);

    /* 43) GS D 0x07 toggle: three argument bytes, and the pins that carry this
     *     very command are refused instead of ending the session. */
    reset_state();
    unsigned char tg[] = { 0x1D, 'D', 0x07, 1, 4, 20 };
    protocol_feed(tg, sizeof tg); protocol_task();
    CHECK(g_reply_len == 3 && g_reply[1] == 0x07 && g_reply[2] == 1);
    CHECK(g_toggle_port == 1 && g_toggle_pin == 4 && g_toggle_n == 20);
    reset_state();
    unsigned char tgu[] = { 0x1D, 'D', 0x07, 0, 12, 5, 0x1B, 'n', 31, 0 };
    protocol_feed(tgu, sizeof tgu); protocol_task();
    CHECK(g_reply[2] == 0 && g_toggle_port == -1);
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[5] == 31);

    /* 44) GS D 0x08 VH interlock: persisted in the config, and setting it drops
     *     the rail immediately. */
    reset_state(); g_vh_on = 1;
    unsigned char vh1[] = { 0x1D, 'D', 0x08, 1 };
    protocol_feed(vh1, sizeof vh1); protocol_task();
    CHECK(g_reply_len == 3 && (g_cfg.flags & OP_FLAG_VH_INHIBIT));
    CHECK(g_vh_on == 0);
    unsigned char vh0[] = { 0x1D, 'D', 0x08, 0 };
    protocol_feed(vh0, sizeof vh0); protocol_task();
    CHECK(!(g_cfg.flags & OP_FLAG_VH_INHIBIT));

    /* 45) ESC U record header, checked against the 37 genuine roll-tag dumps in
     *     this repo's own Src/main.c: byte 3 is a constant 0x3C (the 60-byte
     *     payload length, NOT the SKU length), and bytes 4-7 are a CRC-32/zlib
     *     over bytes 0..59 with 4..7 zeroed, stored little-endian. */
    reset_state(); strcpy(g_cfg.sku, "S0904980"); g_cfg.label_count = 220;
    protocol_feed(u, sizeof u); protocol_task();
    CHECK(g_reply_len == 63);
    CHECK(g_reply[3] == 0x3C);                     /* record length, not strlen */
    {
        unsigned char tmp[60];
        memcpy(tmp, g_reply, 60);
        tmp[4] = tmp[5] = tmp[6] = tmp[7] = 0;
        unsigned long crc = 0xFFFFFFFFul;
        for (int i = 0; i < 60; i++) {
            crc ^= tmp[i];
            for (int b = 0; b < 8; b++)
                crc = (crc & 1ul) ? ((crc >> 1) ^ 0xEDB88320ul) : (crc >> 1);
        }
        crc = ~crc & 0xFFFFFFFFul;
        unsigned long got = (unsigned long)g_reply[4] | ((unsigned long)g_reply[5] << 8)
                          | ((unsigned long)g_reply[6] << 16) | ((unsigned long)g_reply[7] << 24);
        CHECK(got == crc);
    }
    CHECK(g_reply[56] == 0x01);                    /* counter strategy, 37/37 */
    CHECK(g_reply[44] == 0 && g_reply[45] == 0 &&
          g_reply[46] == 0 && g_reply[47] == 0);   /* printable offsets, 37/37 */
    CHECK(g_reply[60] == 0 && g_reply[61] == 0 && g_reply[62] == 0);
    {   /* margin is a tenth of the count the record itself reports, which is
         * max(model default, configured) - so derive it, do not hardcode. */
        int total = g_reply[50] | (g_reply[51] << 8);
        CHECK((g_reply[54] | (g_reply[55] << 8)) == total / 10);
    }

    /* 46) The zero-argument density family. ESC d used to be our feed backdoor
     *     and would have eaten the next command's ESC; it is a genuine opcode. */
    reset_state();
    unsigned char dens[] = { 0x1B, 'c', 0x1B, 'n', 41, 0 };
    protocol_feed(dens, sizeof dens); protocol_task();
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[9] == 75);                       /* ESC c = Light 75 % */
    CHECK(g_reply[5] == 41);                       /* and it consumed no argument */
    reset_state();
    unsigned char dens2[] = { 0x1B, 'd', 0x1B, 'g' };
    protocol_feed(dens2, sizeof dens2); protocol_task();
    CHECK(g_feed == 0);                            /* ESC d no longer feeds paper */
    protocol_feed(q, sizeof q); protocol_task();
    CHECK(g_reply[9] == 113);                      /* ESC g = Dark 112.5 % */

    /* 47) ESC L direction, stated as a value the little-endian reading cannot
     *     produce: 0x0064 = 100 dots. Read LE it would be 25600. */
    reset_state();
    unsigned char lbe[] = { 0x1B, 'L', 0x00, 0x64 };
    protocol_feed(lbe, sizeof lbe); protocol_task();
    protocol_feed(g, sizeof g); protocol_task();
    CHECK(g_feed == 100 + 20);                     /* override 100 + gap */

    /* 48) ESC L sentinels: 7F 00 (custom size) and FF FF (continuous) are not
     *     lengths. With no raster printed yet the pitch is 0, so ESC G feeds
     *     only the gap - never 32512 or 65535 dots. */
    {
        unsigned char s1[] = { 0x1B, 'L', 0x7F, 0x00 };
        unsigned char s2[] = { 0x1B, 'L', 0xFF, 0xFF };
        reset_state();
        protocol_feed(s1, sizeof s1); protocol_task();
        protocol_feed(g, sizeof g); protocol_task();
        CHECK(g_feed == 20);
        reset_state();
        protocol_feed(s2, sizeof s2); protocol_task();
        protocol_feed(g, sizeof g); protocol_task();
        CHECK(g_feed == 20);
        /* a later real length clears the sentinel again */
        protocol_feed(lbe, sizeof lbe); protocol_task();
        g_feed = 0;
        protocol_feed(g, sizeof g); protocol_task();
        CHECK(g_feed == 100 + 20);
    }

    /* 49) A run of ESC bytes (the driver pads with them) must not swallow the
     *     command that follows: 100 and 156 ESCs, then ESC n 42. */
    {
        static const int runs[2] = { 100, 156 };
        for (int r = 0; r < 2; r++) {
            unsigned char buf[200];
            int n = 0;
            for (int i = 0; i < runs[r]; i++) buf[n++] = 0x1B;
            buf[n++] = 0x1B; buf[n++] = 'n'; buf[n++] = 42; buf[n++] = 0;
            reset_state();
            protocol_feed(buf, n); protocol_task();
            protocol_feed(q, sizeof q); protocol_task();
            CHECK(g_reply[5] == 42);
        }
    }

    /* 50) ESC y / ESC z are zero-argument: the next command must still parse. */
    {
        unsigned char yz[] = { 0x1B, 'y', 0x1B, 'z', 0x1B, 'n', 7, 0 };
        reset_state();
        protocol_feed(yz, sizeof yz); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 7);
    }

    /* 51) The framing byte counts the header: a genuine read request with no
     *     payload is ESC W 04 00 objLo objHi. The old parser skipped 4 more
     *     bytes and ate the next command. */
    reset_state();
    {
        unsigned char wr[] = { 0x1B, 'W', 4, 0, 0x01, 0x70,   /* HardwareConfig read */
                               0x1B, 'n', 13, 0 };
        protocol_feed(wr, sizeof wr); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 13 && g_reply[6] == 0);
    }

    /* 52) Firmware update attempt (DYMO Connect SecureFwUpdateCommand): ESC R
     *     00 01 00 F1, then a 128-byte signed header, then the host waits for
     *     ESC r <status>. We must consume exactly the header, refuse with a
     *     non-zero status, and parse normally afterwards. Reflash (ESC R 04 03
     *     00 00) is ignored. */
    reset_state();
    {
        unsigned char rf[] = { 0x1B, 'R', 4, 3, 0, 0 };
        protocol_feed(rf, sizeof rf); protocol_task();
        CHECK(g_reply_len == -1);                  /* no reboot, no reply */
        unsigned char up[6 + 128];
        up[0]=0x1B; up[1]='R'; up[2]=0; up[3]=1; up[4]=0x00; up[5]=0xF1;
        for (int i = 0; i < 128; i++) up[6 + i] = (i % 2) ? 0x1B : 'A'; /* hostile */
        protocol_feed(up, sizeof up); protocol_task();
        CHECK(g_reply_len == 3 && g_reply[0] == 0x1B && g_reply[1] == 'r' && g_reply[2] != 0);
        unsigned char i14[] = { 0x1B, 'n', 14, 0 };
        protocol_feed(i14, sizeof i14); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply_len == 32 && g_reply[5] == 14);
    }

    /* 53) GS D 0x09 reboots into USB DFU only with the exact confirmation
     *     'D' 'F' 'U', never mid-job, and answers before it goes. */
    reset_state();
    {
        unsigned char bad[] = { 0x1D, 'D', 0x09, 'D', 'F', 'X' };
        g_dfu_calls = 0;
        protocol_feed(bad, sizeof bad); protocol_task();
        CHECK(g_dfu_calls == 0 && g_reply_len == 3 && g_reply[1] == 0x09 && g_reply[2] == 0);

        unsigned char job[] = { 0x1B, 's', 1, 0, 0, 0 };
        unsigned char ok[]  = { 0x1D, 'D', 0x09, 'D', 'F', 'U' };
        protocol_feed(job, sizeof job); protocol_task();
        protocol_feed(ok, sizeof ok); protocol_task();
        CHECK(g_dfu_calls == 0 && g_reply[2] == 0);          /* refused mid-job */

        reset_state();
        g_dfu_calls = 0;
        g_vh_on = 1;
        if (setjmp(g_dfu_jmp) == 0) {
            protocol_feed(ok, sizeof ok); protocol_task();
        }
        CHECK(g_dfu_calls == 1);
        CHECK(g_reply_len == 3 && g_reply[1] == 0x09 && g_reply[2] == 1);
        CHECK(g_vh_on == 0);                                  /* heat rail dropped */
    }

    /* 54) ESC Q must clear the job id. DYMO's own language monitor only takes
     *     the print lock when the status reports an idle engine AND job id 0
     *     (LabelWriterLanguageMonitorV2.cpp, CheckLock), so a stale id meant
     *     the genuine driver could never print a second job. */
    reset_state();
    {
        unsigned char job[] = { 0x1B, 's', 0x44, 0x33, 0x22, 0x11 };
        unsigned char endj[] = { 0x1B, 'Q' };
        protocol_feed(job, sizeof job); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[0] == 1 && g_reply[1] == 0x44 && g_reply[4] == 0x11);
        protocol_feed(endj, sizeof endj); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[0] == 0 && g_reply[1] == 0 && g_reply[2] == 0 &&
              g_reply[3] == 0 && g_reply[4] == 0);
        /* and the same through the out-of-band reset (SOFT_RESET / ESC @) */
        protocol_feed(job, sizeof job); protocol_task();
        protocol_reset();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[1] == 0 && g_reply[4] == 0);
    }

    /* 55) ESC * / ESC $ must not disarm the 24 V interlock. store.h promises
     *     that while OP_FLAG_VH_INHIBIT is set no command sequence can heat the
     *     head; a factory reset is a command like any other. */
    reset_state();
    {
        unsigned char fr[] = { 0x1B, '*' };
        g_cfg.flags = OP_FLAG_VH_INHIBIT;
        protocol_feed(fr, sizeof fr); protocol_task();
        CHECK(g_cfg.flags == (OP_FLAG_PAPER_FORCE | OP_FLAG_VH_INHIBIT));
        /* everything else still resets, and the bit is never set by itself */
        g_cfg.flags = 0;
        protocol_feed(fr, sizeof fr); protocol_task();
        CHECK(g_cfg.flags == OP_FLAG_PAPER_FORCE);
    }

    /* 56) An interrupted firmware-update handshake must not leave the refusal
     *     armed: the next job's ESC M skip would end in the same state and fire
     *     a stray ESC r 01 into the host's reply stream. */
    reset_state();
    {
        unsigned char up[] = { 0x1B, 'R', 0x00, 0x01, 0x00, 0xF1 };
        unsigned char partial[10];
        memset(partial, 0xAA, sizeof partial);
        protocol_feed(up, sizeof up); protocol_task();
        protocol_feed(partial, sizeof partial); protocol_task();
        CHECK(g_reply_len == -1);                 /* header not complete yet */
        protocol_reset();
        g_reply_len = -1;
        unsigned char escm[10] = { 0x1B, 'M', 0, 0, 0, 0, 0, 0, 0, 0 };
        protocol_feed(escm, sizeof escm); protocol_task();
        CHECK(g_reply_len == -1);                 /* no spurious ESC r */
        unsigned char idx[] = { 0x1B, 'n', 31, 0 };
        protocol_feed(idx, sizeof idx); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 31);                  /* and the parser is in step */
    }

    /* 57) ESC Z (CompressedPrintData, from lw5xxmon.dll's opcode table): the
     *     17-byte header plus exactly the declared payload is consumed, nothing
     *     is printed or fed, and the next command still parses. */
    reset_state();
    {
        unsigned char z[2 + 15 + 40];
        int k = 0;
        z[k++] = 0x1B; z[k++] = 'Z';
        z[k++] = 0x03;                              /* scheme */
        z[k++] = 40; z[k++] = 0; z[k++] = 0; z[k++] = 0;   /* payload u32 LE */
        z[k++] = 1; z[k++] = 2;                     /* BPP, Align */
        z[k++] = 10; z[k++] = 0; z[k++] = 0; z[k++] = 0;   /* W = lines */
        z[k++] = (unsigned char)(HEAD_DOTS & 0xFF);
        z[k++] = (unsigned char)((HEAD_DOTS >> 8) & 0xFF);
        z[k++] = 0; z[k++] = 0;                     /* H = dots */
        for (int i = 0; i < 40; i++) z[k++] = (i & 1) ? 0x1B : 'A';  /* hostile body */
        protocol_feed(z, sizeof z); protocol_task();
        CHECK(g_lines == 0 && g_feed == 0);         /* consumed, nothing printed */
        CHECK(g_reply_len == -1);                   /* body never became commands */
        unsigned char idx[] = { 0x1B, 'n', 27, 0 };
        protocol_feed(idx, sizeof idx); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 27);
        /* a zero-length body returns to the command state directly */
        reset_state();
        unsigned char z0[2 + 15 + 4];
        memcpy(z0, z, 17);
        z0[3] = z0[4] = z0[5] = z0[6] = 0;
        z0[17] = 0x1B; z0[18] = 'n'; z0[19] = 5; z0[20] = 0;
        protocol_feed(z0, sizeof z0); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 5);
    }

    /* 58) Every fixed-length command in the genuine language consumes exactly
     *     its own argument bytes. The lengths come from the stock port
     *     monitor's own DPL iterator, which carries a byte length per opcode.
     *     Getting one wrong desynchronises the parser into reading raster data
     *     as commands: the old "unknown byte after ESC eats one argument" rule
     *     was wrong for ESC l, ESC t (4 bytes), ESC X (5) and ESC H, ESC m,
     *     ESC b (2).
     *
     *     The probe is built so BOTH directions fail loudly. The arguments end
     *     in 0x1B, so a command that eats too FEW leaves an ESC pending and the
     *     following GS D is swallowed as its opcode - no reply. A command that
     *     eats too MANY swallows the GS itself - also no reply. */
    {
        static const struct { unsigned char op; int args; } cmds[] = {
            { '#', 1 },   /* SetNumberOfCopies    */
            { 'p', 1 },   /* DoCutLabel           */
            { 'P', 0 },   /* GetEthernetPhyState  */
            { 'x', 0 },   /* GetPrintEngineParams */
            { 'H', 2 },   /* SetHorzResolution    */
            { 'm', 2 },   /* GetSensorsValues     */
            { 'b', 2 },   /* PrintEngineStatusTwin*/
            { 'l', 4 },   /* SetLabelLeader       */
            { 't', 4 },   /* SetLabelTrailer      */
            { 'X', 5 },   /* SetPrintEngineParams */
            { 'T', 1 },   /* SetFeedSpeed         */
            { 'q', 1 },   /* SelectRoll           */
            { 'C', 1 },   /* SetPrintDensity      */
            { 'A', 1 },   /* PrintEngineStatus    */
            { 'n', 2 },   /* SetLabelIndex        */
            { 's', 4 },   /* StartPrintJob        */
            { 'M', 8 },   /* SetMediaType         */
        };
        int bad = 0;
        for (unsigned k = 0; k < sizeof cmds / sizeof cmds[0]; k++) {
            unsigned char buf[24];
            int n = 0;
            reset_state();
            buf[n++] = 0x1B; buf[n++] = cmds[k].op;
            for (int i = 0; i < cmds[k].args; i++)
                buf[n++] = (unsigned char)((i == cmds[k].args - 1) ? 0x1B : 0x77);
            buf[n++] = 0x1D; buf[n++] = 'D'; buf[n++] = 0x05;   /* build id, replies */
            protocol_feed(buf, n); protocol_task();
            if (g_reply_len < 3 || g_reply[0] != 'D' || g_reply[1] != 0x05) {
                printf("     ESC %c (%d args): no build-id reply - out of step\n",
                       cmds[k].op, cmds[k].args);
                bad++;
            }
        }
        CHECK(bad == 0);
    }

    /* 59) The inter-label feed is measured along the FEED axis. ESC D's two
     *     32-bit fields are W then H, and W is the number of dot LINES while H
     *     is the width across the head - the opposite of what the names
     *     suggest. The genuine capture settles it: ESC D 01 02 | 9c 00 00 00 |
     *     10 01 00 00 is W=156, H=272, and the raster that follows is
     *     156 * (272/8) = 5304 bytes, matching the stream byte for byte.
     *
     *     emit_line() steps the motor once per printed line, so after a block
     *     the paper has advanced exactly `lines` dots; that is what must be
     *     subtracted from the label pitch. The property that pins the axis is
     *     the second case: the SAME line count at a different raster WIDTH must
     *     feed the same distance, because the image's width has no physical
     *     bearing on how far paper moves.
     *
     *     Storing the width instead made the advance shrink as the image got
     *     wider, and at the full head width it collapsed to the bare gap -
     *     1.7 mm instead of 46 mm, so every label after the first printed on
     *     top of the one before it. The whole rest of this suite passes either
     *     way, which is why this case exists. */
    {
        static unsigned char fj[32 + 100 * (HEAD_DOTS / 8)];
        /* 1111 dots as a RAW ESC L length: 0x0457 is in neither model's paper
         * table, so both take the raw-length path and the arithmetic below is
         * one number for both. (The table path is covered by cases 34-35.) */
        const int pitch = 1111;
        const int lines = 100;
        static const unsigned widths[2] = { HEAD_DOTS, 272 };
        int feed[2];
        for (int w = 0; w < 2; w++) {
            unsigned bpl = widths[w] / 8;
            int k = 0;
            reset_state();
            fj[k++] = 0x1B; fj[k++] = 'L';
            fj[k++] = (unsigned char)((pitch >> 8) & 0xFF);   /* ESC L is BE */
            fj[k++] = (unsigned char)(pitch & 0xFF);
            esc_d(&fj[k], (unsigned)lines, widths[w]); k += 12;
            for (unsigned i = 0; i < lines * bpl; i++) fj[k++] = 0xFF;
            fj[k++] = 0x1B; fj[k++] = 'G';
            /* 8.4 kB does not fit the input ring, so deliver it the way the USB
             * stack does: one 64-byte bulk packet at a time, draining between
             * packets. A single oversized protocol_feed() would silently drop
             * the tail and the raster would never finish. */
            for (int off = 0; off < k; off += 64) {
                int chunk = (k - off < 64) ? (k - off) : 64;
                protocol_feed(&fj[off], (unsigned)chunk);
                protocol_task();
            }
            feed[w] = g_feed;
            CHECK(g_lines == lines);
        }
        /* printed lines + the rest of the pitch + the die-cut gap */
        CHECK(feed[0] == lines + (pitch - lines) + 20);
        CHECK(feed[1] == feed[0]);              /* width must not move the paper */
    }

    /* 60) ESC Z's declared payload length is host data and must be bounded by
     *     the raster geometry in the same header. 0xFFFFFFFF once put the
     *     parser in S_SKIP for 4 GiB, with no escape in the byte stream at all:
     *     the device accepted everything and answered nothing, for this job and
     *     every job behind it, until a SOFT_RESET or a power cycle. */
    {
        static const unsigned long bogus[] = { 0xFFFFFFFFul, 0x10000000ul, 0x00100000ul };
        for (unsigned b = 0; b < sizeof bogus / sizeof bogus[0]; b++) {
            unsigned char z[2 + 15 + 4];
            int k = 0;
            reset_state();
            z[k++] = 0x1B; z[k++] = 'Z';
            z[k++] = 0x03;                                  /* scheme */
            z[k++] = (unsigned char)(bogus[b]        & 0xFF);
            z[k++] = (unsigned char)((bogus[b] >>  8) & 0xFF);
            z[k++] = (unsigned char)((bogus[b] >> 16) & 0xFF);
            z[k++] = (unsigned char)((bogus[b] >> 24) & 0xFF);
            z[k++] = 1; z[k++] = 2;                         /* BPP, Align */
            z[k++] = 10; z[k++] = 0; z[k++] = 0; z[k++] = 0; /* W = 10 lines */
            z[k++] = (unsigned char)(HEAD_DOTS & 0xFF);      /* H = head width */
            z[k++] = (unsigned char)((HEAD_DOTS >> 8) & 0xFF);
            z[k++] = 0; z[k++] = 0;
            /* A body of 10 * HEAD_BYTES could never compress to any of these,
             * so the length is rejected and the parser stays live. */
            z[k++] = 0x1B; z[k++] = 'n'; z[k++] = 29; z[k++] = 0;
            protocol_feed(z, (unsigned)k); protocol_task();
            protocol_feed(q, sizeof q); protocol_task();
            CHECK(g_reply[5] == 29);
        }
        /* An honest length still skips exactly its own body (case 57 covers the
         * in-range path; this asserts the bound did not break it). */
        reset_state();
        unsigned char zo[2 + 15 + 40 + 4];
        int k = 0;
        zo[k++] = 0x1B; zo[k++] = 'Z'; zo[k++] = 0x03;
        zo[k++] = 40; zo[k++] = 0; zo[k++] = 0; zo[k++] = 0;
        zo[k++] = 1; zo[k++] = 2;
        zo[k++] = 10; zo[k++] = 0; zo[k++] = 0; zo[k++] = 0;
        zo[k++] = (unsigned char)(HEAD_DOTS & 0xFF);
        zo[k++] = (unsigned char)((HEAD_DOTS >> 8) & 0xFF);
        zo[k++] = 0; zo[k++] = 0;
        for (int i = 0; i < 40; i++) zo[k++] = (i & 1) ? 0x1B : 'A';
        zo[k++] = 0x1B; zo[k++] = 'n'; zo[k++] = 30; zo[k++] = 0;
        protocol_feed(zo, (unsigned)k); protocol_task();
        protocol_feed(q, sizeof q); protocol_task();
        CHECK(g_reply[5] == 30);
    }

    /* 61) The cool-down wait in emit_line(): over the limit, then cool. The
     *     firmware must WAIT and then print - dropping the line would leave a
     *     white band in the middle of a label with no error anywhere. */
    {
        reset_state();
        g_hot_polls = 3;
        unsigned char j[64]; int n = 0;
        j[n++] = 0x1B; j[n++] = 's'; j[n++] = 1; j[n++] = 0; j[n++] = 0; j[n++] = 0;
        esc_d(&j[n], 3, 16); n += 12;
        for (int i = 0; i < 6; i++) j[n++] = 0xFF;
        protocol_feed(j, (unsigned)n); protocol_task();
        CHECK(g_lines == 3);            /* waited, then printed all three */
        CHECK(g_delays == 3);           /* three 10 ms polls, only on line 1 */
    }

    /* 62) The same gate, over the limit FOREVER, at both call sites - and the
     *     two make opposite decisions on purpose:
     *
     *     emit_line() prints anyway after the bounded ~1 s wait. By then the
     *     dwell has already been scaled down by thermal_dwell_scale(), so the
     *     energy is reduced rather than nominal, and a host job that stalls
     *     forever is worse than a slightly light label.
     *
     *     protocol_self_test() REFUSES. Nobody is waiting on its output, it is
     *     the operator's own bring-up tool, and DECISIONS D7 is explicit that a
     *     bring-up self-test must never be the thing that cooks the head.
     *
     *     Writing these two the same way round would be the natural mistake,
     *     so both are pinned here. */
    {
        reset_state();
        g_hot_polls = -1;
        unsigned char j[64]; int n = 0;
        j[n++] = 0x1B; j[n++] = 's'; j[n++] = 1; j[n++] = 0; j[n++] = 0; j[n++] = 0;
        esc_d(&j[n], 3, 16); n += 12;
        for (int i = 0; i < 6; i++) j[n++] = 0xFF;
        protocol_feed(j, (unsigned)n); protocol_task();
        CHECK(g_lines == 3);            /* prints anyway, at a reduced dwell */
        CHECK(g_delays == 300);         /* 100 bounded polls per line */

        reset_state();
        g_hot_polls = -1;
        protocol_self_test();
        CHECK(g_lines == 0);            /* refuses: not one dot */
        CHECK(g_delays == 100);         /* one bounded wait, then it gives up */
    }

    /* 63) GS with an unknown sub-command consumes exactly that one byte and
     *     resyncs. PROTOCOL.md defines only GS C (1D 43) and GS D (1D 44);
     *     every other 1D xx lands on the resync, which no test had ever
     *     executed. The sweep is exhaustive because it is free. */
    {
        int bad = 0;
        for (int x = 0; x < 256; x++) {
            if (x == 'C' || x == 'D') continue;
            reset_state();
            unsigned char b[] = { 0x1D, (unsigned char)x, 0x1B, 'A', 0x00 };
            protocol_feed(b, sizeof b); protocol_task();
            if (g_reply_len != 32 || g_lines != 0 || g_feed != 0) bad++;
        }
        CHECK(bad == 0);
        /* The corollary: the resync eats the byte itself, so an ESC that lands
         * in that position is consumed and the command behind it is not. */
        reset_state();
        unsigned char b2[] = { 0x1D, 0x1B, 'A', 0x00 };
        protocol_feed(b2, sizeof b2); protocol_task();
        CHECK(g_reply_len == -1);
    }

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
