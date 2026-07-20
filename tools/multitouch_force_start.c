/* mt_force_start - force-open + start every MT device (mirrors what WindowServer's MultitouchSupport
 * does on hotplug). RE probe: if starting our USB AMD from userspace makes the cursor come alive, the
 * device is fully capable and the ONLY missing piece is that WindowServer never opened it (the built-in
 * gate is in the OPEN/hotplug path, not the device). Keeps devices started until Ctrl-C. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *MTDeviceRef;
extern CFArrayRef MTDeviceCreateList(void);
extern int  MTDeviceGetDriverType(MTDeviceRef, int *);
extern void MTRegisterContactFrameCallback(MTDeviceRef, void *);
extern void MTDeviceStart(MTDeviceRef, int);

static int g_frames = 0;
/* Permissive callback signature (we only count; the kernel path drives the cursor, not us). */
static int frame_cb(MTDeviceRef d, void *frame, int nContacts, double ts, int frameNum) {
    (void)d; (void)frame; (void)ts; (void)frameNum;
    if (g_frames++ % 60 == 0)
        printf("  [frame #%d] %d contact(s) streaming to userspace\n", g_frames, nContacts);
    return 0;
}

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("force-starting %ld MT device(s)...\n", (long)n);
    for (CFIndex i = 0; i < n; i++) {
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
        int dt = -1; MTDeviceGetDriverType(d, &dt);
        MTRegisterContactFrameCallback(d, (void *)frame_cb);
        MTDeviceStart(d, 0);
        printf("  started device %ld (driverType=%d)\n", (long)i, dt);
    }
    printf("STARTED. Move a finger on USB now — watch for the cursor + streaming frames. Ctrl-C to stop.\n");
    CFRunLoopRun();
    return 0;
}
