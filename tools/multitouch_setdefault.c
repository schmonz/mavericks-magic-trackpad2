/* mt_setdefault - make our multitouch device the system's CONFIGURED DEFAULT trackpad,
 * to test the hypothesis that hidd only ACTUATES (cursor/gestures) the configured device.
 * Uses MultitouchSupport's own mt_CreateSavedNameForDevice + the com.apple.MultitouchSupport
 * DefaultDevice / DisabledDevices preferences (reverse-engineered from MTDeviceCreateDefault).
 * Run as root with our device present. Then restart hidd and re-test actuation. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *MTDeviceRef;
extern CFArrayRef  MTDeviceCreateList(void);
extern CFStringRef mt_CreateSavedNameForDevice(MTDeviceRef);
extern int MTDeviceGetDeviceID(MTDeviceRef, uint64_t *);

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("MTDeviceCreateList: %ld device(s)\n", (long)n);
    if (n == 0) return 1;

    /* there is only our device; use index 0 */
    MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, 0);
    uint64_t id = 0; MTDeviceGetDeviceID(d, &id);
    CFStringRef name = mt_CreateSavedNameForDevice(d);
    if (!name) { fprintf(stderr, "mt_CreateSavedNameForDevice returned NULL\n"); return 1; }
    char buf[256] = {0};
    CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    printf("device 0x%llx saved name = \"%s\"\n", id, buf);

    CFStringRef domain = CFSTR("com.apple.MultitouchSupport");
    /* DefaultDevice = our saved name */
    CFPreferencesSetAppValue(CFSTR("DefaultDevice"), name, domain);
    /* DisabledDevices = empty array (make sure we are not disabled) */
    CFArrayRef empty = CFArrayCreate(0, NULL, 0, &kCFTypeArrayCallBacks);
    CFPreferencesSetAppValue(CFSTR("DisabledDevices"), empty, domain);
    CFRelease(empty);
    Boolean ok = CFPreferencesAppSynchronize(domain);
    printf("set DefaultDevice + cleared DisabledDevices in %s -> synchronize=%d\n",
           "com.apple.MultitouchSupport", (int)ok);
    CFRelease(name);
    return 0;
}
