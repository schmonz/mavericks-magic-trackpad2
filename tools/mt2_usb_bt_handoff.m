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
 * This daemon closes the gap: it arms the shared presence observer (src/mavericks_presence_observer.h — the
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
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <IOKit/IOMessage.h>
#include <string.h>
#include <signal.h>
#include "mavericks_presence_observer.h"
#include "mt2_cod_match.h"   /* mt2_cod_is_mt2 — device-class match that tolerates live service bits */
#include "mt2_reconnect_policy.h"   /* mt2_reconnect_should_page — page iff ours-by-class AND disconnected */

#define RECONNECT_CADENCE_SEC 15          /* how often the keeper re-attempts while disconnected; tunable */
static dispatch_source_t g_reconnect_timer;   /* file-static: keep the timer alive for the process lifetime */

/* Shutdown/restart quiesce (root-caused 2026-07-20): the keeper's synchronous openConnection blocks ~5s
 * paging an absent MT2, keeping the Broadcom BT controller busy. On a WARM restart that mid-page state
 * carried into EFI's boot-time Bluetooth init (the EFIBluetoothDelay NVRAM setting makes firmware wait on
 * BT) and HUNG before video — a cold power-cycle was the only recovery (twice, 2026-07-20). g_terminating
 * makes the keeper get out of the BT controller's way the instant the system begins to power off/restart. */
/* Single-writer latch: set only on the main thread (SIGTERM source + power callback are serialized there),
 * read on the serial paging queue. volatile sig_atomic_t gives the needed visibility without C11 atomics
 * (absent from the 10.9 toolchain). The dispatch_sync barrier in quiesce_for_shutdown publishes it. */
static volatile sig_atomic_t g_terminating = 0;
static io_connect_t g_pm_root = MACH_PORT_NULL;   /* IORegisterForSystemPower root-port for IOAllowPowerChange */
static mavericks_presence_observer_t *g_obs;   /* the shared presence SM: single source of "are we on USB?" */

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
    if (g_terminating) return;   /* shutdown/restart in progress — never touch the BT controller */
    /* Ask the shared presence SM whether we're currently on USB. If so, DO NOT page BT: the MT2 is
     * single-transport, so a successful BT page flips it off USB and kills the stream (the periodic USB
     * cursor stalls, root-caused 2026-07-20). g_obs is NULL in the one-shot CLI modes (no observer) ->
     * PRESENCE_NONE -> usb_present=0 -> those deliberate wakes/bounces page as before. */
    int usb_present = (presence_observer_state(g_obs) == PRESENCE_USB);
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        for (IOBluetoothDevice *d in devs) {
            if (!mt2_reconnect_should_page([d getClassOfDevice], [d isConnected], usb_present)) continue;
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

/* Quiesce the keeper for a power-off/restart: stop new pages (cancel the timer + latch g_terminating) and
 * BARRIER-drain the serial paging queue so any in-flight openConnection (bounded by the ~5s page timeout)
 * has finished before we return. That leaves the BT controller idle for the OS's own clean teardown and,
 * critically, for the next warm boot's EFI Bluetooth init. Idempotent; safe to call from SIGTERM or the
 * system-power callback. */
static void quiesce_for_shutdown(const char *why) {
    if (g_terminating) return;   /* already quiescing (both callers run serialized on the main thread) */
    g_terminating = 1;
    if (g_reconnect_timer) dispatch_source_cancel(g_reconnect_timer);
    dispatch_sync(reconnect_queue(), ^{});               /* wait out any in-flight page (~5s max) */
    NSLog(@"mt2_usb_bt_handoff: quiesced BT for %s — keeper stopped, controller left idle for warm restart", why);
}

/* System power callback (IORegisterForSystemPower). The MT2 box has EFIBluetoothDelay set, so firmware
 * touches the BT controller on every boot; we MUST hand it over clean. Quiesce on the will-restart /
 * will-power-off edges (our bug is warm restart); allow every transition promptly so we never stall
 * shutdown/sleep. Sleep is intentionally NOT terminal (the keeper must survive wake). */
static void pm_callback(void *ctx, io_service_t svc, natural_t msgType, void *msgArg) {
    (void)ctx; (void)svc;
    switch (msgType) {
        case kIOMessageSystemWillRestart:   quiesce_for_shutdown("will-restart");   IOAllowPowerChange(g_pm_root, (long)msgArg); break;
        case kIOMessageSystemWillPowerOff:  quiesce_for_shutdown("will-power-off");  IOAllowPowerChange(g_pm_root, (long)msgArg); break;
        case kIOMessageCanSystemPowerOff:
        case kIOMessageCanSystemSleep:
        case kIOMessageSystemWillSleep:     IOAllowPowerChange(g_pm_root, (long)msgArg); break;   /* not our bug; don't delay it */
        default: break;
    }
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
        g_obs = presence_observer_create(CFRunLoopGetCurrent(), 1300, on_presence, NULL);
        if (!g_obs) { NSLog(@"mt2_usb_bt_handoff: presence_observer_create failed"); return 1; }
        /* The observer drains its initial iterators WITHOUT firing, so an already-present USB reader at
         * launch produces no USB_APPEAR edge -> the SM would sit at PRESENCE_NONE and the keeper would
         * page BT off USB. Reconcile once against live truth so the SM knows we're on USB from the start. */
        presence_observer_reconcile(g_obs);

        /* Connection-keeper: a repeating timer on the paging queue re-attempts our matched device
         * whenever it might be reachable — the login-screen wake at boot (first fire ~1s after launch,
         * retried until the controller is up and the device answers) AND the "power the trackpad on an
         * hour later and it just connects" case. Idempotent, so it's a quiet no-op while connected. */
        g_reconnect_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, reconnect_queue());
        dispatch_source_set_timer(g_reconnect_timer,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)NSEC_PER_SEC),          /* first fire ~1s after launch */
            (uint64_t)RECONNECT_CADENCE_SEC * NSEC_PER_SEC,                   /* then every cadence */
            (uint64_t)2 * NSEC_PER_SEC);                                      /* leeway */
        dispatch_source_set_event_handler(g_reconnect_timer, ^{ reconnect_matched("timer"); });
        dispatch_resume(g_reconnect_timer);

        /* Register for system power notifications so we quiesce Bluetooth BEFORE the machine powers off or
         * restarts (the warm-restart EFI hang fix). Add its port to the same runloop the observer uses. */
        IONotificationPortRef pmPort = NULL;
        io_object_t pmNotifier = 0;
        g_pm_root = IORegisterForSystemPower(NULL, &pmPort, pm_callback, &pmNotifier);
        if (g_pm_root != MACH_PORT_NULL && pmPort) {
            CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(pmPort), kCFRunLoopCommonModes);
        } else {
            NSLog(@"mt2_usb_bt_handoff: WARNING IORegisterForSystemPower failed — warm-restart BT quiesce unavailable");
        }

        /* SIGTERM (launchctl unload + the shutdown daemon-reap): quiesce then stop the runloop for a clean
         * exit. Ignore the default disposition so the GCD source is the sole handler. */
        signal(SIGTERM, SIG_IGN);
        dispatch_source_t termSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(termSrc, ^{ quiesce_for_shutdown("sigterm"); CFRunLoopStop(CFRunLoopGetCurrent()); });
        dispatch_resume(termSrc);

        NSLog(@"mt2_usb_bt_handoff: armed — %ds reconnect keeper + USB-removal wake + shutdown quiesce", RECONNECT_CADENCE_SEC);
        CFRunLoopRun();
    }
    return 0;
}
