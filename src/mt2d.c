#include "mt2_reader.h"
#include "mt2_decode.h"
#include "mt1_encode.h"
#include "vhid_mt1.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <signal.h>

static vhid_t *g_vhid;

/* Called on the reader's background thread for each raw MT2 frame. */
static void on_frame(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
    touch_frame_t tf = {0};
    if (mt2_decode(frame, len, &tf) != 0) return;
    uint8_t out[256];
    int n = mt1_encode(&tf, out, sizeof(out));
    if (n > 0) vhid_send(g_vhid, out, (size_t)n);
}

static void on_sig(int s) { (void)s; mt2_reader_stop(); if (g_vhid) vhid_destroy(g_vhid); _exit(0); }

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    g_vhid = vhid_create();
    if (!g_vhid) { fprintf(stderr, "vhid_create failed (run as root)\n"); return 1; }
    if (mt2_reader_start(on_frame, NULL) != 0) {
        fprintf(stderr, "mt2_reader_start failed (run as root; MT2Claim kext loaded?)\n");
        vhid_destroy(g_vhid);
        return 1;
    }
    printf("mt2d running: MT2 -> MT1 -> virtual trackpad. Touch the pad. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
