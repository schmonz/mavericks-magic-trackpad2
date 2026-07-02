/* mt2_panebattery_probe — replicate the Trackpad prefpane's battery read EXACTLY.
 *
 * The pane's -[BaseTrackPadController _checkBatteryTimer:] does, for each paired IOBluetoothDevice
 * that isConnected and getDeviceClassMinor == 0x25:
 *     [[AppleBluetoothHIDDevice withBluetoothDevice:dev] batteryPercent]
 * and shows that float. This runs the identical calls (via the ObjC runtime, no private headers) and
 * prints the result + the resolved _hidDevice, so we see what the PANE would show — independent of
 * ioreg (which showed BatteryPercent=100 on the node). Read-only. Build:
 *   clang -o /tmp/mt2_panebattery_probe tools/mt2_panebattery_probe.m \
 *     -framework Foundation -framework IOBluetooth -lobjc
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

int main(void) {
    @autoreleasepool {
        Class BTDev = objc_getClass("IOBluetoothDevice");
        Class AHID  = objc_getClass("AppleBluetoothHIDDevice");
        if (!BTDev || !AHID) { printf("missing class BTDev=%p AHID=%p\n", BTDev, AHID); return 1; }

        NSArray *paired = ((id(*)(Class,SEL))objc_msgSend)(BTDev, sel_getUid("pairedDevices"));
        printf("pairedDevices: %lu\n", (unsigned long)paired.count);
        for (id dev in paired) {
            BOOL conn = ((BOOL(*)(id,SEL))objc_msgSend)(dev, sel_getUid("isConnected"));
            int minor = ((int(*)(id,SEL))objc_msgSend)(dev, sel_getUid("getDeviceClassMinor"));
            id name = ((id(*)(id,SEL))objc_msgSend)(dev, sel_getUid("name"));
            printf("- name=%s connected=%d minor=0x%x\n",
                   name ? [[NSString stringWithFormat:@"%@", name] UTF8String] : "(nil)", conn, minor);
            if (!conn || minor != 0x25) continue;

            /* Dump getAddress (the 6 bytes _HIDDeviceForIOBluetoothDevice compares to each
             * IOBluetoothHIDDriver node's BD_ADDR). A byte-order/format mismatch vs our node's
             * BD_ADDR=<044bedec0207> would make the match fail -> nil -> 0%. */
            const unsigned char *addr =
                ((const unsigned char *(*)(id,SEL))objc_msgSend)(dev, sel_getUid("getAddress"));
            if (addr) printf("    getAddress bytes = <%02x%02x%02x%02x%02x%02x>\n",
                             addr[0],addr[1],addr[2],addr[3],addr[4],addr[5]);
            id addrStr = ((id(*)(id,SEL))objc_msgSend)(dev, sel_getUid("addressString"));
            if (addrStr) printf("    addressString = %s\n",
                                [[NSString stringWithFormat:@"%@", addrStr] UTF8String]);

            /* Replicate _HIDDeviceForIOBluetoothDevice EXACTLY: match IOBluetoothHIDDriver nodes,
             * CFEqual their BD_ADDR (CFData) to NSData(getAddress,6). */
            if (addr) {
                NSData *want = [NSData dataWithBytes:addr length:6];
                io_iterator_t it = 0;
                IOServiceGetMatchingServices(kIOMasterPortDefault,
                    IOServiceMatching("IOBluetoothHIDDriver"), &it);
                int found = 0;
                for (io_registry_entry_t e; (e = IOIteratorNext(it)); IOObjectRelease(e)) {
                    io_name_t cn = {0}; IOObjectGetClass(e, cn);
                    CFTypeRef bd = IORegistryEntryCreateCFProperty(e, CFSTR("BD_ADDR"),
                                                                   kCFAllocatorDefault, 0);
                    BOOL eq = bd && CFGetTypeID(bd) == CFDataGetTypeID()
                              && [(__bridge NSData *)bd isEqual:want];
                    printf("    [match] class=%s BD_ADDR-type=%s isEqual(getAddress)=%d\n",
                           cn, bd ? (CFGetTypeID(bd)==CFDataGetTypeID()?"data":"other") : "absent", eq);
                    if (bd) CFRelease(bd);
                    if (eq) found = 1;
                }
                IOObjectRelease(it);
                printf("    -> _HIDDeviceForIOBluetoothDevice would %s\n",
                       found ? "MATCH (nonzero)" : "return 0 (nil device -> 0%)");
            }

            /* Exactly the pane's call. */
            id hid = ((id(*)(Class,SEL,id))objc_msgSend)(AHID, sel_getUid("withBluetoothDevice:"), dev);
            printf("    withBluetoothDevice -> %p (class %s)\n",
                   hid, hid ? object_getClassName(hid) : "(nil)");
            if (hid) {
                /* Peek the resolved _hidDevice io_service (ivar). */
                Ivar iv = class_getInstanceVariable(object_getClass(hid), "_hidDevice");
                if (iv) {
                    unsigned int hd = *(unsigned int *)((__bridge void *)hid + ivar_getOffset(iv));
                    printf("    _hidDevice io_service = 0x%x\n", hd);
                }
                float pct = ((float(*)(id,SEL))objc_msgSend)(hid, sel_getUid("batteryPercent"));
                printf("    batteryPercent -> %.1f   <-- THIS is what the pane shows\n", pct);
            }
        }
    }
    return 0;
}
