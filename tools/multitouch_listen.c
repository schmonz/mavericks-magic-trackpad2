/* mt_listen - does our AppleMultitouchDevice deliver touches to a MultitouchSupport
 * client? Enumerates MT devices, registers a contact callback, and MTDeviceStart()s
 * each. If our fake-mode device works, MTDeviceStart triggers our kext's enable
 * handler (ENABLE-MT in dmesg) and the callback prints nFingers as frames are fed.
 *
 * Build: clang -O2 -o /tmp/mt_listen tools/mt_listen.c \
 *   -F/System/Library/PrivateFrameworks -framework CoreFoundation -framework MultitouchSupport
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *MTDeviceRef;
extern CFArrayRef MTDeviceCreateList(void);
typedef int (*MTContactCallbackFunction)(int, void *, int, double, int);
extern void MTRegisterContactFrameCallback(MTDeviceRef, MTContactCallbackFunction);
extern void MTDeviceStart(MTDeviceRef, int);
extern void MTDeviceStop(MTDeviceRef);

static int contact_cb(int device, void *touches, int nFingers, double ts, int frame) {
    (void)touches;
    static unsigned long n = 0;
    if (++n <= 5 || nFingers > 0) {
        printf("CONTACT dev=%d fingers=%d ts=%.3f frame=%d\n", device, nFingers, ts, frame);
        fflush(stdout);
    }
    return 0;
}

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("mt_listen: %ld device(s); starting each.\n", (long)n);
    for (CFIndex i = 0; i < n; i++) {
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
        MTRegisterContactFrameCallback(d, contact_cb);
        MTDeviceStart(d, 0);
        printf("started device %ld\n", (long)i);
    }
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
