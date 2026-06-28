/*
 * mt2_prefpane_refresh — a Scripting Addition (.osax) injected into System
 * Preferences to give the Trackpad pane the LIVE USB observer it lacks.
 *
 * WHY: the 10.9 Trackpad pane registers an IOServiceObserver only on
 * BNBTrackpadDevice (BT); AppleUSBMultitouchDriver (USB) is a one-shot presence
 * check. So an open pane misses a BT->USB switch (RE: docs/mt-stack/open-questions.md
 * "Trackpad prefpane live-update"). We add the missing USB observer and drive the
 * pane's own recompute on USB appear/terminate. We add ONLY the trigger; Apple's
 * pane does all rendering.
 *
 * GC-NEUTRAL: 10.9 System Preferences runs Obj-C garbage collection and refuses
 * to load any bundle carrying __objc_imageinfo. So this file is PURE C — all Cocoa
 * is done via the objc runtime (objc_msgSend / sel_registerName), never @interface
 * / @implementation / Obj-C literals. Logging is syslog() (libc), not NSLog, to
 * avoid linking Foundation. Do not introduce any Obj-C-compiled code.
 *
 * Build: clang -bundle -mmacosx-version-min=10.9 \
 *        -framework IOKit -framework CoreFoundation -lobjc
 * Loaded via the OSAXHandlers entry in Info.plist (event 'MT2x'/'load' ->
 * MT2InjectHandler), sent by tools/mt2_prefpane_refresh/mt2_pane_arm at Sys Prefs launch.
 */

#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

#define LOG(...) syslog(LOG_NOTICE, "[MT2PaneRefresh] " __VA_ARGS__)

static id              gPane       = NULL;   /* the live Trackpad pane instance */
static IONotificationPortRef gNotify = NULL;
static io_iterator_t   gIterUSBup  = 0;   /* AppleUSBMultitouchDriver first-match */
static io_iterator_t   gIterUSBdn  = 0;   /* AppleUSBMultitouchDriver terminated  */
static io_iterator_t   gIterBTup   = 0;   /* BNBTrackpadDevice first-match         */
static io_iterator_t   gIterBTdn   = 0;   /* BNBTrackpadDevice terminated          */
static int             gArmed      = 0;

typedef void (*didSelect_t)(id, SEL);
static didSelect_t     gOrigDidSelect = NULL;

/* --- small objc helpers (no Obj-C syntax) --- */
static int responds(id obj, SEL s) {
    return obj && ((signed char (*)(id, SEL, SEL))objc_msgSend)(
        obj, sel_registerName("respondsToSelector:"), s);
}

/* Re-run the pane's own full detection (Trackpad loadMainView @0x225d): resets
 * state, re-detects BT+USB+property+mouse, and updates the LIVE display. This is
 * what makes a BT-launched open pane track USB. */
static void do_recompute(void) {
    if (!gPane) return;
    SEL lmv = sel_registerName("loadMainView");
    if (responds(gPane, lmv)) {
        ((id (*)(id, SEL))objc_msgSend)(gPane, lmv);
        LOG("recompute: loadMainView (coalesced)");
    } else {
        LOG("pane does not respond to loadMainView");
    }
}

/* Schedule a single coalesced recompute after delay_ms, superseding any pending
 * one (generation counter). All on the main thread (IOKit source + main queue), so
 * no locking. The KEY to a flicker-free handoff is ASYMMETRIC timing (see dev_changed):
 * a transport REMOVAL waits out the device's one-transport-at-a-time gap, so the
 * following APPEAR supersedes it — the whole BT<->USB switch becomes ONE update with
 * no NoTrackpad in between. */
static int usb_device_present(void);   /* defined below; used by the removal settle-check */
static int gGen = 0;
#define APPEAR_DELAY_MS 250    /* show the newly-present transport promptly */
#define REMOVE_CHECK_MS 1300   /* > MT2 USB enum/registration latency (~1.1s); below this the
                                  incoming USB device isn't yet findable, so a handoff misreads as
                                  a removal and flashes NoTrackpad. This is the device's floor. */
