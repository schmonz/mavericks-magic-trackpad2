/* mt2_hidnode_probe — mirror the Trackpad prefpane's battery-node resolution EXACTLY.
 *
 * The pane's -[AppleBluetoothHIDDevice batteryPercent] reads its BatteryPercent from an
 * io_service resolved by _HIDDeviceForIOBluetoothDevice(dev, "IOBluetoothHIDDriver"):
 *   IOServiceGetMatchingServices(IOServiceMatching("IOBluetoothHIDDriver"))
 *   -> the entry whose "BD_ADDR" property equals the device address.
 * This tool runs that same match and prints every result's class + BD_ADDR + BatteryPercent,
 * so we can see whether our manually-started BNBTrackpadDevice is in the pane's match set and
 * what battery it would read. Read-only. Build:
 *   clang -o /tmp/mt2_hidnode_probe tools/mt2_hidnode_probe.c -framework IOKit -framework CoreFoundation
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

static void print_cfstr(const char *label, io_registry_entry_t e, const char *key) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!v) { printf("  %s = (absent)\n", label); return; }
    CFTypeID t = CFGetTypeID(v);
    if (t == CFNumberGetTypeID()) {
        long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &n);
        printf("  %s = %ld\n", label, n);
    } else if (t == CFDataGetTypeID()) {
        const UInt8 *b = CFDataGetBytePtr((CFDataRef)v); CFIndex len = CFDataGetLength((CFDataRef)v);
        printf("  %s = <", label);
        for (CFIndex i = 0; i < len; i++) printf("%02x", b[i]);
        printf(">\n");
    } else if (t == CFStringGetTypeID()) {
        char buf[128]; CFStringGetCString((CFStringRef)v, buf, sizeof(buf), kCFStringEncodingUTF8);
        printf("  %s = \"%s\"\n", label, buf);
    } else {
        printf("  %s = (type %ld)\n", label, (long)t);
    }
    CFRelease(v);
}

int main(int argc, char **argv) {
    const char *cls = (argc > 1) ? argv[1] : "IOBluetoothHIDDriver";
    printf("IOServiceGetMatchingServices(\"%s\"):\n", cls);
    io_iterator_t it = MACH_PORT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMasterPortDefault, IOServiceMatching(cls), &it);
    if (kr != KERN_SUCCESS) { printf("  match failed: 0x%x\n", kr); return 1; }
    int n = 0;
    for (io_registry_entry_t e; (e = IOIteratorNext(it)); IOObjectRelease(e)) {
        io_name_t name = {0};
        IOObjectGetClass(e, name);
        printf("[%d] class = %s\n", n++, name);
        print_cfstr("BD_ADDR", e, "BD_ADDR");
        print_cfstr("BatteryPercent", e, "BatteryPercent");
        print_cfstr("Product", e, "Product");
    }
    IOObjectRelease(it);
    if (n == 0) printf("  (no matches — the pane's _hidDevice would be NULL -> 0%%)\n");
    return 0;
}
