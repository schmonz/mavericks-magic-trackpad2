#include "mavericks_vhid_mt1.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdlib.h>

/* IOHIDUserDevice is exported from IOKit but has no public header on 10.9. */
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
typedef enum { kRptInput = 0, kRptOutput, kRptFeature } RptType;
typedef IOReturn (*RptCB)(void *refcon, RptType type, uint32_t reportID,
                          uint8_t *report, CFIndex reportLength);
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef, CFDictionaryRef);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef, const uint8_t *, CFIndex);
extern void IOHIDUserDeviceScheduleWithRunLoop(IOHIDUserDeviceRef, CFRunLoopRef, CFStringRef);
extern void IOHIDUserDeviceRegisterGetReportCallback(IOHIDUserDeviceRef, RptCB, void *);
extern void IOHIDUserDeviceRegisterSetReportCallback(IOHIDUserDeviceRef, RptCB, void *);

struct vhid { IOHIDUserDeviceRef dev; };

/* The REAL Apple Magic Trackpad HID report descriptor (verbatim). It declares
 * the mouse report (0x02), a battery feature (0x47), a 64-byte vendor feature
 * (0x55, used to switch the device into multitouch mode), and a vendor input
 * (0x13). It deliberately does NOT declare the multitouch report (0x28): the
 * device sends those raw and AppleMultitouchHIDEventDriver reads them by id
 * once it recognizes the product. Presenting this exact descriptor is what
 * makes the driver treat us as a real trackpad. */
static const uint8_t kMT1Desc[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01,             /* Usage Page Desktop, Usage Mouse, Collection App */
    0x85,0x02,                                   /*   Report ID 2 */
    0x05,0x09, 0x19,0x01, 0x29,0x02,             /*   Buttons 1..2 */
    0x15,0x00, 0x25,0x01, 0x95,0x02, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x06, 0x81,0x03,             /*   6-bit padding */
    0x05,0x01, 0x09,0x01, 0xa1,0x00,             /*   Usage Pointer, Collection Physical */
    0x15,0x81, 0x25,0x7f, 0x09,0x30, 0x09,0x31, 0x75,0x08, 0x95,0x02, 0x81,0x06, /* X,Y rel */
    0xc0,                                        /*   End Physical */
    0x05,0x06, 0x09,0x20, 0x85,0x47,             /* Device Controls, Battery; Report ID 0x47 */
    0x15,0x00, 0x25,0x64, 0x75,0x08, 0x95,0x01, 0xb1,0xa2, /* feature battery */
    0x06,0x02,0xff, 0x09,0x55, 0x85,0x55,        /* Vendor 0xff02, usage 0x55; Report ID 0x55 */
    0x15,0x00, 0x26,0xff,0x00, 0x75,0x08, 0x95,0x40, 0xb1,0xa2, /* 64-byte vendor feature */
    0x85,0x13,                                   /* Report ID 0x13 */
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01,
    0x06,0x01,0xff, 0x09,0x0a, 0x81,0x02,        /* vendor input bit */
    0x06,0x01,0xff, 0x09,0x0c, 0x81,0x22,
    0x75,0x01, 0x95,0x06, 0x81,0x01,             /* 6-bit padding */
    0xc0,                                        /* End Collection (App) */
    0x00
};

static void put_num(CFMutableDictionaryRef d, CFStringRef k, int v) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    CFDictionarySetValue(d, k, n); CFRelease(n);
}

/* The driver may query (GET) or set (SET) feature reports (e.g. 0x55 to enable
 * multitouch, 0x47 battery). Acknowledge everything so init proceeds; answer
 * GETs with zeros. */
static IOReturn on_get(void *r, RptType type, uint32_t rid, uint8_t *report, CFIndex len) {
    (void)r;(void)type;(void)rid;
    if (report && len > 0) for (CFIndex i = 0; i < len; i++) report[i] = 0;
    return kIOReturnSuccess;
}
static IOReturn on_set(void *r, RptType type, uint32_t rid, uint8_t *report, CFIndex len) {
    (void)r;(void)type;(void)rid;(void)report;(void)len;
    return kIOReturnSuccess;
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
    IOHIDUserDeviceRegisterGetReportCallback(d, on_get, NULL);
    IOHIDUserDeviceRegisterSetReportCallback(d, on_set, NULL);
    IOHIDUserDeviceScheduleWithRunLoop(d, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
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
