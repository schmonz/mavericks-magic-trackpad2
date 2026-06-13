#include "vhid_mouse.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdlib.h>

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef, CFDictionaryRef);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef, const uint8_t *, CFIndex);

struct vhid_mouse { IOHIDUserDeviceRef dev; };

/* Standard mouse: report id 1, 3 buttons, relative X/Y, vertical wheel, and a
 * horizontal wheel via Consumer AC Pan. Report = {1, buttons, dx, dy, wheel, hwheel}. */
static const uint8_t kMouseDesc[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01,             /* Desktop, Mouse, App */
    0x85,0x01,                                   /*   Report ID 1 */
    0x09,0x01, 0xa1,0x00,                        /*   Pointer, Physical */
    0x05,0x09, 0x19,0x01, 0x29,0x03,             /*     Buttons 1..3 */
    0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x03,             /*     5-bit pad */
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38,  /*     X, Y, Wheel */
    0x15,0x81, 0x25,0x7f, 0x75,0x08, 0x95,0x03, 0x81,0x06,
    0x05,0x0c, 0x0a,0x38,0x02,                   /*     Consumer, AC Pan */
    0x15,0x81, 0x25,0x7f, 0x75,0x08, 0x95,0x01, 0x81,0x06,
    0xc0,                                        /*   End Physical */
    0xc0                                         /* End App */
};

static void put_num(CFMutableDictionaryRef d, CFStringRef k, int v) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    CFDictionarySetValue(d, k, n); CFRelease(n);
}

vhid_mouse_t *vhid_mouse_create(void) {
    CFMutableDictionaryRef p = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef desc = CFDataCreate(kCFAllocatorDefault, kMouseDesc, sizeof(kMouseDesc));
    CFDictionarySetValue(p, CFSTR("ReportDescriptor"), desc); CFRelease(desc);
    put_num(p, CFSTR("VendorID"), 0x05ac);
    put_num(p, CFSTR("ProductID"), 0x0265);
    put_num(p, CFSTR("PrimaryUsagePage"), 0x01);
    put_num(p, CFSTR("PrimaryUsage"), 0x02);
    CFDictionarySetValue(p, CFSTR("Transport"), CFSTR("USB"));
    CFDictionarySetValue(p, CFSTR("Product"), CFSTR("Magic Trackpad 2 (mt2d)"));
    CFDictionarySetValue(p, CFSTR("Manufacturer"), CFSTR("mt2d"));

    IOHIDUserDeviceRef d = IOHIDUserDeviceCreate(kCFAllocatorDefault, p);
    CFRelease(p);
    if (!d) return NULL;
    vhid_mouse_t *v = calloc(1, sizeof(*v));
    v->dev = d;
    return v;
}

static int8_t clamp8(int v) { return v < -127 ? -127 : (v > 127 ? 127 : (int8_t)v); }

int vhid_mouse_send(vhid_mouse_t *v, unsigned buttons, int dx, int dy, int wheel, int hwheel) {
    if (!v || !v->dev) return -1;
    uint8_t rep[6];
    rep[0] = 0x01;
    rep[1] = (uint8_t)(buttons & 0x07);
    rep[2] = (uint8_t)clamp8(dx);
    rep[3] = (uint8_t)clamp8(dy);
    rep[4] = (uint8_t)clamp8(wheel);
    rep[5] = (uint8_t)clamp8(hwheel);
    return IOHIDUserDeviceHandleReport(v->dev, rep, sizeof(rep)) == kIOReturnSuccess ? 0 : -1;
}

void vhid_mouse_destroy(vhid_mouse_t *v) {
    if (!v) return;
    if (v->dev) CFRelease(v->dev);
    free(v);
}
