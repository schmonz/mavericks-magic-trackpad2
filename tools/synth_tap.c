/* synth_tap - drive our REAL pipeline (mt2_session -> lifecycle -> mt1_encode) with a
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
#include "../src/mt2_session.h"
#include "../src/mt1_encode.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
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
    int n = mt1_encode(f, out, sizeof(out), elapsed_ms());
    if (n > 0) {
        kern_return_t kr = IOConnectCallStructMethod(g_conn, 0, out, (size_t)n, NULL, NULL);
        int st = (f->ntouches > 0) ? f->touches[0].state : -1;
        printf("  feed ntouches=%d state=%d sz=%d -> inject kr=0x%x (n=%d)\n",
               f->ntouches, st, f->ntouches > 0 ? f->touches[0].size : 0, kr, n);
        fflush(stdout);
    }
}
static void sink_click(void *ctx, unsigned mask) { (void)ctx; printf("  post_click mask=0x%x\n", mask); fflush(stdout); }
static void sink_arm(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }

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

    mt2_session_t s; memset(&s, 0, sizeof s);
    mt2_session_sink_t sink; sink.post_click = sink_click; sink.feed_frame = sink_feed;
    sink.arm_timer = sink_arm; sink.ctx = 0;

    uint32_t now = elapsed_ms();
    mt2_session_connect(&s, (uintptr_t)SRC, MT2_EVENT_DRIVEN, now);
    printf("synth_tap: %d down-frames @ %dms at MT2(%d,%d)\n", downframes, frame_ms, x, y);

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
        mt2_session_frame(&s, (uintptr_t)SRC, &f, now, &sink);
        usleep((useconds_t)frame_ms * 1000);
    }
    /* lift: empty frame -> session synthesizes the BreakTouch */
    touch_frame_t lift; memset(&lift, 0, sizeof lift); lift.ntouches = 0;
    now = elapsed_ms();
    mt2_session_frame(&s, (uintptr_t)SRC, &lift, now, &sink);

    IOServiceClose(g_conn);
    printf("synth_tap: done\n");
    return 0;
}
