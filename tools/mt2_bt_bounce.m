/* mt2_bt_bounce — force a clean Bluetooth re-establish of the paired MT2.
 *
 * Why: after a kext reload the BT *link* stays up, so the device never re-initiates its interrupt
 * channel (PSM 19 is device-initiated). Our reader re-matches the still-open control channel (PSM 17)
 * but the manually-started BNBTrackpadDevice has no fresh PSM 19 to bind, so its start() fails and the
 * pad is dead until a manual tap (see docs/mt-stack + the reload-collision note). Bouncing the baseband
 * link (closeConnection -> openConnection) makes the device re-open BOTH PSM 17 and 19, so our reader
 * re-matches and BNB starts clean — no manual tap.
 *
 * The Bluetooth twin of tools/mt2_reenumerate (which does the equivalent for USB via re-enumerate).
 * The MT2 only ever drives one transport at a time, so this is a clean-reconnect helper, not an
 * arbiter — there is nothing to tear down on the other transport.
 *
 *   clang -fobjc-arc -mmacosx-version-min=10.9 -framework Foundation -framework IOBluetooth \
 *         -o mt2_bt_bounce tools/mt2_bt_bounce.m
 *   mt2_bt_bounce                 # bounce all CoD-0x594 paired devices (the MT2 over BT)
 *   mt2_bt_bounce 04-4b-ed-ec-02-07   # bounce one device by address (dash- or colon-separated)
 */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#define MT2_COD 0x594   /* Peripheral(5) + pointing + digitizer minor 0x25 — the MT2 over BT */

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSString *addr = (argc > 1) ? @(argv[1]) : nil;   /* optional dash/colon address */

        NSArray *devs = [IOBluetoothDevice pairedDevices];
        int n = 0;
        for (IOBluetoothDevice *d in devs) {
            if (addr && [[d addressString] caseInsensitiveCompare:
                         [addr stringByReplacingOccurrencesOfString:@":" withString:@"-"]] != NSOrderedSame)
                continue;
            if (!addr && [d getClassOfDevice] != MT2_COD) continue;

            BOOL wasConnected = [d isConnected];
            if (wasConnected) {
                IOReturn rc = [d closeConnection];   /* synchronous: returns after the link is down */
                printf("closeConnection %s -> 0x%08x\n", [[d addressString] UTF8String], rc);
            }
            /* openConnection re-establishes the baseband link; IOBluetooth then re-runs HID matching,
             * so our MT2BTReader wins PSM 17 again and the device opens PSM 19 -> clean BNB start. */
            IOReturn ro = [d openConnection];
            printf("openConnection  %s -> 0x%08x (wasConnected=%d, CoD=0x%x)\n",
                   [[d addressString] UTF8String], ro, wasConnected, (unsigned)[d getClassOfDevice]);
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
