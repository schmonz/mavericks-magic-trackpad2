/* mt2_linkstated — the Magic Trackpad 2's transport link-state coordinator. A resident root LaunchDaemon
 * that keeps the device's link in the state the CURRENT transport calls for. The MT2 is one device with
 * two transports but TWO IOKit driver instances (MT2USBReader / MT2BTReader) — so a transport switch is a
 * DEVICE switch, and needs re-init the reader-swap alone doesn't do. This daemon reads transport truth from
 * the shared presence SM (mavericks_presence via the observer, the same one the prefpane uses) and reacts
 * to the presence edges — five jobs (mostly BT actions, plus the USB gesture-open):
 *
 *   RECONNECT (page BT) — while we're NOT on USB and the paired MT2 is BT-disconnected, a 15s keeper timer
 *                       `openConnection`s it: the login-screen wake at boot + "power it on an hour later
 *                       and it just connects". openConnection wakes even a deep-idle radio with no tap
 *                       (proven 2026-07-04; same primitive as tools/mt2_bt_bounce). GATED OFF on USB — a
 *                       page that wins mid-USB flips the single-transport device off its own cable and
 *                       kills the USB stream (the periodic cursor stalls; see mt2_reconnect_policy.h).
 *   HANDOFF (page BT) — on the USB-removal edge, page BT at once so unplugging the cable "just keeps
 *                       working" with no tap. ASYMMETRIC BY DESIGN: only USB->BT needs help; BT->USB
 *                       (plug in) is smooth and the device handles it — we add nothing there.
 *   YIELD (close BT)  — on the USB-appear edge, close any BT link so we never hold the device off the
 *                       cable — covers the TOCTOU where the cable goes in during a ~5s page.
 *   GESTURE-KICK (hidd) — on the USB-appear edge, `killall hidd` so a fresh hidd opens the frames client
 *                       (AppleMultitouchDeviceUserClient) for the just-appeared USB AMD. Without it a
 *                       MID-SESSION BT->USB switch has cursor+gestures DEAD (only kernel 2-finger click
 *                       works) — the whole multitouch path needs the frames client open, and only a fresh
 *                       USB BOOT gets a kick (from voodooinputmavericks-run); the switch never re-kicked.
 *                       Root-caused + fixed 2026-07-21. (The observer drains initial iterators without
 *                       firing, so this fires on a LIVE USB appear/switch, NOT at boot — no double-kick.)
 *   QUIESCE (stop BT) — on restart/power-off, cancel the keeper + drain the paging queue so the BT
 *                       controller is idle for the OS teardown AND the next boot's EFI Bluetooth init.
 *                       Without it a warm restart inherited a mid-page controller and HUNG before video
 *                       (EFIBluetoothDelay makes firmware wait on BT); root-caused + fixed 2026-07-20.
 *
 * All BT action targets CoD-0x594 paired devices only (mt2_cod_match.h), so a co-connected non-MT2 Apple
 * device is never touched. Root LaunchDaemon (dist/dev.modernmavericks.voodooinputmavericks.linkstated.plist)
 * so it runs regardless of GUI session.
 *
 *   clang -fobjc-arc -mmacosx-version-min=10.9 -framework Foundation -framework IOBluetooth \
 *         -framework IOKit -o mt2_linkstated tools/mt2_linkstated.m
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
    dispatch_once(&once, ^{ q = dispatch_queue_create("dev.modernmavericks.mt2.reconnect", DISPATCH_QUEUE_SERIAL); });
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
            IOReturn ro = [d openConnection];   /* synchronous baseband re-establish (blocks ~5s) */
            NSLog(@"mt2_linkstated: [%s] openConnection %@ -> 0x%08x", why, [d addressString], ro);
        }
    }
}

/* Fire the actuator off the main runloop (used by edge/timer triggers). */
static void reconnect_matched_async(const char *why) {
    dispatch_async(reconnect_queue(), ^{ reconnect_matched(why); });
}

/* Drop the BT link on our matched device(s) WITHOUT re-opening. Two uses: the --disconnect-once CLI
 * (exercise the keeper deterministically) and the USB-appear yield (close any BT link the single-
 * transport device must not hold once it's on the cable). Idempotent: skips non-matching + already-
 * disconnected devices. Never touches BT during shutdown quiesce. */
static void disconnect_matched(const char *why) {
    if (g_terminating) return;
    @autoreleasepool {
        NSArray *devs = [IOBluetoothDevice pairedDevices];
        for (IOBluetoothDevice *d in devs) {
            if (!mt2_cod_is_mt2([d getClassOfDevice])) continue;
            if (![d isConnected]) continue;
            IOReturn rc = [d closeConnection];
            NSLog(@"mt2_linkstated: [%s] closeConnection %@ -> 0x%08x", why, [d addressString], rc);
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
                NSLog(@"mt2_linkstated: bounce closeConnection %@ -> 0x%08x", [d addressString], rc);
            }
            IOReturn ro = [d openConnection];            /* re-open -> re-run HID matching + our enable */
            NSLog(@"mt2_linkstated: bounce openConnection %@ -> 0x%08x", [d addressString], ro);
        }
        if (!n) NSLog(@"mt2_linkstated: bounce — no paired CoD-0x594 MT2 found");
    }
}

