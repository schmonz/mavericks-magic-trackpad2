/* synth_tap - drive our REAL pipeline (mavericks_session -> lifecycle -> mavericks_amd_construct_report) with a
 * synthetic single-finger TAP and inject each encoded frame into the kext, so we can
 * test tap-to-click with no physical finger. A tap = one stationary contact present for
 * a few frames (well under the recognizer's 250ms duration gate), then a lift frame that
 * makes the session synthesize the BreakTouch.
 *
 * This reproduces exactly what the kext emits to AppleMultitouchDevice for a tap, since
 * selector 0 forwards encoded 0x28 bytes to handleTouchFrame (same as the kext's sink).
 *
 *   synth_tap [downframes] [frame_ms] [x] [y]
 *     downframes : contact-present frames (default 5)
 *     frame_ms   : spacing between frames in ms (default 12 -> ~5 frames = 60ms tap)
 *     x,y        : MT2-space position (default 0,0 = center)
 *
 * Run as root with the kext loaded. Pair with click_monitor (does a click commit?) and/or
 * mt_contacts (what does the recognizer receive?). */
#include "../src/mavericks_session.h"
#include "../src/mavericks_amd_terminal_encode.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SRC 0x7A9

static io_connect_t g_conn = IO_OBJECT_NULL;

/* ABSOLUTE uptime in ms (like the kext's uptime_ms): large + monotonic. A clock that
 * starts at 0 makes the recognizer reject the first frame ("timestamp invalid"). */
static uint32_t elapsed_ms(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t ns = mach_absolute_time() * (uint64_t)tb.numer / (uint64_t)tb.denom;
    return (uint32_t)(ns / 1000000ULL);
}

static void sink_feed(void *ctx, const touch_frame_t *f) {
    (void)ctx;
    uint8_t out[256];
    /* DIAGNOSTIC: SYNTH_CONST_TS=<ms> makes every frame carry the SAME encoded device
     * timestamp, decoupling the recognizer's clock from real injection time. If the
     * recognizer's measured tap duration collapses to ~0, it derives time from OUR
     * device timestamp (fix in mavericks_amd_construct_report); if it stays large, it uses wall-clock. */
    const char *cts = getenv("SYNTH_CONST_TS");
    const char *tdiv = getenv("SYNTH_TS_DIV");   /* calibrate the ts scale: ts = elapsed_ms / DIV */
    uint32_t base = cts ? (uint32_t)atoi(cts) : elapsed_ms();
    uint32_t ts = tdiv ? base / (uint32_t)atoi(tdiv) : base;
    int n = mavericks_amd_construct_report(f, out, sizeof(out), ts);
    if (n > 0) {
        kern_return_t kr = IOConnectCallStructMethod(g_conn, 0, out, (size_t)n, NULL, NULL);
        int st = (f->ntouches > 0) ? f->touches[0].state : -1;
        printf("  feed ntouches=%d state=%d sz=%d -> inject kr=0x%x (n=%d)\n",
               f->ntouches, st, f->ntouches > 0 ? f->touches[0].size : 0, kr, n);
        fflush(stdout);
    }
}
static void sink_click(void *ctx, unsigned mask) { (void)ctx; printf("  post_button_edge mask=0x%x\n", mask); fflush(stdout); }
static void sink_arm(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }

/* Inject ONE raw zero-contact frame (a "pump" frame). The recognizer flushes a tap's
 * queued button-click only when a LATER chord frame arrives; during the inter-tap gap
 * synth_tap feeds NO frames, so each tap's click waits for the NEXT tap to flush it (and
 * the last tap drops entirely -> the ~7/16 clean-count cap, ORACLES.md flush artifact).
 * Feeding K absence pump-frames after each lift gives the recognizer those flush cycles
 * within the tap's OWN window, so every tap (including the last) surfaces its click. */
static void inject_pump(uint32_t ts) {
    touch_frame_t e; memset(&e, 0, sizeof e); e.ntouches = 0;
    uint8_t out[256];
    int n = mavericks_amd_construct_report(&e, out, sizeof(out), ts);
    if (n > 0) IOConnectCallStructMethod(g_conn, 0, out, (size_t)n, NULL, NULL);
}

