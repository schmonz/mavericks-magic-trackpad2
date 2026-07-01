/*
 * mt2_battery_probe.c — read the Magic Trackpad 2 battery, live, read-only (no kext).
 *
 * In MULTITOUCH mode (what our driver puts the device in), the MT2 exposes battery as the standard
 * Apple Power-Device report on the vendor interface usagePage 0xff00 / usage 0x0b:
 *   Report ID 0x90, Usage Page 0x84 (Power Device) + 0x85 (Battery System),
 *   layout: [0x90 report-id][status-flags byte][capacity byte = Usage 0x65, 0..100].
 * So GET_REPORT(Input, 0x90) returns 3 bytes; battery % = byte[2]. Verified live = 0x64 (100%), stable.
 * (The mouse-mode descriptor's battery-strength report 0x47 is NOT present in multitouch mode — red herring.)
 * The report is device-native, so it also arrives over Bluetooth (our BT reader can grab it from the stream).
 *
 * Build: clang -O2 tools/mt2_battery_probe.c -framework IOKit -framework CoreFoundation -o /tmp/mt2batt
 */
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

static int ip(IOHIDDeviceRef d, CFStringRef k) {
    CFNumberRef n = (CFNumberRef)IOHIDDeviceGetProperty(d, k);
    int v = 0; if (n) CFNumberGetValue(n, kCFNumberIntType, &v); return v;
}

int main(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(mgr, NULL);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (!devs) { fprintf(stderr, "no HID devices\n"); return 1; }
    CFIndex n = CFSetGetCount(devs);
    IOHIDDeviceRef *arr = malloc(sizeof(IOHIDDeviceRef) * n);
    CFSetGetValues(devs, (const void **)arr);
    int hits = 0;
    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef d = arr[i];
        if (ip(d, CFSTR(kIOHIDProductIDKey)) != 0x0265) continue;   /* MT2: USB vid 0x05ac / BT vid 0x004c */
        IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone);
        uint8_t b[16] = {0}; CFIndex L = sizeof b;
        IOReturn r = IOHIDDeviceGetReport(d, kIOHIDReportTypeInput, 0x90, b, &L);
        if (r == kIOReturnSuccess && L >= 3 && b[0] == 0x90) {       /* only the iface exposing 0x90 answers */
            CFStringRef tr = (CFStringRef)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDTransportKey));
            char t[24] = "?"; if (tr) CFStringGetCString(tr, t, sizeof t, kCFStringEncodingUTF8);
            printf("Magic Trackpad 2 battery: %d%%  (%s, %s, report 0x90 raw:",
                   b[2], t, (b[1] & 0x05) ? "charging" : "on battery");
            for (CFIndex k = 0; k < L; k++) printf(" %02x", b[k]);
            printf(")\n");
            hits++;
        }
        IOHIDDeviceClose(d, kIOHIDOptionsTypeNone);
    }
    free(arr);
    if (!hits) fprintf(stderr, "(no battery read — device on BT, or claimed exclusively)\n");
    return hits ? 0 : 2;
}
