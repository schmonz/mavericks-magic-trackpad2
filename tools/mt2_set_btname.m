/* mt2_set_btname — give our Bluetooth MT2 a proper display name in the Bluetooth prefpane.
 *
 * 10.9's BT stack mis-fetches the MT2's friendly name (stores the bytes 0x02 0x01), so the pane shows
 * it nameless. The pane prefers the user-override "displayName" key, which blued does NOT overwrite on
 * re-fetch (RE'd 2026-06-25, docs/mt-stack/explanation.md "Bluetooth prefpane device identity"). The
 * BT prefpane's Rename uses -[IOBluetoothDevice setDisplayName:]; we do the same, programmatically, so
 * the name sticks across power-cycles/transports.
 *
 * Matches the MT2 by Class-of-Device 0x594 (Peripheral / pointing / digitizer) among paired devices;
 * pass an explicit dash- or colon-separated address to target one device, and/or a name as the 2nd arg.
 *
 *   clang -fobjc-arc -mmacosx-version-min=10.9 -framework Foundation -framework IOBluetooth \
 *         -o mt2_set_btname tools/mt2_set_btname.m
 *   mt2_set_btname                       # name all CoD-0x594 paired devices "Magic Trackpad 2"
 *   mt2_set_btname "Magic Trackpad 2"    # explicit name, auto-detect device(s)
 *   mt2_set_btname "Magic Trackpad 2" 04-4b-ed-ec-02-07   # explicit name + address
 *   mt2_set_btname --mirror [addr]       # MIRROR MODE: read the device's CURRENT displayName (what the
 *                                        #   pane's right-click Rename wrote) and push it ONBOARD via
 *                                        #   setDeviceName:, so a UI rename follows the device. Idempotent
 *                                        #   (setDeviceName: no-ops if unchanged). Driven by a LaunchAgent
 *                                        #   (dist/com.schmonz.mt2namemirror.plist) that WatchPaths the BT
 *                                        #   prefs; fires on every rename. Does NOT rewrite the alias.
 */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <objc/runtime.h>   /* runtime introspection: adapt to whatever name-setter this box's IOBluetooth has */

#define MT2_COD 0x594   /* Peripheral(5) + pointing + digitizer minor 0x25 — the MT2 over BT */

/* setDisplayName: is shipped in IOBluetooth but not in the public header; declare it. */
@interface IOBluetoothDevice (MT2Private)
- (void)setDisplayName:(NSString *)name;
- (NSString *)getDisplayName;   /* current host alias — what the pane's Rename wrote (mirror source) */
@end

/* BluetoothHIDDevice::setDeviceName: writes the ON-DEVICE friendly name — a HID feature-report write
 * that persists ON the device and follows it across Macs/re-pairs (RE'd 2026-06-30, [[mt2-device-writable-name]])
 * — vs setDisplayName:'s per-pairing HOST alias. Our unit's on-device name got clobbered to bytes 0x02
 * 0x01 (our multitouch-enable payload); one clean setDeviceName: write fixes it at the source, so even a
 * re-fetch/right-click "Update Name" shows it. withBluetoothDevice: may return nil if the device isn't
 * BT-connected or its ExtendedFeatures gate isn't up (our kext publishes that); both private, declare. */
/* The name machinery (setDeviceName:/reportIDForReportKey:) lives on AppleBluetoothHIDDevice — it reads
 * the subclass ivar _featureDict — NOT the base BluetoothHIDDevice. (RE 2026-07-04: setDeviceName:@0x43180
 * and reportIDForReportKey:@0x441ff both touch _OBJC_IVAR_$_AppleBluetoothHIDDevice._featureDict.) This is
 * the SAME class our battery reads use. Calling on the base class was the "unrecognized selector" crash. */
