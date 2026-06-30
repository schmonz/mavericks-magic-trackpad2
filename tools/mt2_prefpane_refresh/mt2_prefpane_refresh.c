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
#include "mt2_pane_sm.h"

#define LOG(...) syslog(LOG_NOTICE, "[MT2PaneRefresh] " __VA_ARGS__)

static id              gPane       = NULL;   /* the live Trackpad pane instance */
static IONotificationPortRef gNotify = NULL;
static io_iterator_t   gIterUSBup  = 0;   /* AppleUSBMultitouchDriver first-match */
static io_iterator_t   gIterUSBdn  = 0;   /* AppleUSBMultitouchDriver terminated  */
static io_iterator_t   gIterBTup   = 0;   /* BNBTrackpadDevice first-match         */
static io_iterator_t   gIterBTdn   = 0;   /* BNBTrackpadDevice terminated          */
static int             gArmed      = 0;
static psm_state_t     gPsm = PSM_NONE;   /* the SM state; the adapter's only decision state */
/* The original MTTrackpadController _magicTrackpadAction: IMP (we own the selector; see
 * my_magicAction). perform() calls it via gOrigMagicAction_call() to set battery state. */
static void (*gOrigMagicAction)(id, SEL, id, signed char) = NULL;

typedef void (*didSelect_t)(id, SEL);
static didSelect_t     gOrigDidSelect = NULL;

/* --- small objc helpers (no Obj-C syntax) --- */
static int responds(id obj, SEL s) {
    return obj && ((signed char (*)(id, SEL, SEL))objc_msgSend)(
        obj, sel_registerName("respondsToSelector:"), s);
}

static int service_present(const char *cls) {
    CFMutableDictionaryRef m = IOServiceMatching(cls);
    if (!m) return 0;
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, m);  /* consumes m */
    if (s) { IOObjectRelease(s); return 1; }
    return 0;
}

/* Is the pane currently showing the trackpad view (vs the no-device "NoTrackpad" view)?
 * The controller is a generic InputDeviceNibController for every view, so its class can't
 * tell them apart; the reliable signal is nibFileName — the no-device controller answers
 * "NoTrackpad", while the trackpad controller returns nil. So: trackpad iff != "NoTrackpad". */
static int current_view_is_trackpad(void) {
    Ivar iv = class_getInstanceVariable(object_getClass(gPane), "mCurrentController");
    id ctrl = iv ? object_getIvar(gPane, iv) : NULL;
    if (!ctrl) return 0;
    id nib = ((id (*)(id, SEL))objc_msgSend)(ctrl, sel_registerName("nibFileName"));
    if (!nib) return 1;
    return !((signed char (*)(id, SEL, id))objc_msgSend)(
        nib, sel_registerName("isEqualToString:"), (id)CFSTR("NoTrackpad"));
}

/* Perform one SM action against the live pane. RENDER_* drives Apple's own controller update:
 * loadMainView only when the view type must change (NoTrackpad<->Trackpad), else just set the
 * battery via the real _magicTrackpadAction(connected). We OWN that selector (see my_magicAction),
 * so this is the only place it fires. */
static void gOrigMagicAction_call(int connected);   /* fwd decl; defined with the swizzle */
static void perform(psm_action_t a) {
    if (!gPane) return;
    switch (a) {
    case PSM_ACT_RENDER_NONE:
        if (current_view_is_trackpad()) {
            SEL lmv = sel_registerName("loadMainView");
            if (responds(gPane, lmv)) ((id (*)(id, SEL))objc_msgSend)(gPane, lmv);
        }
        LOG("perform: RENDER_NONE (NoTrackpad)");
        break;
    case PSM_ACT_RENDER_BT:
    case PSM_ACT_RENDER_USB: {
        if (!current_view_is_trackpad()) {
            SEL lmv = sel_registerName("loadMainView");
            if (responds(gPane, lmv)) ((id (*)(id, SEL))objc_msgSend)(gPane, lmv);
        }
        gOrigMagicAction_call(a == PSM_ACT_RENDER_BT ? 1 : 0);
        LOG("perform: %s", a == PSM_ACT_RENDER_BT ? "RENDER_BT" : "RENDER_USB");
        break;
    }
    case PSM_ACT_HOLD: LOG("perform: HOLD (keep view; arm window)"); break;
    case PSM_ACT_NONE: break;
    }
}

static int gGen = 0;
#define REMOVE_CHECK_MS 1300

/* Run one event through the SM and perform its action on the main thread. gGen serializes the
 * pending HOLD timer: entering HOLD arms a window timer tagged with `my`; any LATER resolving
 * event bumps gGen so the in-flight timer's `my != gGen` guard fires and it no-ops (superseded).
 * A no-op event (PSM_ACT_NONE — a duplicate/stale edge) must NOT bump gGen: doing so would cancel
 * a live HOLD timer without rescheduling, stranding the SM in PSM_HOLD (reconcile won't resolve
 * HOLD->NONE by design). So only HOLD arms, and only a real resolution supersedes. */
static void sm_event(psm_event_t e) {
    psm_result_t r = psm_step(gPsm, e);
    gPsm = r.next;
    perform(r.action);
    if (r.action == PSM_ACT_HOLD) {
        int my = ++gGen;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)REMOVE_CHECK_MS * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            if (my != gGen) return;
            psm_result_t rr = psm_step(gPsm, PSM_EV_REMOVAL_ELAPSED);
            gPsm = rr.next; perform(rr.action);
        });
    } else if (r.action != PSM_ACT_NONE) {
        ++gGen;   /* a real resolution supersedes a pending hold timer; a no-op leaves it alone */
    }
}

