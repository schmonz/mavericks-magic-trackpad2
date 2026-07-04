/* mt2_set_btname — give our Bluetooth MT2 a proper display name in the Bluetooth prefpane.
 *
 * 10.9's BT stack mis-fetches the MT2's friendly name, so the pane shows it nameless. The pane
 * prefers the user-override "displayName" key, which blued does NOT overwrite on re-fetch (RE'd
 * 2026-06-25, docs/mt-stack/explanation.md "Bluetooth prefpane device identity"). The BT prefpane's
 * Rename uses -[IOBluetoothDevice setDisplayName:]; we do the same, programmatically, so the name
 * sticks across power-cycles/transports.
 *
 * NOTE (2026-07-04): an on-device "name follows the device" write via
 * -[AppleBluetoothHIDDevice setDeviceName:] was RE'd, attempted, and REMOVED — it is a no-op on this
 * hardware (the MT2 declares no DeviceName feature report reachable that way on 10.9: USB
 * MaxFeatureReportSize=1; over BT there's a 64B vendor feature report but no DeviceName mapping, and
 * ExtendedFeatures is empty). The real on-device write mechanism newer macOS uses is un-RE'd. See
 * [[mt2-device-writable-name]]. The host displayName alias below is the working name fix.
 *
 * Matches the MT2 by Class-of-Device 0x594 among paired devices; pass an explicit address to target one.
 *
 *   clang -fobjc-arc -mmacosx-version-min=10.9 -framework Foundation -framework IOBluetooth \
 *         -o mt2_set_btname tools/mt2_set_btname.m
 *   mt2_set_btname                       # name all CoD-0x594 paired devices "Magic Trackpad 2"
 *   mt2_set_btname "Magic Trackpad 2"    # explicit name
 *   mt2_set_btname "Magic Trackpad 2" 04-4b-ed-ec-02-07   # explicit name + address
 */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#define MT2_COD 0x594   /* Peripheral(5) + pointing + digitizer minor 0x25 — the MT2 over BT */

/* setDisplayName: is shipped in IOBluetooth but not in the public header; declare it. */
@interface IOBluetoothDevice (MT2Private)
- (void)setDisplayName:(NSString *)name;
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSString *want = (argc > 1) ? @(argv[1]) : @"Magic Trackpad 2";
        NSString *addr = (argc > 2) ? @(argv[2]) : nil;   /* optional, dash/colon address */

        NSArray *devs = [IOBluetoothDevice pairedDevices];
        int n = 0;
        for (IOBluetoothDevice *d in devs) {
            if (addr && [[d addressString] caseInsensitiveCompare:
                         [addr stringByReplacingOccurrencesOfString:@":" withString:@"-"]] != NSOrderedSame)
                continue;
            if (!addr && [d getClassOfDevice] != MT2_COD) continue;

            [d setDisplayName:want];   /* host-side per-pairing alias (what the pane's Rename sets) */
            printf("set displayName=\"%s\" on %s (CoD=0x%x)\n",
                   [want UTF8String], [[d addressString] UTF8String], (unsigned)[d getClassOfDevice]);
            n++;
        }
        if (!n) {
            printf("no matching device among %lu paired (looked for %s)\n",
                   (unsigned long)devs.count, addr ? [addr UTF8String] : "CoD 0x594");
            return 1;
        }
        return 0;
    }
}