@interface AppleBluetoothHIDDevice : NSObject
+ (instancetype)withBluetoothDevice:(IOBluetoothDevice *)device;
- (void)setDeviceName:(NSString *)name;
- (int)reportIDForReportKey:(NSString *)key;
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        /* --mirror: source the name from each device's CURRENT displayName (the pane's Rename result)
         * and push it onboard only. Otherwise: set the alias + onboard to an explicit/default name. */
        BOOL mirror = (argc > 1 && strcmp(argv[1], "--mirror") == 0);
        NSString *want = mirror ? nil : ((argc > 1) ? @(argv[1]) : @"Magic Trackpad 2");
        NSString *addr = (argc > (mirror ? 2 : 2)) ? @(argv[2]) : nil;   /* optional, dash/colon address */

        NSArray *devs = [IOBluetoothDevice pairedDevices];
        int n = 0;
        for (IOBluetoothDevice *d in devs) {
            if (addr && [[d addressString] caseInsensitiveCompare:
                         [addr stringByReplacingOccurrencesOfString:@":" withString:@"-"]] != NSOrderedSame)
                continue;
            if (!addr && [d getClassOfDevice] != MT2_COD) continue;

            NSString *name = want;
            if (mirror) {
                /* The rename the user just made lives in the host alias; mirror THAT onboard. */
                name = [d getDisplayName];
                if (!name || name.length == 0) {
                    printf("mirror: %s has no displayName yet — nothing to push onboard\n",
                           [[d addressString] UTF8String]);
                    n++;   /* matched the device; just nothing to do */
                    continue;
                }
            } else {
                [d setDisplayName:name];   /* host-side per-pairing alias (what the pane's Rename sets) */
                printf("set displayName=\"%s\" on %s (CoD=0x%x)\n",
                       [want UTF8String], [[d addressString] UTF8String], (unsigned)[d getClassOfDevice]);
            }

            /* On-device name (persists on the device, un-clobbers the 0x02 0x01 we wrote; follows the
             * device across Macs). setDeviceName: no-ops if unchanged, so mirror fires are cheap.
             * Best-effort: the HID wrapper is nil unless the MT2 is BT-connected + ExtendedFeatures up. */
            id hid = [AppleBluetoothHIDDevice withBluetoothDevice:d];
            const char *tag = mirror ? "mirror:" : "  +";
            if (hid && [hid respondsToSelector:@selector(setDeviceName:)]) {
                [hid setDeviceName:name];
                printf("%s setDeviceName=\"%s\" on-device (%s)\n",
                       tag, [name UTF8String], class_getName(object_getClass(hid)));
            } else if (hid) {
                /* Bonus for the kext fallback: dump _featureDict so we harvest the report IDs (the
                 * key->{id,min,max} map that reportIDForReportKey: reads) for a direct SET_REPORT write. */
                Ivar fv = class_getInstanceVariable(object_getClass(hid), "_featureDict");
                id fd = fv ? object_getIvar(hid, fv) : nil;
                if (fd) printf("      _featureDict = %s\n", [[fd description] UTF8String]);
                /* The wrapper exists but does NOT respond to setDeviceName: — this is what crashed on the
                 * target box. Almost certainly a framework-version mismatch: setDeviceName: exists in the
                 * IOBluetooth we RE'd (the dev box) but not in the target's build. Instead of crashing,
                 * report the REAL runtime class + every name-ish setter it DOES expose, so we learn what
                 * this machine offers and adapt. (See mt2-kernelcache-re-workflow: running binary != on-disk.) */
                Class c = object_getClass(hid);
                printf("%s setDeviceName: NOT available on this box's %s — introspecting:\n",
                       mirror ? "mirror:" : "  !", class_getName(c));
                for (Class k = c; k && k != [NSObject class]; k = class_getSuperclass(k)) {
                    unsigned mc = 0; Method *ms = class_copyMethodList(k, &mc);
                    for (unsigned i = 0; i < mc; i++) {
                        const char *s = sel_getName(method_getName(ms[i]));
                        if (strcasestr(s, "name")) printf("      %s  -[%s %s]\n",
                            [[NSString stringWithFormat:@"(%@)", NSStringFromClass(k)] UTF8String], class_getName(k), s);
                    }
                    if (ms) free(ms);
                }
                printf("      -> falling back to the host displayName alias only for this device\n");
            } else {
                printf("%s setDeviceName: skipped — HID wrapper nil (MT2 not BT-connected, or ExtendedFeatures gate down)\n",
                       mirror ? "mirror:" : "  !");
            }
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
