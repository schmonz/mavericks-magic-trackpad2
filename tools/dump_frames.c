#include "../src/mt2_usb_read.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <signal.h>

static void cb(const uint8_t *r, size_t len, void *ctx) {
    (void)ctx;
    printf("len=%zu: ", len);
    for (size_t i = 0; i < len; i++) printf("%02x ", r[i]);
    printf("\n");
    fflush(stdout);
}

static void on_sig(int s) { (void)s; mt2_usb_read_stop(); _exit(0); }

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    if (mt2_usb_read_start(cb, NULL) != 0) return 1;
    printf("Reading raw MT2 frames. Touch the trackpad. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