static void schedule_update(int delay_ms, int removal) {
    int my = ++gGen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay_ms * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (my != gGen) return;   /* a later event superseded this one */
        /* For a removal, re-check at FIRE time (after settle): if the USB device has
         * since enumerated, this was a BT->USB handoff — hold and let the USB appear
         * show it (no NoTrackpad). Otherwise it's a genuine removal -> recompute. */
        if (removal && usb_device_present()) {
            LOG("removal settled with USB present -> hold for USB appear");
            return;
        }
        do_recompute();
    });
}

static void drain(io_iterator_t it) {
    io_object_t o;
    while ((o = IOIteratorNext(it))) IOObjectRelease(o);
}

/* Is the MT2's raw USB device present (cable inserted)? True as soon as the cable is
 * in — BEFORE AppleUSBMultitouchDriver matches — so on a transport removal we can tell
 * a genuine removal (no USB at all) from a BT->USB handoff (USB device already here,
 * its driver still coming). */
static int usb_device_present(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBDevice");
    if (!m) return 0;
    int vid = 1452, pid = 613;   /* Apple, Magic Trackpad 2 (USB) */
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    if (v) { CFDictionarySetValue(m, CFSTR("idVendor"),  v); CFRelease(v); }
    if (p) { CFDictionarySetValue(m, CFSTR("idProduct"), p); CFRelease(p); }
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, m);  /* consumes m */
    if (s) { IOObjectRelease(s); return 1; }
    return 0;
}

/* One callback for every appear/disappear of either transport. The pane's own
 * updates are suppressed (see my_deviceConnected), so WE are the sole driver: any
 * change schedules a single coalesced recompute. refcon labels the event for logs. */
static void dev_changed(void *ref, io_iterator_t it) {
    drain(it);
    const char *tag = ref ? (const char *)ref : "?";
    int appear = (tag[0] && tag[strlen(tag) - 1] == '+');
    /* APPEAR: show the new transport promptly. REMOVAL: defer a short settle, then
     * decide — if USB has enumerated by then it's a handoff (hold), else NoTrackpad. */
    LOG("device change: %s -> %s in %dms", tag, appear ? "show" : "removal-check",
        appear ? APPEAR_DELAY_MS : REMOVE_CHECK_MS);
    schedule_update(appear ? APPEAR_DELAY_MS : REMOVE_CHECK_MS, !appear);
}

/* Arm live IOKit observers on BOTH transports (BNBTrackpadDevice +
 * AppleUSBMultitouchDriver), FirstMatch + Terminated. IOServiceAddMatchingNotification
 * consumes the matching dict, so build a fresh one per call. Drain the initial
 * iterators WITHOUT acting (already-present services, not live events). */
static void arm_observer(void) {
    if (gArmed) return;
    gNotify = IONotificationPortCreate(kIOMasterPortDefault);
    if (!gNotify) { LOG("IONotificationPortCreate failed"); return; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(gNotify),
                       kCFRunLoopCommonModes);

    IOServiceAddMatchingNotification(gNotify, kIOFirstMatchNotification,
        IOServiceMatching("AppleUSBMultitouchDriver"), dev_changed, (void *)"USB+", &gIterUSBup);
    drain(gIterUSBup);
    IOServiceAddMatchingNotification(gNotify, kIOTerminatedNotification,
        IOServiceMatching("AppleUSBMultitouchDriver"), dev_changed, (void *)"USB-", &gIterUSBdn);
    drain(gIterUSBdn);
    IOServiceAddMatchingNotification(gNotify, kIOFirstMatchNotification,
        IOServiceMatching("BNBTrackpadDevice"), dev_changed, (void *)"BT+", &gIterBTup);
    drain(gIterBTup);
    IOServiceAddMatchingNotification(gNotify, kIOTerminatedNotification,
        IOServiceMatching("BNBTrackpadDevice"), dev_changed, (void *)"BT-", &gIterBTdn);
    drain(gIterBTdn);

    gArmed = 1;
    LOG("armed live observers (USB + BT, first-match + terminated)");
}

/* Suppress the pane's OWN observer-driven update ENTIRELY — we take ownership of
 * when the display updates. Without this the pane redraws on every BT edge (e.g.
 * blanks to NoTrackpad the instant BT drops during a BT->USB handoff, before USB is
 * recognized). Our observers (dev_changed) + a single coalesced loadMainView are the
 * sole driver, so the pane holds its prior state through a switch, then updates once. */
