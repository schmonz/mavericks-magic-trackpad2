/* bt_inject_personality - add a BNBTrackpadDevice IOKit personality (at runtime, no
 * kext) matching the Magic Trackpad 2's REAL Bluetooth identity, so Apple's real
 * BT multitouch transport binds the live IOBluetoothL2CAPChannel and constructs the
 * AppleMultitouchDevice the system way.
 *
 * Match keys come from the MT2's live L2CAP channel: VendorID 76 (BT-SIG Apple),
 * ProductID 613 (0x0265), VendorIDSource 1. (Apple's stock BNBTrackpadDriver wants
 * the USB-form VID 1452 / source 2 / PID 782, so it never matches the MT2.)
 *
 * Inject:  sudo ./bt_inject_personality add
 * Remove:  sudo ./bt_inject_personality remove
 * Run as root. Reversible; a reboot also clears it.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

extern kern_return_t IOCatalogueSendData(mach_port_t masterPort, uint32_t flag,
                                         const char *buffer, uint32_t size);
#define kIOCatalogAddDrivers     1
#define kIOCatalogRemoveDrivers  2

static void put_str(CFMutableDictionaryRef d, const char *k, const char *v) {
    CFStringRef ck = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFStringRef cv = CFStringCreateWithCString(0, v, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, ck, cv); CFRelease(ck); CFRelease(cv);
}
static void put_int(CFMutableDictionaryRef d, const char *k, int v) {
    CFStringRef ck = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFNumberRef cv = CFNumberCreate(0, kCFNumberIntType, &v);
    CFDictionarySetValue(d, ck, cv); CFRelease(ck); CFRelease(cv);
}

int main(int argc, char **argv) {
    int remove = (argc > 1 && strcmp(argv[1], "remove") == 0);

    CFMutableDictionaryRef p = CFDictionaryCreateMutable(0, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    put_str(p, "IOClass", "BNBTrackpadDevice");
    put_str(p, "CFBundleIdentifier", "com.apple.driver.AppleBluetoothMultitouch");
    put_str(p, "IOProviderClass", "IOBluetoothL2CAPChannel");
    put_str(p, "IOMatchCategory", "BNBTrackpadDevice");
    put_int(p, "VendorID", 76);
    put_int(p, "ProductID", 613);
    put_int(p, "VendorIDSource", 1);
    put_int(p, "IOProbeScore", 100000);

    /* AddDrivers wants an array of personalities; RemoveDrivers wants a matching dict. */
    CFTypeRef payload;
    if (remove) {
        payload = p;  /* a matching dict: remove personalities that match these keys */
    } else {
        CFMutableArrayRef arr = CFArrayCreateMutable(0, 1, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(arr, p);
        payload = arr;
    }

    CFDataRef xml = CFPropertyListCreateData(0, payload, kCFPropertyListXMLFormat_v1_0, 0, 0);
    if (!xml) { fprintf(stderr, "serialize failed\n"); return 1; }

    kern_return_t kr = IOCatalogueSendData(kIOMasterPortDefault,
        remove ? kIOCatalogRemoveDrivers : kIOCatalogAddDrivers,
        (const char *)CFDataGetBytePtr(xml), (uint32_t)CFDataGetLength(xml));
    printf("IOCatalogueSendData(%s) -> 0x%x %s\n", remove ? "remove" : "add", kr,
           kr == KERN_SUCCESS ? "OK" : "FAIL");
    return kr == KERN_SUCCESS ? 0 : 1;
}
