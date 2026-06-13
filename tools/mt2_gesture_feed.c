/* mt2_gesture_feed - Milestone 4 userspace feeder.
 *
 * Reads raw MT2 multitouch frames (src/mt2_reader: needs the MT2Claim kext loaded
 * + the device re-enumerated so interface 1 is free), decodes them to the neutral
 * touch model, re-encodes each as a Magic Trackpad 1 (0x28) input report, and
 * pushes the report into the MT2Gesture kext's user client via selector 0
 * (-> com_schmonz_MT2Gesture::feedFrame -> AppleMultitouchDevice::handleTouchFrame).
 * From there the real AppleMultitouchDevice enqueues the bytes to MultitouchSupport,
 * which drives Apple's gesture engine.
 *
 * Run as root, with both kexts loaded:
 *   sudo kextload /usr/local/lib/mt2d/MT2Claim.kext   (frees interface 1)
 *   sudo /usr/local/sbin/mt2_reenumerate              (so MT2Claim attaches)
 *   sudo kextload /tmp/MT2Gesture.kext                (the gesture transport)
 *   sudo ./mt2_gesture_feed
 *
 * The feed return code is surfaced on every transition: kr=0x0 means the frame was
 * enqueued to a connected MultitouchSupport client; kr=0xe00002bc means the device
 * has no client yet / is not ready (benign - just means nothing is listening).
 */
#include "../src/mt2_reader.h"
#include "../src/mt2_decode.h"
#include "../src/mt1_encode.h"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <signal.h>

/* Monotonic milliseconds since the first call - the MT1 frame timestamp the
 * gesture engine needs (must increase frame-to-frame). */
static uint32_t elapsed_ms(void) {
    static mach_timebase_info_data_t tb;
    static uint64_t t0;
    if (tb.denom == 0) { mach_timebase_info(&tb); t0 = mach_absolute_time(); }
    uint64_t ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
    return (uint32_t)(ns / 1000000ULL);
}

static io_connect_t g_conn = IO_OBJECT_NULL;

static void on_frame(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
    touch_frame_t tf = {0};
    if (mt2_decode(frame, len, &tf) != 0) return;

    uint8_t out[256];
    int n = mt1_encode(&tf, out, sizeof(out), elapsed_ms());
    if (n <= 0) return;

    kern_return_t kr = IOConnectCallStructMethod(g_conn, 0, out, (size_t)n, NULL, NULL);

    /* Report only on change, so a working stream is quiet but we still see the
     * moment MultitouchSupport connects (0xe00002bc -> 0x0) or drops out. */
    static kern_return_t last_kr = 0x7fffffff;
    static unsigned long frames = 0;
    frames++;
    if (kr != last_kr) {
        fprintf(stderr, "feed: kr=0x%x (after %lu frames)\n", kr, frames);
        last_kr = kr;
    }
}

static void on_sig(int s) {
    (void)s;
    mt2_reader_stop();
    if (g_conn) IOServiceClose(g_conn);
    _exit(0);
}

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    io_service_t svc = IOServiceGetMatchingService(
        kIOMasterPortDefault, IOServiceMatching("com_schmonz_MT2Gesture"));
    if (!svc) {
        fprintf(stderr, "mt2_gesture_feed: MT2Gesture service not found "
                        "(is /tmp/MT2Gesture.kext loaded?)\n");
        return 1;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mt2_gesture_feed: IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    if (mt2_reader_start(on_frame, NULL) != 0) {
        fprintf(stderr, "mt2_gesture_feed: mt2_reader_start failed "
                        "(root? MT2Claim loaded + device re-enumerated?)\n");
        IOServiceClose(g_conn);
        return 1;
    }

    printf("mt2_gesture_feed: MT2 -> MT1 report -> MT2Gesture user client. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