static void (*gOrigDeviceConnected)(id, SEL, id, signed char) = NULL;  /* saved, intentionally never called */
static void my_deviceConnected(id self, SEL _cmd, id obs, signed char connected) {
    (void)self; (void)_cmd; (void)obs; (void)connected;
}

/* Capture the live Trackpad pane: arm the USB observer + swizzle the pane class's
 * deviceConnected: for NoTrackpad suppression (the class is loaded by now). */
static void capture_pane(id self) {
    if (gPane == self) return;
    gPane = self;
    LOG("captured Trackpad pane");
    if (!gOrigDeviceConnected) {
        Class c = object_getClass(self);
        SEL sel = sel_registerName("_ioServiceObserver:deviceConnected:");
        Method m = class_getInstanceMethod(c, sel);
        if (m) {
            gOrigDeviceConnected = (void (*)(id, SEL, id, signed char))method_getImplementation(m);
            method_setImplementation(m, (IMP)my_deviceConnected);
            LOG("swizzled _ioServiceObserver:deviceConnected: (NoTrackpad suppression)");
        }
    }
    arm_observer();
}

/* Swizzled -[NSPreferencePane didSelect]: call through, then capture if Trackpad. */
static void my_didSelect(id self, SEL _cmd) {
    if (gOrigDidSelect) gOrigDidSelect(self, _cmd);
    const char *cn = object_getClassName(self);
    if (cn && strcmp(cn, "Trackpad") == 0) capture_pane(self);
}

/* Capture-race fix: if System Preferences was ALREADY showing Trackpad when we
 * injected (its didSelect fired before our swizzle), proactively ask it for the
 * current pane and capture it. System Preferences' controller exposes
 * -currentPrefPaneInstance (RE'd); it's reachable via the app delegate. Retried a few
 * times because the pane may not be loaded the instant we inject. */
static void try_capture_current(void) {
    if (gPane) return;
    Class appCls = objc_getClass("NSApplication");
    if (!appCls) return;
    id app = ((id (*)(id, SEL))objc_msgSend)((id)appCls, sel_registerName("sharedApplication"));
    if (!app) return;
    id delegate = ((id (*)(id, SEL))objc_msgSend)(app, sel_registerName("delegate"));
    SEL cur = sel_registerName("currentPrefPaneInstance");
    id cand[2]; cand[0] = delegate; cand[1] = app;
    for (int i = 0; i < 2; i++) {
        if (!responds(cand[i], cur)) continue;
        id pane = ((id (*)(id, SEL))objc_msgSend)(cand[i], cur);
        if (!pane) continue;
        const char *cn = object_getClassName(pane);
        if (cn && strcmp(cn, "Trackpad") == 0) {
            LOG("proactive capture via currentPrefPaneInstance (already on Trackpad)");
            capture_pane(pane);
        }
        return;
    }
}

static void install_swizzle(void) {
    Class c = objc_getClass("NSPreferencePane");
    if (!c) { LOG("no NSPreferencePane class"); return; }
    SEL sel = sel_registerName("didSelect");
    Method m = class_getInstanceMethod(c, sel);
    if (!m) { LOG("no -[NSPreferencePane didSelect]"); return; }
    if (gOrigDidSelect) return;                 /* already installed */
    gOrigDidSelect = (didSelect_t)method_getImplementation(m);
    method_setImplementation(m, (IMP)my_didSelect);
    LOG("swizzled -[NSPreferencePane didSelect]");
}

/* OSAX entry point named in Info.plist (OSAXHandlers Events MT2xload Handler). */
OSErr MT2InjectHandler(const AppleEvent *evt, AppleEvent *reply, long refcon) {
    (void)evt; (void)reply; (void)refcon;
    LOG("inject handler invoked");
    install_swizzle();
    return noErr;
}

/* Runs the instant our image loads into the host process (SIMBL [bundle load]).
 * Install the didSelect swizzle immediately; capture happens on the next select. */
__attribute__((constructor))
static void mt2_image_loaded(void) {
    LOG("image loaded into pid %d", getpid());
    install_swizzle();
    /* Handle the direct-open case (already on Trackpad): retry a proactive capture
     * on the main queue once the app/pane are up. didSelect still handles navigation. */
    int delays[3] = {300, 1200, 3000};
    for (int i = 0; i < 3; i++) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delays[i] * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{ try_capture_current(); });
    }
}
