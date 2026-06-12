#include "../src/vhid_mt1.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

int main(void) {
    vhid_t *v = vhid_create();
    if (!v) { fprintf(stderr, "vhid_create failed (run as root?)\n"); return 1; }
    printf("Virtual Magic Trackpad created. Inspect:\n");
    printf("  ioreg -r -c IOHIDUserDevice -w0 | grep -iE 'IOHIDInterface|AppleMultitouch'\n");
    printf("Ctrl-C to remove.\n");
    fflush(stdout);
    CFRunLoopRun();
    vhid_destroy(v);
    return 0;
}
