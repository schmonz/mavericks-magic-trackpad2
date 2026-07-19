/* mt2_usb_bt_handoff — resident BT connection-keeper for our trackpad: keeps the paired MT2 connected
 * with no physical tap. Pages it (openConnection) on three triggers: a repeating timer (login-screen
 * wake at boot + "power it on later and it just connects"), and the USB-removal edge (unplug -> BT).
 * (Rename to a broader name is a deferred follow-up, gated on the reboot acid test.) Original note:
 * no-click USB→BT handoff (the "I unplug the cable and it just keeps working" bit).
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
#include "mt2_reconnect_policy.h"   /* mt2_reconnect_should_page — page iff ours-by-class AND disconnected */

/* Serial queue for ALL paging. openConnection blocks ~5s on page timeout when the device is off,
 * so it must never run on the main runloop (which drives the presence observer). Serialized so
 * overlapping triggers (timer, USB edge) never page concurrently. */
static dispatch_queue_t reconnect_queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("com.schmonz.mt2.reconnect", DISPATCH_QUEUE_SERIAL); });
    return q;
}

/* Actuator: page every paired device that is ours-by-class AND not already connected. Idempotent
 * (skips connected devices -> a quiet no-op when nothing needs waking) and safe on any trigger.
 * `why` labels the trigger in the log. Generalizes the old wake_bt_mt2. */
static void reconnect_matched(const char *why) {
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        for (IOBluetoothDevice *d in devs) {
            if (!mt2_reconnect_should_page([d getClassOfDevice], [d isConnected])) continue;
            IOReturn ro = [d openConnection];   /* synchronous baseband re-establish */
            NSLog(@"mt2_usb_bt_handoff: [%s] openConnection %@ -> 0x%08x", why, [d addressString], ro);
        }
    }
}

/* Fire the actuator off the main runloop (used by edge/timer triggers). */
static void reconnect_matched_async(const char *why) {
    dispatch_async(reconnect_queue(), ^{ reconnect_matched(why); });
}

/* Test/debug only: drop the link on our matched device(s) WITHOUT re-opening — leaves the device
 * powered but disconnected, so --reconnect-once and the timer can be exercised deterministically. */
static void disconnect_matched(const char *why) {
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        for (IOBluetoothDevice *d in devs) {
            if (!mt2_cod_is_mt2([d getClassOfDevice])) continue;
            if (![d isConnected]) continue;
            IOReturn rc = [d closeConnection];
            NSLog(@"mt2_usb_bt_handoff: [%s] closeConnection %@ -> 0x%08x", why, [d addressString], rc);
        }
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
    if (event == PRESENCE_EV_USB_REMOVE) reconnect_matched_async("usb-remove");
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
                reconnect_matched("wake-once");
                return 0;
            }
            /* Post-update: the link is up but the multitouch stream stalled across the kext reload, so a
             * plain wake no-ops. Bounce (close+reopen) to force the re-establish + re-enable. */
            if (argv[i] && strcmp(argv[i], "--bounce-once") == 0) {
                bounce_bt_mt2();
                return 0;
            }
            if (argv[i] && strcmp(argv[i], "--reconnect-once") == 0) {
                reconnect_matched("reconnect-once");
                return 0;
            }
            if (argv[i] && strcmp(argv[i], "--disconnect-once") == 0) {
                disconnect_matched("disconnect-once");
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
