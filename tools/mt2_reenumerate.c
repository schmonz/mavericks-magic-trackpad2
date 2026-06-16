/* Force the Magic Trackpad 2 to re-enumerate so IOKit re-runs driver matching
 * (and the MT2USBClaim kext, once installed, wins the multitouch interface). Used
 * by the installer so activation doesn't require a reboot. */
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

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
    (*d)->Release(d);
    return (r2 == kIOReturnSuccess) ? 0 : 1;
}
