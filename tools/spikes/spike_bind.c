// spike_bind.c — go/no-go: does AppleMultitouchHIDEventDriver bind to a
// userspace-backed IOHIDUserDevice that advertises as a Magic Trackpad 1?
//
// Build: clang -o spike_bind spike_bind.c -framework IOKit -framework CoreFoundation
// Run:   ./spike_bind   (keeps device alive; inspect with ioreg in another shell)

#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

// IOHIDUserDevice API is exported from IOKit but has no public header on 10.9.
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef allocator, CFDictionaryRef properties);
extern void IOHIDUserDeviceScheduleWithRunLoop(IOHIDUserDeviceRef device, CFRunLoopRef runLoop, CFStringRef runLoopMode);

// Top-level Generic Desktop / Mouse collection so DeviceUsagePair = (1,2),
// which is what BNBTrackpadEventDriver requires to match. Includes a buttons +
// X/Y mouse report (report ID 2) plus a vendor multitouch report (ID 0x29).
static const uint8_t kDesc[] = {
    0x05, 0x01,             // Usage Page (Generic Desktop)
    0x09, 0x02,             // Usage (Mouse)
    0xa1, 0x01,             // Collection (Application)
    0x85, 0x02,             //   Report ID (2)
    0x09, 0x01,             //   Usage (Pointer)
    0xa1, 0x00,             //   Collection (Physical)
    0x05, 0x09,             //     Usage Page (Button)
    0x19, 0x01, 0x29, 0x03, //     Usage Min 1 .. Max 3
    0x15, 0x00, 0x25, 0x01, //     Logical 0..1
    0x95, 0x03, 0x75, 0x01, //     Count 3, Size 1
    0x81, 0x02,             //     Input (Data,Var,Abs)
    0x95, 0x01, 0x75, 0x05, //     Count 1, Size 5 (padding)
    0x81, 0x03,             //     Input (Const)
    0x05, 0x01,             //     Usage Page (Generic Desktop)
    0x09, 0x30, 0x09, 0x31, //     Usage X, Y
    0x15, 0x81, 0x25, 0x7f, //     Logical -127..127
    0x75, 0x08, 0x95, 0x02, //     Size 8, Count 2
    0x81, 0x06,             //     Input (Data,Var,Rel)
    0xc0,                   //   End Collection (Physical)
    0x06, 0x00, 0xff,       //   Usage Page (Vendor 0xff00)
    0x85, 0x29,             //   Report ID (0x29)
    0x09, 0x01,             //   Usage (0x01)
    0x15, 0x00, 0x26, 0xff, 0x00, // Logical 0..255
    0x75, 0x08, 0x95, 0x08, //   Size 8, Count 8
    0x81, 0x02,             //   Input (Data,Var,Abs)
    0xc0                    // End Collection (Application)
};

static void setNum(CFMutableDictionaryRef d, const char *key, int val) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val);
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, k, n);
    CFRelease(n); CFRelease(k);
}
static void setStr(CFMutableDictionaryRef d, const char *key, const char *val) {
    CFStringRef v = CFStringCreateWithCString(kCFAllocatorDefault, val, kCFStringEncodingUTF8);
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, k, v);
    CFRelease(v); CFRelease(k);
}

int main(void) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFDataRef desc = CFDataCreate(kCFAllocatorDefault, kDesc, sizeof(kDesc));
    CFDictionarySetValue(props, CFSTR("ReportDescriptor"), desc);

    setNum(props, "VendorID", 1452);
    setNum(props, "ProductID", 782);        // 0x30E = Magic Trackpad 1 (Bluetooth)
    setNum(props, "VendorIDSource", 2);     // Bluetooth SIG
    setNum(props, "LocationID", 0x12345678);
    setNum(props, "PrimaryUsagePage", 0x01);
    setNum(props, "PrimaryUsage", 0x02);
    setStr(props, "Transport", "Bluetooth");
    setStr(props, "Product", "Magic Trackpad");
    setStr(props, "Manufacturer", "Apple Inc.");

    IOHIDUserDeviceRef dev = IOHIDUserDeviceCreate(kCFAllocatorDefault, props);
    if (!dev) {
        fprintf(stderr, "IOHIDUserDeviceCreate FAILED\n");
        return 1;
    }
    printf("IOHIDUserDeviceCreate OK — virtual device created.\n");
    printf("Inspect with:  ioreg -l -w0 | grep -iA30 'Magic Trackpad'\n");
    printf("Looking for AppleMultitouchHIDEventDriver attached. Ctrl-C to remove.\n");

    IOHIDUserDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRunLoopRun();
    return 0;
}