int main(int argc, char **argv) {
    int downframes = (argc > 1) ? atoi(argv[1]) : 5;
    int frame_ms   = (argc > 2) ? atoi(argv[2]) : 12;
    int x          = (argc > 3) ? atoi(argv[3]) : 0;
    int y          = (argc > 4) ? atoi(argv[4]) : 0;

    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("com_schmonz_MT2Gesture"));
    if (!svc) { fprintf(stderr, "MT2Gesture service not found (kext loaded?)\n"); return 1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "IOServiceOpen 0x%x\n", kr); return 1; }

    mavericks_session_t s; memset(&s, 0, sizeof s);
    mavericks_session_sink_t sink; sink.post_button_edge = sink_click; sink.feed_frame = sink_feed;
    sink.arm_timer = sink_arm; sink.ctx = 0;

    uint32_t now = elapsed_ms();
    mavericks_session_connect(&s, (uintptr_t)SRC, MT2_EVENT_DRIVEN, &mt2_policy_bt, now);
    printf("synth_tap: %d down-frames @ %dms at MT2(%d,%d)\n", downframes, frame_ms, x, y);

    /* SYNTH_TAPS=N: repeat N taps in ONE open connection (default 1). The recognizer queues a
     * tap-click but only FLUSHES it on a later chord frame; a single open/inject/close never
     * pumps it again, so the queued click never reaches CGEvents. Streaming several taps (and
     * holding the connection open after) gives the recognizer cycles to dispatch the queue. */
    const char *nt = getenv("SYNTH_TAPS");
    int ntaps = nt ? atoi(nt) : 1;
    const char *g = getenv("SYNTH_GAP_MS");
    int gap_ms = g ? atoi(g) : 200;   /* between taps in one connection */
    const char *pp = getenv("SYNTH_PUMP");
    int pump = pp ? atoi(pp) : 0;     /* trailing absence pump-frames per tap (flush each tap's click) */
    const char *pms = getenv("SYNTH_PUMP_MS");
    int pump_ms = pms ? atoi(pms) : frame_ms;  /* spacing before/between pump frames (0 = back-to-back) */
    for (int tap = 0; tap < ntaps; tap++) {
        for (int i = 0; i < downframes; i++) {
            touch_frame_t f; memset(&f, 0, sizeof f);
            f.ntouches = 1;
            f.touches[0].id = 1; f.touches[0].state = TS_TOUCHING;
            /* tiny per-frame jitter: a real finger is never mathematically static; identical
             * frames may not advance the recognizer's path/tap state machine. */
            f.touches[0].x = x + i * 6; f.touches[0].y = y + (i & 1 ? 4 : -4);
            f.touches[0].touch_major = 600; f.touches[0].touch_minor = 500;
            f.touches[0].size = 40; f.touches[0].orientation = 0;
            now = elapsed_ms();
            mavericks_session_frame(&s, (uintptr_t)SRC, &f, now, &sink);
            usleep((useconds_t)frame_ms * 1000);
        }
        /* lift: empty frame -> session synthesizes the BreakTouch AND a trailing zero-contact
         * frame so the recognizer finalizes the path lift. */
        touch_frame_t lift; memset(&lift, 0, sizeof lift); lift.ntouches = 0;
        now = elapsed_ms();
        mavericks_session_frame(&s, (uintptr_t)SRC, &lift, now, &sink);
        /* pump frames: flush THIS tap's queued click now, instead of waiting for the next tap. */
        for (int p = 0; p < pump; p++) {
            if (pump_ms > 0) usleep((useconds_t)pump_ms * 1000);
            inject_pump(elapsed_ms());
        }
        usleep((useconds_t)gap_ms * 1000);
    }
    /* hold the connection open so any deferred queue-flush / event delivery can land. */
    usleep(1500 * 1000);
    IOServiceClose(g_conn);
    printf("synth_tap: done (%d taps)\n", ntaps);
    return 0;
}
