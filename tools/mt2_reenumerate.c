/* Force the Magic Trackpad 2 to re-enumerate so IOKit re-runs driver matching
 * (so MT2USBReader's high-probe-score personality wins interface 1 over
 * IOUSBHIDDriver). Used by the installer so activation doesn't require a reboot. */
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <syslog.h>

int main(void) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = 0x05ac, pid = 0x0265;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(0, kCFNumberIntType, &vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(0, kCFNumberIntType, &pid));
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, m);
    if (!s) { fprintf(stderr, "MT2 not found\n"); return 1; }
    IOCFPlugInInterface **pl; SInt32 sc;
    IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &pl, &sc);
    IOObjectRelease(s);
    if (!pl) { fprintf(stderr, "plugin failed\n"); return 1; }
    IOUSBDeviceInterface500 **d = NULL;
    (*pl)->QueryInterface(pl, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&d);
    (*pl)->Release(pl);
    if (!d) { fprintf(stderr, "queryinterface failed\n"); return 1; }
    IOReturn r1 = (*d)->USBDeviceOpenSeize(d);
    IOReturn r2 = (*d)->USBDeviceReEnumerate(d, 0);
    printf("openSeize=0x%08x reEnumerate=0x%08x\n", r1, r2);
    /* Persistent "had something to do" signal (the wrapper pipes our stdout to /dev/null): we only reach
     * here when the USB MT2 was PRESENT and we re-enumerated it — i.e. the boot wrapper found the reader
     * had lost interface 1 and called us to reclaim it. If this line NEVER appears across a bunch of USB
     * boots, the reader always wins interface 1 on its own and this tool can be retired. */
    syslog(LOG_NOTICE, "mt2_reenumerate: reclaimed the USB MT2 (openSeize=0x%08x reEnumerate=0x%08x)", r1, r2);
    (*d)->Release(d);
    return (r2 == kIOReturnSuccess) ? 0 : 1;
}
