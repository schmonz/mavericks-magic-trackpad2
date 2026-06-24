/* mt2_usb_enable - send the Magic Trackpad 2's USB multitouch-enable Feature report
 * to the device while Apple's genuine AppleUSBMultitouchDriver owns it.
 *
 * Spike (2026-06-24, genuine-usb-prefpane): the genuine USB driver starts + the event
 * driver binds, but the pad never streams because Apple's driver (built for product IDs
 * <=612) doesn't send MT2's enable. This reproduces the exact request our MT2USBReader
 * issues: SET_REPORT(Feature), report id 0x02, payload {0x02,0x01} (wValue 0x0302). If
 * the pad starts streaming afterwards -> cursor lights -> genuine-USB confirmed viable.
 *
 * Build:  make mt2_usb_enable      Run: ./mt2_usb_enable [repeat]
 */
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

#define MT2_VID 1452
#define MT2_PID 613          /* USB idProduct */
#define ENABLE_REPORT_ID 0x02
static const uint8_t kEnablePayload[2] = {0x02, 0x01};

static CFDictionaryRef matchDict(uint32_t vid, uint32_t pid) {
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *vals[] = { v, p };
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(v); CFRelease(p);
    return d;
}

int main(int argc, char **argv) {
    int repeat = (argc > 1) ? atoi(argv[1]) : 1;
    if (repeat < 1) repeat = 1;

    IOHIDManagerRef mgr = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    CFDictionaryRef m = matchDict(MT2_VID, MT2_PID);
    IOHIDManagerSetDeviceMatching(mgr, m);
    CFRelease(m);

    IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDManagerOpen failed: 0x%08x\n", r);
        return 1;
    }
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (!devs || CFSetGetCount(devs) == 0) {
        fprintf(stderr, "no MT2 USB device (vid %d pid %d) found\n", MT2_VID, MT2_PID);
        return 2;
    }
    CFIndex n = CFSetGetCount(devs);
    const void **arr = calloc(n, sizeof(void *));
    CFSetGetValues(devs, arr);
    printf("found %ld matching HID device(s)\n", (long)n);

    int ok = 0;
    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef dev = (IOHIDDeviceRef)arr[i];
        IOReturn o = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
        if (o != kIOReturnSuccess)
            printf("  dev[%ld] open: 0x%08x (continuing; SetReport may still work)\n", (long)i, o);
        for (int k = 0; k < repeat; k++) {
            IOReturn s = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature,
                ENABLE_REPORT_ID, kEnablePayload, sizeof(kEnablePayload));
            printf("  dev[%ld] SetReport(Feature id 0x%02x {0x02,0x01}) #%d: 0x%08x %s\n",
                (long)i, ENABLE_REPORT_ID, k + 1, s,
                s == kIOReturnSuccess ? "OK" : "FAIL");
            if (s == kIOReturnSuccess) ok = 1;
        }
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    }
    free(arr);
    if (devs) CFRelease(devs);
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return ok ? 0 : 3;
}
