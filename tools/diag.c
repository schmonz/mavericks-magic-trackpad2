/* throwaway diagnostic — enumerate all MT2 HID interfaces, show open/enable
 * results, seize the multitouch interface, and print any input reports. */
#include <stdio.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>

#define VID 0x05ac
#define PID 0x0265

static uint8_t bufs[8][512];
static IOHIDDeviceRef devs[8];
static int n;

static void on_report(void *ctx, IOReturn r, void *s, IOHIDReportType t,
                      uint32_t rid, uint8_t *rep, CFIndex len) {
    (void)r;(void)s;(void)t;
    if (len <= 1) return;
    int idx = (int)(intptr_t)ctx;
    printf("[if%d] rid=%u len=%ld: ", idx, rid, len);
    for (CFIndex i = 0; i < len && i < 40; i++) printf("%02x ", rep[i]);
    printf("\n"); fflush(stdout);
}

static int getprop(IOHIDDeviceRef d, CFStringRef k) {
    CFNumberRef num = IOHIDDeviceGetProperty(d, k);
    int v = -1; if (num) CFNumberGetValue(num, kCFNumberIntType, &v); return v;
}

int main(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, 0);
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int vid=VID, pid=PID;
    CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey), CFNumberCreate(0,kCFNumberIntType,&vid));
    CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), CFNumberCreate(0,kCFNumberIntType,&pid));
    IOHIDManagerSetDeviceMatching(mgr, m);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(mgr, 0);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);

    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    n = set ? (int)CFSetGetCount(set) : 0;
    if (n > 8) n = 8;
    if (n) CFSetGetValues(set, (const void **)devs);
    printf("Found %d MT2 interfaces\n", n);

    for (int i = 0; i < n; i++) {
        int up = getprop(devs[i], CFSTR(kIOHIDPrimaryUsagePageKey));
        int u  = getprop(devs[i], CFSTR(kIOHIDPrimaryUsageKey));
        IOReturn seize = IOHIDDeviceOpen(devs[i], kIOHIDOptionsTypeSeizeDevice);
        printf("if%d UsagePage=0x%04x Usage=0x%02x  seizeOpen=0x%08x\n", i, up, u, seize);
        IOHIDDeviceRegisterInputReportCallback(devs[i], bufs[i], sizeof(bufs[i]),
                                               on_report, (void*)(intptr_t)i);
        /* try enabling multitouch on every interface */
        uint8_t en[] = {0x02, 0x01};
        IOReturn sr = IOHIDDeviceSetReport(devs[i], kIOHIDReportTypeFeature, 0x02, en, 2);
        printf("    SetReport(0x02,{02,01}) = 0x%08x\n", sr);
    }
    printf("\nNow touch the trackpad...\n"); fflush(stdout);
    CFRunLoopRun();
    return 0;
}
