/* mt2_usb_bt_handoff — no-click USB→BT handoff (the "I unplug the cable and it just keeps working" bit).
 *
 * When the MT2 is on USB, cabling drops its Bluetooth link (single-transport device,
 * docs/mt-stack + [[mt2-single-transport-at-a-time]]). Unplug the cable and the BT radio is
 * deep-idle: today it takes a physical CLICK to wake, so there's a dead gap. PROVEN on-device
 * 2026-07-04: a host `-[IOBluetoothDevice openConnection]` wakes that deep-idle radio with ZERO
 * physical action — the same primitive tools/mt2_bt_bounce uses for the reload case.
 *
 * This daemon closes the gap: it arms an IOKit termination notification on our own USB reader
 * class `com_schmonz_MT2USBReader` (fires exactly when the MT2's USB cable is pulled) and, on
 * fire, calls `openConnection` on the paired CoD-0x594 MT2. No tap. Runs as a root LaunchDaemon
 * so it works regardless of GUI session (dist/com.schmonz.mt2usbbthandoff.plist).
 *
 * ASYMMETRIC BY DESIGN (do NOT make it symmetric): only the USB→BT (unplug) direction needs help.
 * BT→USB (plug in) is already smooth and the device handles it — we add nothing there. The
 * "ensure BT is paired on plug-in" half is a separate, un-proven enhancement, deliberately omitted.
 *
 *   clang -fobjc-arc -mmacosx-version-min=10.9 -framework Foundation -framework IOBluetooth \
 *         -framework IOKit -o mt2_usb_bt_handoff tools/mt2_usb_bt_handoff.m
 */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <IOKit/IOKitLib.h>
#include <string.h>

#define MT2_COD 0x594   /* Peripheral(5) + pointing + digitizer minor 0x25 — the MT2 over BT */

/* Wake the deep-idle BT MT2 the same way mt2_bt_bounce does: a plain openConnection. When the USB
 * cable is pulled the BT link is already down (single-transport device), so this is a pure wake —
 * no closeConnection needed. Skip if somehow already connected (idempotent / spurious-fire safe). */
static void wake_bt_mt2(void) {
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        int n = 0;
        for (IOBluetoothDevice *d in devs) {
            if ([d getClassOfDevice] != MT2_COD) continue;
            if ([d isConnected]) { n++; continue; }   /* already up — nothing to wake */
            IOReturn ro = [d openConnection];          /* synchronous baseband re-establish */
            NSLog(@"mt2_usb_bt_handoff: USB removed -> openConnection %@ -> 0x%08x", [d addressString], ro);
            n++;
        }
        if (!n) NSLog(@"mt2_usb_bt_handoff: USB removed but no paired CoD-0x594 MT2 found");
    }
}

/* kIOTerminatedNotification callback: our USB reader instance(s) just went away = cable pulled. */
static void usb_reader_terminated(void *refcon, io_iterator_t it) {
    (void)refcon;
    io_service_t s; BOOL any = NO;
    while ((s = IOIteratorNext(it))) { any = YES; IOObjectRelease(s); }   /* must drain to re-arm */
    if (any) wake_bt_mt2();
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        /* One-shot mode: wake the deep-idle BT MT2 exactly once and exit — NO notification, NO run
         * loop. Reuses the same proven wake_bt_mt2 primitive. Used post-update (after the kext
         * reload) so the trackpad resumes without a physical tap. The single openConnection is
         * synchronous and returns after the BT page timeout (~5s) if the device is unreachable, so
         * this can't hang; a paired+powered device wakes near-instantly, and unpaired/off-connected
         * cases are skipped/error-return with no loop. */
        for (int i = 1; i < argc; i++) {
            if (argv[i] && strcmp(argv[i], "--wake-once") == 0) {
                wake_bt_mt2();
                return 0;
            }
        }

        IONotificationPortRef np = IONotificationPortCreate(kIOMasterPortDefault);
        if (!np) { NSLog(@"mt2_usb_bt_handoff: IONotificationPortCreate failed"); return 1; }
        CFRunLoopAddSource(CFRunLoopGetCurrent(),
                           IONotificationPortGetRunLoopSource(np), kCFRunLoopDefaultMode);

        io_iterator_t it = 0;
        kern_return_t kr = IOServiceAddMatchingNotification(
            np, kIOTerminatedNotification,
            IOServiceMatching("com_schmonz_MT2USBReader"),
            usb_reader_terminated, NULL, &it);
        if (kr != KERN_SUCCESS) {
            NSLog(@"mt2_usb_bt_handoff: IOServiceAddMatchingNotification failed 0x%x", kr);
            return 1;
        }
        /* Drain the initial iterator to ARM the notification. For a termination notification this
         * is empty at startup (nothing has terminated yet), so draining here — NOT via the callback
         * — guarantees we never fire a spurious wake just because USB happens to be plugged in now. */
        io_service_t s; while ((s = IOIteratorNext(it))) IOObjectRelease(s);

        NSLog(@"mt2_usb_bt_handoff: armed — watching com_schmonz_MT2USBReader termination");
        CFRunLoopRun();
    }
    return 0;
}
