/* mt2_usb_bt_handoff — no-click USB→BT handoff (the "I unplug the cable and it just keeps working" bit).
 *
 * When the MT2 is on USB, cabling drops its Bluetooth link (single-transport device,
 * docs/mt-stack + [[mt2-single-transport-at-a-time]]). Unplug the cable and the BT radio is
 * deep-idle: today it takes a physical CLICK to wake, so there's a dead gap. PROVEN on-device
 * 2026-07-04: a host `-[IOBluetoothDevice openConnection]` wakes that deep-idle radio with ZERO
 * physical action — the same primitive tools/mt2_bt_bounce uses for the reload case.
 *
 * This daemon closes the gap: it arms the shared presence observer (src/mt2_presence_observer.h — the
 * same one the prefpane uses) and, on the USB-removal edge (cable pulled), calls `openConnection` on
 * the paired CoD-0x594 MT2. No tap. Runs as a root LaunchDaemon so it works regardless of GUI session
 * (dist/com.schmonz.mt2usbbthandoff.plist). The observer watches the canonical AppleUSBMultitouchDriver
 * terminate, which fires within ~0.04 ms of the old com_schmonz_MT2USBReader edge (measured 2026-07-09).
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
#include "mt2_presence_observer.h"
#include "mt2_cod_match.h"   /* mt2_cod_is_mt2 — device-class match that tolerates live service bits */

/* Wake the deep-idle BT MT2 the same way mt2_bt_bounce does: a plain openConnection. When the USB
 * cable is pulled the BT link is already down (single-transport device), so this is a pure wake —
 * no closeConnection needed. Skip if somehow already connected (idempotent / spurious-fire safe). */
static void wake_bt_mt2(void) {
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        int n = 0;
        for (IOBluetoothDevice *d in devs) {
            if (!mt2_cod_is_mt2([d getClassOfDevice])) continue;
            if ([d isConnected]) { n++; continue; }   /* already up — nothing to wake */
            IOReturn ro = [d openConnection];          /* synchronous baseband re-establish */
            NSLog(@"mt2_usb_bt_handoff: USB removed -> openConnection %@ -> 0x%08x", [d addressString], ro);
            n++;
        }
        if (!n) NSLog(@"mt2_usb_bt_handoff: USB removed but no paired CoD-0x594 MT2 found");
    }
}

/* Post-update/post-reload BOUNCE: a full closeConnection -> openConnection on the MT2, forcing the BT
 * link to re-open BOTH L2CAP channels (PSM 17 control + 19 interrupt) so our reader re-attaches and
 * re-sends the multitouch enable. Distinct from wake_bt_mt2 (a plain openConnection): after a kext
 * reload the baseband link is STILL UP but the multitouch stream has stalled, so openConnection no-ops
 * with kBluetoothHCIErrorACLConnectionAlreadyExists (0x0b) and the user still has to click. A bounce
 * ALWAYS re-establishes, which resumes the stream with no tap. Same primitive as tools/mt2_bt_bounce
 * (proven safe for the reload case, [[mt2-reload-async-teardown-collision]]). */
static void bounce_bt_mt2(void) {
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        int n = 0;
        for (IOBluetoothDevice *d in devs) {
            if (!mt2_cod_is_mt2([d getClassOfDevice])) continue;
            n++;
            if ([d isConnected]) {
                IOReturn rc = [d closeConnection];       /* synchronous: returns after the link is down */
                NSLog(@"mt2_usb_bt_handoff: bounce closeConnection %@ -> 0x%08x", [d addressString], rc);
            }
            IOReturn ro = [d openConnection];            /* re-open -> re-run HID matching + our enable */
            NSLog(@"mt2_usb_bt_handoff: bounce openConnection %@ -> 0x%08x", [d addressString], ro);
        }
        if (!n) NSLog(@"mt2_usb_bt_handoff: bounce — no paired CoD-0x594 MT2 found");
    }
}

/* Shared presence-observer callback. We need the raw USB-removal EDGE (cable pulled), not the SM's
 * rendered action — a HOLD action can't tell a USB drop from a BT drop — so we key on the event.
 * This is the exact trigger the old ad-hoc com_schmonz_MT2USBReader-terminate notifier fired on;
 * the canonical AppleUSBMultitouchDriver terminate the observer watches fires within ~0.04 ms of it
 * (measured on-device 2026-07-09). ASYMMETRIC BY DESIGN preserved: only USB_REMOVE wakes BT. */
static void on_presence(presence_action_t action, presence_event_t event, void *ctx) {
    (void)action; (void)ctx;
    if (event == PRESENCE_EV_USB_REMOVE) wake_bt_mt2();
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
            /* Post-update: the link is up but the multitouch stream stalled across the kext reload, so a
             * plain wake no-ops. Bounce (close+reopen) to force the re-establish + re-enable. */
            if (argv[i] && strcmp(argv[i], "--bounce-once") == 0) {
                bounce_bt_mt2();
                return 0;
            }
        }

        /* Arm the shared presence observer (same one the prefpane uses). It drives the presence SM
         * and reports each transition; we act only on the USB_REMOVE edge (see on_presence). The
         * observer drains its initial iterators without firing, so no spurious wake at startup just
         * because USB happens to be plugged in now. */
        mt2_presence_observer_t *obs =
            presence_observer_create(CFRunLoopGetCurrent(), 1300, on_presence, NULL);
        if (!obs) { NSLog(@"mt2_usb_bt_handoff: presence_observer_create failed"); return 1; }

        NSLog(@"mt2_usb_bt_handoff: armed via shared presence observer (wakes BT on USB removal)");
        CFRunLoopRun();
    }
    return 0;
}