/* Reconcile against device truth (poll): self-heal for missed/manually-started edges. */
static void sm_reconcile(void) {
    int bt  = service_present("BNBTrackpadDevice");
    int usb = service_present("AppleUSBMultitouchDriver");
    psm_result_t r = psm_reconcile(gPsm, bt, usb);
    if (r.action != PSM_ACT_NONE) LOG("reconcile bt=%d usb=%d -> action", bt, usb);
    gPsm = r.next;
    perform(r.action);
}

static void drain(io_iterator_t it) {
    io_object_t o;
    while ((o = IOIteratorNext(it))) IOObjectRelease(o);
}

/* One callback for every appear/disappear of either transport. The pane's own
 * updates are suppressed (see my_deviceConnected), so WE are the sole driver: each edge becomes
 * one SM event (sm_event), and the SM decides what to render. refcon labels the event for logs. */
static void dev_changed(void *ref, io_iterator_t it) {
    drain(it);
    const char *tag = ref ? (const char *)ref : "?";
    LOG("device change: %s", tag);
    if      (!strcmp(tag, "BT+"))  sm_event(PSM_EV_BT_APPEAR);
    else if (!strcmp(tag, "BT-"))  sm_event(PSM_EV_BT_REMOVE);
    else if (!strcmp(tag, "USB+")) sm_event(PSM_EV_USB_APPEAR);
    else if (!strcmp(tag, "USB-")) sm_event(PSM_EV_USB_REMOVE);
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
 * recognized). Our observers (dev_changed) drive the SM, which owns every render, so the pane
 * holds its prior state through a switch, then updates once. */
static void (*gOrigDeviceConnected)(id, SEL, id, signed char) = NULL;  /* saved, intentionally never called */
static void my_deviceConnected(id self, SEL _cmd, id obs, signed char connected) {
    (void)self; (void)_cmd; (void)obs; (void)connected;
}

/* We OWN this selector: the controller's autonomous calls are suppressed unconditionally (like we
 * own _ioServiceObserver). The SM decides battery state; perform() calls the original via
 * gOrigMagicAction_call(). This removes the power-off battery-hide race entirely. */
static void my_magicAction(id self, SEL _cmd, id arg, signed char connected) {
    (void)self; (void)_cmd; (void)arg; (void)connected;   /* suppress the autonomous call entirely */
}

/* Find the LIVE MTTrackpadController. Primary: mCurrentController.mController (the current view's
 * content controller). Fallback: [MTTrackpadController sharedController]. (RE 2026-06-30 PROBE.) */
static id find_mt_controller(void) {
    if (!gPane) return NULL;
    Class mt = objc_getClass("MTTrackpadController");
    SEL isk = sel_registerName("isKindOfClass:");
    Ivar iv = class_getInstanceVariable(object_getClass(gPane), "mCurrentController");
    id cur = iv ? object_getIvar(gPane, iv) : NULL;
    if (cur) {
        Ivar mc = class_getInstanceVariable(object_getClass(cur), "mController");
        id c = mc ? object_getIvar(cur, mc) : NULL;
        if (c && mt && ((signed char (*)(id, SEL, Class))objc_msgSend)(c, isk, mt)) return c;
    }
    if (mt && ((signed char (*)(id, SEL, SEL))objc_msgSend)(
            (id)mt, sel_registerName("respondsToSelector:"), sel_registerName("sharedController"))) {
        id sc = ((id (*)(id, SEL))objc_msgSend)((id)mt, sel_registerName("sharedController"));
        if (sc && ((signed char (*)(id, SEL, Class))objc_msgSend)(sc, isk, mt)) return sc;
    }
    return NULL;
}

static void gOrigMagicAction_call(int connected) {
    if (!gOrigMagicAction || !gPane) return;
    id ctrl = find_mt_controller();
    if (!ctrl) { LOG("gOrigMagicAction_call: no MTTrackpadController found"); return; }
    gOrigMagicAction(ctrl, sel_registerName("_magicTrackpadAction:deviceConnected:"),
                     ctrl, (signed char)(connected ? 1 : 0));
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
    if (!gOrigMagicAction) {
        /* The in-place battery-hide lives on MTTrackpadController (the runtime owner of
         * _magicTrackpadAction:deviceConnected:; the binary symbol mislabels it
         * "BaseTrackPadController", but its body uses MTTrackpadController ivars and the live
         * runtime confirms MTTrackpadController owns the IMP). */
        Class c = objc_getClass("MTTrackpadController");
        SEL sel = sel_registerName("_magicTrackpadAction:deviceConnected:");
        Method m = c ? class_getInstanceMethod(c, sel) : NULL;
        if (m) {
            gOrigMagicAction = (void (*)(id, SEL, id, signed char))method_getImplementation(m);
            method_setImplementation(m, (IMP)my_magicAction);
            LOG("swizzled MTTrackpadController _magicTrackpadAction:deviceConnected: (own selector; suppress autonomous)");
        } else {
            LOG("MTTrackpadController _magicTrackpadAction: not found (suppression disabled) getClass=%p", c);
        }
    }
    arm_observer();
    sm_reconcile();   /* sync to current truth immediately */
    static dispatch_source_t tick;
    if (!tick) {
        tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                                  2 * NSEC_PER_SEC, (uint64_t)(0.25 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(tick, ^{ sm_reconcile(); });
        dispatch_resume(tick);
    }
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
