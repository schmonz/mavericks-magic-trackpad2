/* synth_feed - inject SYNTHETIC moving contacts into our MT2Gesture device, with no
 * real trackpad. One finger sweeps horizontally (x low->high) at the surface midline,
 * ~60 Hz, with monotonically increasing timestamps. Lets us test the actuation gate
 * (does hidd turn clean moving contacts into cursor motion?) decoupled from the flaky
 * USB feeder and from any MT2->MT1 coordinate-mapping doubts.
 *
 * Run as root with /tmp/MT2Gesture.kext loaded. Watch the cursor (and mt_contacts).
 */
#include "../src/touch_model.h"
#include "../src/mavericks_amd_terminal_encode.h"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static io_connect_t g_conn = IO_OBJECT_NULL;
static volatile int g_run = 1;

static uint32_t elapsed_ms(void) {
    /* ABSOLUTE uptime ms (monotonic, large) -- a clock starting at 0 makes the
     * recognizer reject frames as "timestamp invalid". Matches the kext's uptime_ms. */
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t ns = mach_absolute_time() * (uint64_t)tb.numer / (uint64_t)tb.denom;
    return (uint32_t)(ns / 1000000ULL);
}
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv) {
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    /* fingers: 1 = one-finger cursor sweep (x), 2 = two-finger vertical SCROLL (y),
     * 3 = three-finger horizontal SWIPE (x). MT2 range x[-3678..3934] y[-2478..2587]. */
    int fingers = (argc > 1) ? atoi(argv[1]) : 1;
    if (fingers < 1) fingers = 1;
    if (fingers > 3) fingers = 3;
    /* argv[2] = per-frame step in MT2 units (default 200). 0 = hold stationary at center. */
    int step = (argc > 2) ? atoi(argv[2]) : 200;
    const int LO = -2500, HI = 2500;

    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("com_schmonz_MT2Gesture"));
    if (!svc) { fprintf(stderr, "MT2Gesture service not found (kext loaded?)\n"); return 1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "IOServiceOpen 0x%x\n", kr); return 1; }

    printf("synth_feed: %d finger(s) %s sweep at ~60Hz. Ctrl-C to stop.\n",
           fingers, fingers == 2 ? "VERTICAL(scroll)" : "HORIZONTAL");
    fflush(stdout);

    int sweep = 0, dir = 1;            /* the moving axis value */
    int offs[3] = { 0, -900, 900 };    /* perpendicular spread for finger 2/3 */
    int third[3] = { 0, 0, 0 };
    unsigned long frames = 0; kern_return_t last = 0x7fffffff;
    while (g_run) {
        touch_frame_t f; f.ntouches = fingers; f.button = 0; f.timestamp = 0;
        for (int i = 0; i < fingers; i++) {
            touch_t *t = &f.touches[i];
            t->id = 7 + i; t->state = TS_TOUCHING;
            if (fingers == 2) {        /* vertical scroll: x fixed/spread, y = sweep */
                t->x = offs[i]; t->y = sweep;
            } else {                   /* 1 or 3 finger: x = sweep, y fixed/spread */
                t->x = sweep; t->y = (fingers == 3) ? (i - 1) * 900 : 0;
                (void)third;
            }
            t->touch_major = 600; t->touch_minor = 500; t->orientation = 0; t->size = 40;
        }
        uint8_t out[256];
        uint32_t ts = getenv("SYNTH_FIXED_TS") ? (uint32_t)atoi(getenv("SYNTH_FIXED_TS")) : elapsed_ms();
        int n = mavericks_amd_construct_report(&f, out, sizeof(out), ts);
        if (n > 0) {
            kr = IOConnectCallStructMethod(g_conn, 0, out, (size_t)n, NULL, NULL);
            if (kr != last) { fprintf(stderr, "synth: kr=0x%x (frame %lu)\n", kr, frames); last = kr; }
        }
        frames++;
        sweep += dir * step;
        if (sweep >= HI) { sweep = HI; dir = -1; }
        if (sweep <= LO) { sweep = LO; dir = 1; }
        usleep(16000);   /* ~60 Hz */
    }
    /* lift */
    touch_frame_t f = {0}; f.ntouches = 0;
    uint8_t out[256]; int n = mavericks_amd_construct_report(&f, out, sizeof(out), elapsed_ms());
    if (n > 0) IOConnectCallStructMethod(g_conn, 0, out, (size_t)n, NULL, NULL);
    IOServiceClose(g_conn);
    return 0;
}
