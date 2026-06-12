#include "vhid_mt1.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdlib.h>

/* IOHIDUserDevice is exported from IOKit but has no public header on 10.9. */
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef, CFDictionaryRef);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef, const uint8_t *, CFIndex);

struct vhid { IOHIDUserDeviceRef dev; };

/* Top-level Generic Desktop / Mouse collection (required for driver binding:
 * satisfies BNBTrackpadEventDriver's DeviceUsagePairs {1,1}/{1,2}), then a
 * vendor-defined input report (ID 0x28) large enough to carry MT1 touch data. */
static const uint8_t kMT1Desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,       /* Usage Page Desktop, Usage Mouse, Collection App */
    0x85, 0x02,                               /*   Report ID 2 (mouse) */
    0x09, 0x01, 0xa1, 0x00,                   /*   Usage Pointer, Collection Physical */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,       /*     Buttons 1..3 */
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,       /*     5-bit padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,       /*     X, Y */
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0,                                     /*   End Physical */
    0x06, 0x00, 0xff,                         /*   Usage Page Vendor 0xff00 */
    0x85, 0x28,                               /*   Report ID 0x28 (multitouch) */
    0x09, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, /*   Usage 1, logical 0..255 */
    0x75, 0x08, 0x95, 0xff,                   /*   Size 8, Count 255 (max touch payload) */
    0x81, 0x02,                               /*   Input (Data,Var,Abs) */
    0xc0                                      /* End App */
};

static void put_num(CFMutableDictionaryRef d, CFStringRef k, int v) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    CFDictionarySetValue(d, k, n); CFRelease(n);
}

vhid_t *vhid_create(void) {
    CFMutableDictionaryRef p = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef desc = CFDataCreate(kCFAllocatorDefault, kMT1Desc, sizeof(kMT1Desc));
    CFDictionarySetValue(p, CFSTR("ReportDescriptor"), desc); CFRelease(desc);
    put_num(p, CFSTR("VendorID"), 1452);
    put_num(p, CFSTR("ProductID"), 782);
    put_num(p, CFSTR("VendorIDSource"), 2);
    put_num(p, CFSTR("PrimaryUsagePage"), 0x01);
    put_num(p, CFSTR("PrimaryUsage"), 0x02);
    CFDictionarySetValue(p, CFSTR("Transport"), CFSTR("Bluetooth"));
    CFDictionarySetValue(p, CFSTR("Product"), CFSTR("Magic Trackpad"));
    CFDictionarySetValue(p, CFSTR("Manufacturer"), CFSTR("Apple Inc."));

    IOHIDUserDeviceRef d = IOHIDUserDeviceCreate(kCFAllocatorDefault, p);
    CFRelease(p);
    if (!d) return NULL;
    vhid_t *v = calloc(1, sizeof(*v));
    v->dev = d;
    return v;
}

int vhid_send(vhid_t *v, const uint8_t *report, size_t len) {
    if (!v || !v->dev) return -1;
    return IOHIDUserDeviceHandleReport(v->dev, report, (CFIndex)len) == kIOReturnSuccess ? 0 : -1;
}

void vhid_destroy(vhid_t *v) {
    if (!v) return;
    if (v->dev) CFRelease(v->dev);
    free(v);
}