/* Shared presence-observer callback. We need the raw USB-removal EDGE (cable pulled), not the SM's
 * rendered action — a HOLD action can't tell a USB drop from a BT drop — so we key on the event.
 * This is the exact trigger the old ad-hoc com_schmonz_MT2USBReader-terminate notifier fired on;
 * the canonical AppleUSBMultitouchDriver terminate the observer watches fires within ~0.04 ms of it
 * (measured on-device 2026-07-09). ASYMMETRIC BY DESIGN preserved: only USB_REMOVE wakes BT. */
/* USB gesture-open: kick hidd so a fresh instance opens the frames client
 * (AppleMultitouchDeviceUserClient) for the just-appeared USB AMD. Needed on a MID-SESSION BT->USB switch:
 * a fresh USB boot gets this kick from voodooinputmavericks-run, but the switch never re-kicked, leaving
 * cursor AND gestures dead (only kernel 2-finger click works) because the whole USB multitouch path needs
 * the frames client open (proven on-device 2026-07-21). killall is blunt (one momentary all-HID hiccup)
 * but it's the only thing that reliably opens the client for an AMD that appeared into a running hidd
 * (docs/mt-stack/open-questions.md). Skipped during shutdown quiesce. */
static void kick_hidd(const char *why) {
    if (g_terminating) return;
    @try {
        [NSTask launchedTaskWithLaunchPath:@"/usr/bin/killall" arguments:@[@"hidd"]];
        NSLog(@"mt2_linkstated: [%s] kicked hidd to open the frames client for the USB AMD", why);
    } @catch (NSException *e) {
        NSLog(@"mt2_linkstated: [%s] hidd kick skipped: %@", why, e);
    }
}

static void on_presence(presence_action_t action, presence_event_t event, void *ctx) {
    (void)action; (void)ctx;
    if (event == PRESENCE_EV_USB_REMOVE) reconnect_matched_async("usb-remove");   /* unplug -> wake BT */
    /* USB just came up (cable in, from BT-idle OR from off). The MT2 is single-transport, so we must hold
     * NO BT link. YIELD closes the TOCTOU the point-in-time usb_present check can't: if the cable went in
     * DURING a ~5s openConnection, that page may have opened/attempted a BT link. Enqueue it on the SAME
     * serial paging queue so it runs AFTER any in-flight page. THEN GESTURE-KICK: let the USB AMD register
     * (reader->mux->shell->AMD is ~sub-second), then kick hidd so it opens the frames client.
     *
     * This is now the SOLE gesture-kicker. It fires on a LIVE appear the observer didn't drain as an
     * initial iterator — both a mid-session BT->USB switch AND a cold USB boot (there the wrapper's
     * mt2_reenumerate reclaim detaches/reattaches interface 1, which is a fresh appear). The wrapper no
     * longer kicks: two kickers made two hidd kills ~1s apart -> the second cut off the first respawn ->
     * a ~9s all-HID freeze (measured 2026-07-21). A won-on-load boot has USB present when we arm, so it's
     * drained (no appear, no kick) — correct, since won-on-load means gestures already work.
     *
     * KICK_DELAY: margin for the AMD to finish registering before we kick. On-device the AMD's
     * "started + registered" lands sub-second after the appear (measured 2026-07-21), so 1s clears it
     * with margin. Was 2s; trimmed with the wrapper's settle as pure slack over the observed timing. */
    else if (event == PRESENCE_EV_USB_APPEAR) {
        static const int64_t KICK_DELAY_SEC = 1;   /* AMD registers sub-second; 1s = margin, not padding */
        dispatch_async(reconnect_queue(), ^{ disconnect_matched("usb-appeared"); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, KICK_DELAY_SEC * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{ kick_hidd("usb-appeared"); });
    }
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
    NSLog(@"mt2_linkstated: quiesced BT for %s — keeper stopped, controller left idle for warm restart", why);
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
        if (!g_obs) { NSLog(@"mt2_linkstated: presence_observer_create failed"); return 1; }
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
            NSLog(@"mt2_linkstated: WARNING IORegisterForSystemPower failed — warm-restart BT quiesce unavailable");
        }

        /* SIGTERM (launchctl unload + the shutdown daemon-reap): quiesce then stop the runloop for a clean
         * exit. Ignore the default disposition so the GCD source is the sole handler. */
        signal(SIGTERM, SIG_IGN);
        dispatch_source_t termSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(termSrc, ^{ quiesce_for_shutdown("sigterm"); CFRunLoopStop(CFRunLoopGetCurrent()); });
        dispatch_resume(termSrc);

        NSLog(@"mt2_linkstated: armed — %ds reconnect keeper + USB handoff/yield/hidd-kick + shutdown quiesce", RECONNECT_CADENCE_SEC);
        CFRunLoopRun();
    }
    return 0;
}
