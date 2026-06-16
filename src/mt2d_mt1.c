/* Research daemon: MT2 -> decode -> MT1 report -> fake-MT1 IOHIDUserDevice.
 * Binds AppleMultitouchHIDEventDriver but does not yet yield a MultitouchDevice
 * (gesture engine). Kept for continued research into the full-gesture path. */
#include "mt2_usb_read.h"
#include "mt2_usb_decode.h"
#include "mt1_encode.h"
#include "vhid_mt1.h"
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <signal.h>

static uint32_t elapsed_ms(void) {
    static mach_timebase_info_data_t tb;
    static uint64_t t0;
    if (tb.denom == 0) { mach_timebase_info(&tb); t0 = mach_absolute_time(); }
    uint64_t ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
    return (uint32_t)(ns / 1000000ULL);
}

static vhid_t *g_vhid;

static void on_frame(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
    touch_frame_t tf = {0};
    if (mt2_usb_decode(frame, len, &tf) != 0) return;
    uint8_t out[256];
    int n = mt1_encode(&tf, out, sizeof(out), elapsed_ms());
    if (n > 0) vhid_send(g_vhid, out, (size_t)n);
}

static void on_sig(int s) { (void)s; mt2_usb_read_stop(); if (g_vhid) vhid_destroy(g_vhid); _exit(0); }

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    g_vhid = vhid_create();
    if (!g_vhid) { fprintf(stderr, "vhid_create failed (run as root)\n"); return 1; }
    if (mt2_usb_read_start(on_frame, NULL) != 0) {
        fprintf(stderr, "mt2_usb_read_start failed (root; MT2USBClaim kext loaded?)\n");
        vhid_destroy(g_vhid); return 1;
    }
    printf("mt2d_mt1 (research): MT2 -> MT1 -> fake-MT1 device. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
