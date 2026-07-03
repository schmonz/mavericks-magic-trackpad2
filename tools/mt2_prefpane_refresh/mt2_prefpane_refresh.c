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
#include <time.h>
#include <mach-o/dyld.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>
#include "mt2_pane_sm.h"
#include "mt2_single_load.h"

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
/* The exact (self=controller, arg) pair the pane hands _magicTrackpadAction, captured from the
 * autonomous call (see my_magicAction). The original body does [arg armIterators] (arg = the pane's
 * iterator owner, disasm 0x4caf: `mov %r14,%rdi` where r14=rdx=arg); replaying with these real objects
 * renders correctly. Task 5's regression passed the CONTROLLER as arg -> [controller armIterators] ->
 * doesNotRecognizeSelector -> no render. Both are stable for the pane's life, so any capture is reusable;
 * only `connected` varies (the SM decides it). */
static id gMagicCtrl = NULL;   /* self: the trackpad controller */
static id gMagicArg  = NULL;   /* arg:  the object [arg armIterators] targets (the pane) */

typedef void (*didSelect_t)(id, SEL);
static didSelect_t     gOrigDidSelect = NULL;
static didSelect_t     gOrigWillSelect = NULL;
static void mt2_activate(const char *via);  /* fwd decl; THE single activation choke point */

/* Original MTTrackpadController _checkBatteryTimer: — we call through then force the "Change
 * Batteries" button hidden (the MT2 has a sealed rechargeable battery; that AA-era control never
 * applies, yet the pane re-shows it every tick at 0%). */
static void (*gOrigCheckBatteryTimer)(id, SEL, id) = NULL;

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

/* Launch the shared updater helper (Sparkle host) on demand. Fixed absolute path — one shared copy,
 * same for the osax and SIMBL routes (see the single-load choke point). Phase 4 wires this to the
 * pane's "Check for Updates" control; unused until then. `open` returns immediately; the helper runs
 * its own UI. */
static void mt2_launch_updater(void) __attribute__((unused));
static void mt2_launch_updater(void) {
    const char *app = "/usr/local/lib/mt2d/MavericksTrackpad2Updater.app";
    if (access(app, F_OK) != 0) { LOG("updater: %s not installed", app); return; }
    pid_t pid = fork();
    if (pid == 0) { execl("/usr/bin/open", "open", app, (char *)NULL); _exit(127); }
    LOG("updater: launched %s", app);
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
static void install_device_icon_swizzle(const char *src);  /* fwd decl; device-tied BT-pane icon */
static id (*gOrigDeviceIcon)(id, SEL) = NULL;        /* saved -[IOBluetoothDevice deviceIcon] */
static id (*gOrigGetDeviceIcon)(id, SEL) = NULL;     /* saved -[IOBluetoothDevice getDeviceIcon] */
static id (*gOrigImage)(id, SEL) = NULL;             /* saved -[IOBluetoothDevice image] (the row icon accessor) */
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
        /* Rebuild the view only on a real change to/from the trackpad view (e.g. leaving NoTrackpad).
         * A BT<->USB switch stays on the trackpad view, so the faithful replay below does the in-place
         * transport update — no loadMainView, no movie-reload blink. */
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

/* We OWN this selector: suppress the controller's autonomous render (the SM owns WHEN we replay, via
 * gOrigMagicAction_call). Capture the (self, arg) the pane passes so the replay is FAITHFUL — the body
 * does [arg armIterators], so arg must be the pane's iterator owner, never the controller. */
static void my_magicAction(id self, SEL _cmd, id arg, signed char connected) {
    (void)_cmd; (void)connected;
    if (self && (self != gMagicCtrl || arg != gMagicArg)) {
        gMagicCtrl = self; gMagicArg = arg;
        LOG("captured magic (self=%s, arg=%s armIterators=%d)",
            object_getClassName(self), object_getClassName(arg),
            (int)((signed char (*)(id, SEL, SEL))objc_msgSend)(
                arg, sel_registerName("respondsToSelector:"), sel_registerName("armIterators")));
    }
}

/* Replay the pane's OWN _magicTrackpadAction faithfully: the captured (self=controller, arg) with the
 * SM's `connected`. The body does [arg armIterators], so arg MUST be the pane object the runtime handed
 * us — passing the controller there was the doesNotRecognizeSelector regression. Until a device event has
 * let us capture the pair, skip (bootstrap renders natively via loadMainView on a view change). */
static void gOrigMagicAction_call(int connected) {
    if (!gOrigMagicAction || !gPane || !gMagicCtrl || !gMagicArg) {
        LOG("gOrigMagicAction_call: (self,arg) not captured yet -> skip replay");
        return;
    }
    gOrigMagicAction(gMagicCtrl, sel_registerName("_magicTrackpadAction:deviceConnected:"),
                     gMagicArg, (signed char)(connected ? 1 : 0));
}

/* Force the pane's "Change Batteries" button hidden. The MT2 charges over Lightning (sealed
 * battery), so the AA-era swap prompt never applies — yet the pane RE-SHOWS it every
 * _checkBatteryTimer tick whenever the level reads 0% (setHidden:(pct>0.0), threshold 0.0; disasm
 * Trackpad.prefPane @0x4bdf). mChangeBatteryButton is an MTTrackpadController ivar. */
static void hide_change_battery_button(id controller) {
    if (!controller) return;
    Ivar iv = class_getInstanceVariable(object_getClass(controller), "mChangeBatteryButton");
    id btn = iv ? object_getIvar(controller, iv) : NULL;
    if (btn) {
        signed char was = ((signed char (*)(id, SEL))objc_msgSend)(btn, sel_registerName("isHidden"));
        ((void (*)(id, SEL, signed char))objc_msgSend)(
            btn, sel_registerName("setHidden:"), (signed char)1);
        if (!was) LOG("hid Change-Batteries button %p on %s", btn, object_getClassName(controller));
    } else {
        static int warned = 0;
        if (!warned) { warned = 1;
            LOG("mChangeBatteryButton ivar not found on %s (iv=%p)", object_getClassName(controller), iv); }
    }
}

/* Capture-free fallback: recursively walk a view tree and hide the Change-Batteries NSButton.
 * Matched by ACTION SELECTOR (locale-independent); first walk logs every button's action+title so
 * the criterion is verifiable in syslog. Needed because (a) _checkBatteryTimer is one-shot on a
 * valid read (invalidates its fast timer; disasm 0x4b45/0x4c15) so the swizzle rarely re-fires, and
 * (b) the button ivar lives on MTTrackpadController, which we may never be handed on a quiet open. */
static int gLoggedButtons = 0;
static void walk_hide_battery_button(id view, int depth) {
    if (!view || depth > 12) return;
    Class btnCls = objc_getClass("NSButton");
    if (btnCls && ((signed char (*)(id, SEL, Class))objc_msgSend)(
            view, sel_registerName("isKindOfClass:"), btnCls)) {
        SEL action = ((SEL (*)(id, SEL))objc_msgSend)(view, sel_registerName("action"));
        const char *an = action ? sel_getName(action) : "(none)";
        id title = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("title"));
        const char *tn = title ? ((const char *(*)(id, SEL))objc_msgSend)(
                                     title, sel_registerName("UTF8String")) : "";
        if (!gLoggedButtons)
            LOG("button scan: action=%s title=\"%s\"", an, tn ? tn : "");
        /* Primary: the button's real action selector (ground truth from the live scan:
         * "lowBatteryButton:") — locale-independent. Title match kept as a fallback. */
        if (strcmp(an, "lowBatteryButton:") == 0 || (tn && strstr(tn, "Change Batter"))) {
            signed char was = ((signed char (*)(id, SEL))objc_msgSend)(
                view, sel_registerName("isHidden"));
            ((void (*)(id, SEL, signed char))objc_msgSend)(
                view, sel_registerName("setHidden:"), (signed char)1);
            if (!was) LOG("hid Change-Batteries button (action=%s)", an);
        }
    }
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    if (!subs) return;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++)
        walk_hide_battery_button(((id (*)(id, SEL, unsigned long))objc_msgSend)(
            subs, sel_registerName("objectAtIndex:"), i), depth + 1);
}

/* ---- USB battery paint ----------------------------------------------------------------------
 * Apple's USB Trackpad view HIDES the battery row (BatteryControl + percent + static label,
 * hidden=1 in the window bottom bar) and shows "Set Up Bluetooth Trackpad…" — but the MT2 reports
 * battery over USB too (Power-Device report 0x90, byte[1]&1 = charging, byte[2] = %; readable from
 * userspace via IOHIDDeviceGetReport — proven by tools/mt2_battery_probe.c). So when the SM state
 * is USB we unhide Apple's own row and feed it ourselves: charging progress on the cable. */

/* Read the MT2 battery via HID (any transport; on the USB view the USB interface answers).
 * Returns 1 and fills pct/charging on success. Fresh manager per read (every ~30 s) so cable
 * cycles never leave a stale device ref. kIOHIDOptionsTypeNone — never seizes the device. */
static int mt2_usb_read_battery(int *pct, int *charging) {
    int got = 0;
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) return 0;
    IOHIDManagerSetDeviceMatching(mgr, NULL);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (devs) {
        CFIndex n = CFSetGetCount(devs);
        IOHIDDeviceRef stack[64];
        if (n > 0 && n <= 64) {
            CFSetGetValues(devs, (const void **)stack);
            for (CFIndex i = 0; i < n && !got; i++) {
                CFNumberRef pidRef = (CFNumberRef)IOHIDDeviceGetProperty(stack[i], CFSTR(kIOHIDProductIDKey));
                int pid = 0;
                if (pidRef) CFNumberGetValue(pidRef, kCFNumberIntType, &pid);
                if (pid != 0x0265) continue;                     /* MT2 (USB vid 05ac / BT vid 004c) */
                if (IOHIDDeviceOpen(stack[i], kIOHIDOptionsTypeNone) != kIOReturnSuccess) continue;
                uint8_t b[16] = {0}; CFIndex L = sizeof b;
                if (IOHIDDeviceGetReport(stack[i], kIOHIDReportTypeInput, 0x90, b, &L)
                        == kIOReturnSuccess && L >= 3 && b[0] == 0x90 && b[2] <= 100) {
                    *pct = b[2]; *charging = (b[1] & 0x01) != 0; got = 1;
                }
                IOHIDDeviceClose(stack[i], kIOHIDOptionsTypeNone);
            }
        }
        CFRelease(devs);
    }
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return got;
}

/* Find the battery row: the view whose class name contains "BatteryControl", plus its two
 * NSTextField siblings that FOLLOW it in subview order (percent field, then the static
 * "Trackpad battery level:" label — order verified by the live tree dump both transports). Walked
 * fresh each paint — never cached (System Prefs runs ObjC GC; a stale cached view would dangle). */
static void find_battery_row(id view, int depth, id *ctl, id *pctField, id *staticLbl) {
    if (!view || depth > 14 || *ctl) return;
    if (strstr(object_getClassName(view), "BatteryControl")) {
        *ctl = view;
        id sup = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("superview"));
        id subs = sup ? ((id (*)(id, SEL))objc_msgSend)(sup, sel_registerName("subviews")) : NULL;
        unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
        Class tfCls = objc_getClass("NSTextField");
        int seen = 0;
        for (unsigned long i = 0; i < n; i++) {
            id v = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
            if (!seen) { if (v == view) seen = 1; continue; }
            if (tfCls && ((signed char (*)(id, SEL, Class))objc_msgSend)(
                    v, sel_registerName("isKindOfClass:"), tfCls)) {
                if (!*pctField) *pctField = v;
                else if (!*staticLbl) { *staticLbl = v; break; }
            }
        }
        return;
    }
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    if (!subs) return;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count"));
    for (unsigned long i = 0; i < n && !*ctl; i++)
        find_battery_row(((id (*)(id, SEL, unsigned long))objc_msgSend)(
            subs, sel_registerName("objectAtIndex:"), i), depth + 1, ctl, pctField, staticLbl);
}

static int gUsbBattPainted = 0;   /* we unhid the row for USB (so we re-hide it on -> NoTrackpad) */
static int gStripPending  = 0;   /* our "NN% (charging)" USB text is on the field -> strip it once BT
                                  * shows the battery row. Survives a USB->NoTrackpad->BT hop (unlike
                                  * gUsbBattPainted, which we clear on every leave-USB tick). */

/* Called from the tick with the window root. state==PSM_USB: read (throttled 30 s) + paint.
 * Leaving USB: ->NONE re-hides the row (NoTrackpad must not show battery); ->BT strips our
 * "NN% (charging)" USB text down to a clean "NN%". Apple does NOT re-render the row on a LIVE
 * transport switch (only on a full load), so our charging text would linger otherwise. The strip is
 * keyed on gStripPending (not gUsbBattPainted) so it also fires on the usual USB->NoTrackpad->BT path
 * — where the battery field only exists once BT brings the trackpad view back — and is retried each
 * tick until that field appears. On BT the device is on battery, never charging, so "NN%" is right. */
static void usb_battery_tick(id root) {
    static time_t lastRead = 0;
    static int pct = -1, charging = 0;
    if (gPsm != PSM_USB) {
        if (gPsm == PSM_HOLD) return;                /* keep the display through the removal window */
        id ctl = NULL, pf = NULL, sl = NULL;
        find_battery_row(root, 0, &ctl, &pf, &sl);
        if (gPsm == PSM_NONE) {
            /* NoTrackpad must not show a battery row; re-hide the one we unhid for USB. (Can't strip
             * the '(charging)' text here — the field lives only in the trackpad view; gStripPending
             * carries it to the BT case below.) */
            if (gUsbBattPainted) {
                id vs[3] = { ctl, pf, sl };
                for (int i = 0; i < 3; i++) if (vs[i])
                    ((void (*)(id, SEL, signed char))objc_msgSend)(vs[i], sel_registerName("setHidden:"), 1);
                LOG("usb-battery: row re-hidden (left USB -> NoTrackpad)");
            }
        } else if (gStripPending && pf && pct >= 0) {   /* -> BT: strip the '(charging)' residue */
            char label[48];
            snprintf(label, sizeof(label), "%d%%", pct);
            CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, label, kCFStringEncodingUTF8);
            if (s) {
                ((void (*)(id, SEL, id))objc_msgSend)(pf, sel_registerName("setStringValue:"), (id)s);
                CFRelease(s);
                ((void (*)(id, SEL))objc_msgSend)(pf, sel_registerName("sizeToFit"));
            }
            if (ctl) ((void (*)(id, SEL, float))objc_msgSend)(ctl, sel_registerName("setFloatValue:"), (float)pct / 100.0f);
            gStripPending = 0;
            LOG("usb-battery: stripped '(charging)' residue on BT (%s)", label);
        }
        gUsbBattPainted = 0; lastRead = 0;   /* gStripPending persists until the BT strip above clears it */
        return;
    }
    time_t now = time(NULL);
    if (!lastRead || now - lastRead >= 30) {
        int p, c;
        if (mt2_usb_read_battery(&p, &c)) {
            if (p != pct || c != charging) LOG("usb-battery: read %d%% charging=%d", p, c);
            pct = p; charging = c;
        }
        lastRead = now;
    }
    if (pct < 0) return;                                   /* nothing read yet — leave Apple's UI alone */
    id ctl = NULL, pf = NULL, sl = NULL;
    find_battery_row(root, 0, &ctl, &pf, &sl);
    if (!ctl || !pf || !sl) return;
    ((void (*)(id, SEL, float))objc_msgSend)(ctl, sel_registerName("setFloatValue:"), (float)pct / 100.0f);
    char label[48];
    snprintf(label, sizeof(label), charging ? "%d%% (charging)" : "%d%%", pct);
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, label, kCFStringEncodingUTF8);
    if (s) {
        ((void (*)(id, SEL, id))objc_msgSend)(pf, sel_registerName("setStringValue:"), (id)s);
        CFRelease(s);
        /* The nib sizes the field for "100%"; widen so " (charging)" isn't clipped (grows right,
         * into the empty gap before the Set Up button). */
        ((void (*)(id, SEL))objc_msgSend)(pf, sel_registerName("sizeToFit"));
    }
    id vs[3] = { ctl, pf, sl };
    for (int i = 0; i < 3; i++)
        ((void (*)(id, SEL, signed char))objc_msgSend)(vs[i], sel_registerName("setHidden:"), 0);
    if (!gUsbBattPainted) { gUsbBattPainted = 1; LOG("usb-battery: row painted (%s)", label); }
    gStripPending = 1;   /* our text is on the field; strip it when we next reach BT (any path) */
}

/* On-demand RE aid: `touch /tmp/mt2_pane_dump` -> the next tick dumps the whole window view tree
 * (class, hidden, frame, and any title/stringValue) to syslog, once, then removes the flag. Used to
 * map the battery-row controls in each transport state (e.g. what exists on the USB view). */
static void dump_view_tree(id view, int depth) {
    if (!view || depth > 14) return;
    char pad[32]; int p = depth < 15 ? depth * 2 : 30;
    memset(pad, ' ', (size_t)p); pad[p] = 0;
    const char *cn = object_getClassName(view);
    signed char hid = ((signed char (*)(id, SEL))objc_msgSend)(view, sel_registerName("isHidden"));
    const char *txt = "";
    SEL tsel = sel_registerName("title");
    if (!responds(view, tsel)) tsel = sel_registerName("stringValue");
    if (responds(view, tsel)) {
        id s = ((id (*)(id, SEL))objc_msgSend)(view, tsel);
        if (s) txt = ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String"));
    }
    double fv = -1;
    if (responds(view, sel_registerName("doubleValue")))
        fv = ((double (*)(id, SEL))objc_msgSend)(view, sel_registerName("doubleValue"));
    /* Image source hunt: any view/cell answering -image -> its NSImage name (or "<file/anon>"). */
    char imginfo[96]; imginfo[0] = 0;
    if (responds(view, sel_registerName("image"))) {
        id im = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("image"));
        if (im) {
            id nm = responds(im, sel_registerName("name"))
                  ? ((id (*)(id, SEL))objc_msgSend)(im, sel_registerName("name")) : NULL;
            const char *ns = nm ? ((const char *(*)(id, SEL))objc_msgSend)(nm, sel_registerName("UTF8String")) : NULL;
            snprintf(imginfo, sizeof(imginfo), " IMG=%s", ns ? ns : "<anon>");
        }
    }
    LOG("tree %s%s hidden=%d val=%.2f \"%s\"%s", pad, cn, hid, fv, txt ? txt : "", imginfo);
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    if (!subs) return;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++)
        dump_view_tree(((id (*)(id, SEL, unsigned long))objc_msgSend)(
            subs, sel_registerName("objectAtIndex:"), i), depth + 1);
}

/* Hide the button — called from our own 2s reconcile tick so it fires regardless of whether the
 * pane's battery timer re-runs. Walk from the pane's mainView (covers whichever controller/tab owns
 * the button right now). */
static void hide_battery_button_now(void) {
    if (!gPane) return;
    id mv = ((id (*)(id, SEL))objc_msgSend)(gPane, sel_registerName("mainView"));
    if (!mv) return;
    /* The battery row (level + Change Batteries) is NOT inside mainView — it sits in the window's
     * bottom bar (verified: a mainView-only scan finds just the gesture buttons). Walk the whole
     * window contentView; fall back to mainView if the pane isn't in a window yet. */
    id win = ((id (*)(id, SEL))objc_msgSend)(mv, sel_registerName("window"));
    id root = win ? ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("contentView")) : mv;
    if (root) { walk_hide_battery_button(root, 0); gLoggedButtons = 1; usb_battery_tick(root); }
    /* NB: the /tmp/mt2_pane_dump view-tree dump lives ONLY in the aux tick (front window) so it works
     * on any pane, incl. Bluetooth; a second trigger here would consume the flag first on the Trackpad
     * pane and dump the wrong window. */
}

/* Swizzled _checkBatteryTimer:: let the pane update the level/label as usual, then re-hide the
 * Change-Batteries button (it re-shows it at 0%). Caveat: hides for any trackpad the pane shows; on
 * this stack the connected device is always the MT2 (a co-connected genuine MT1 with AA batteries
 * would be a rare exception — future refinement could gate on the MT2 CoD minor 0x25). */
static void my_checkBatteryTimer(id self, SEL _cmd, id timer) {
    if (gOrigCheckBatteryTimer) gOrigCheckBatteryTimer(self, _cmd, timer);
    hide_change_battery_button(self);
}

/* Install the _checkBatteryTimer swizzle. Called EARLY from willSelect (before the pane's one-shot
 * battery timer fires, so our hook hides the Change-Batteries button in that same fire -> no flash)
 * and from capture_pane as a belt. MTTrackpadController exists once the Trackpad pane bundle loads. */
static void install_battery_timer_swizzle(const char *src) {
    if (gOrigCheckBatteryTimer) return;
    Class c = objc_getClass("MTTrackpadController");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("_checkBatteryTimer:")) : NULL;
    if (!m) return;
    gOrigCheckBatteryTimer = (void (*)(id, SEL, id))method_getImplementation(m);
    method_setImplementation(m, (IMP)my_checkBatteryTimer);
    LOG("swizzled MTTrackpadController _checkBatteryTimer: via %s (hide Change Batteries — sealed MT2)", src);
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
    install_battery_timer_swizzle("capture");   /* belt; willSelect installs it earlier (no flash) */
    arm_observer();
    sm_reconcile();   /* sync to current truth immediately */
    static dispatch_source_t tick;
    if (!tick) {
        tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                                  2 * NSEC_PER_SEC, (uint64_t)(0.25 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(tick, ^{ sm_reconcile(); hide_battery_button_now(); });
        dispatch_resume(tick);
    }
}

/* Swizzled -[NSPreferencePane willSelect] (fires BEFORE didSelect): install the image swizzle here,
 * before the pane populates its device list, so the MT2 row's first draw already reads our art (no
 * bowtie->trackpad flash). didSelect proved too late — the Bluetooth pane populates earlier. */
static void my_willSelect(id self, SEL _cmd) {
    install_device_icon_swizzle("willSelect");
    /* Trackpad pane: install the battery-timer swizzle BEFORE the pane's one-shot _checkBatteryTimer
     * fires, so the Change-Batteries button is hidden in that same fire (no flash at low battery). */
    const char *cn = object_getClassName(self);
    if (cn && strcmp(cn, "Trackpad") == 0) install_battery_timer_swizzle("willSelect");
    if (gOrigWillSelect) gOrigWillSelect(self, _cmd);
}

/* Swizzled -[NSPreferencePane didSelect]: call through, then capture if Trackpad. */
static void my_didSelect(id self, SEL _cmd) {
    install_device_icon_swizzle("didSelect");   /* belt, in case willSelect didn't fire/was late */
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

/* ---- Bluetooth-pane MT2 icon ------------------------------------------------------------------
 * The BT pane's device-row icon resolves via +[IOBluetoothDeviceImageVault imageForDevice:]
 * (IOBluetoothUI @0x18867) -> vault[major][minor]; the vault has NO trackpad entry, and the MT2's
 * CoD (major 5, minor 0x25) also fails the separate isPointingDevice path -> generic BT logo
 * (full RE: docs/mt-stack/explanation.md "Picture"). Fix: swizzle imageForDevice: to return
 * Apple's own trackpad art (Trackpad.prefPane/TrackpadPicture.png) for any (5,0x25) device — that
 * CoD IS a Magic Trackpad 2; a genuine MT1 (different minor) is untouched. IOBluetoothUI loads
 * LAZILY (only when the BT pane opens), so the installer is idempotent and driven from a dyld
 * add-image hook (which also replays already-loaded images at registration) + the tick as a belt.
 * System-Preferences-process only by design — the BT PANE is this deliverable; BluetoothUIServer /
 * the menu extra stay generic (separate multi-process delivery if ever wanted). */
/* Our MT2 art, prepared like the vault does for bundle-loaded images (scalable, 32x32 pt row). */
static id mt2_trackpad_image(void) {
    static id img = NULL;
    if (!img) {
        id a = ((id (*)(id, SEL))objc_msgSend)(
            (id)objc_getClass("NSImage"), sel_registerName("alloc"));
        a = ((id (*)(id, SEL, id))objc_msgSend)(a, sel_registerName("initWithContentsOfFile:"),
            (id)CFSTR("/System/Library/PreferencePanes/Trackpad.prefPane/Contents/Resources/TrackpadPicture.png"));
        if (a) {
            /* Natural size + scalable; the row's NSImageView scales it to fit (like Apple's own
             * device art, which is 512x512). */
            ((void (*)(id, SEL, signed char))objc_msgSend)(a, sel_registerName("setScalesWhenResized:"), 1);
            CFRetain(a);   /* pin under ObjC GC — a C static is not a GC root */
            img = a;
            LOG("bt-icon: TrackpadPicture.png loaded for MT2 (major 5, minor 0x25)");
        }
    }
    return img;
}

/* The Bluetooth pane's "Devices" list is a VIEW-BASED NSTableView: each row is an NSTableCellView
 * with a direct-child NSImageView holding the device icon (Magic Mouse gets a mouse image; the MT2
 * gets the generic BT logo because its CoD (5,0x25) misses the IOBluetoothDeviceImageVault — RE:
 * the pane never calls the vault's major/minor path, so a vault swizzle is inert). We instead set
 * the MT2 row's NSImageView.image directly to Apple's trackpad art — same "own the real view"
 * approach as the battery row. Re-asserted each aux tick (the pane repopulates on device edges). */

/* Does this cell's subtree contain an NSTextField reading "Magic Trackpad 2" — i.e. is it the MT2
 * device row? (Apple's product-database name; the cell's objectValue is a dict, not the device, so
 * we match the visible row text.) Used only to locate the row for the one-shot binding re-read. */
static int cell_is_mt2(id v, int depth) {
    if (!v || depth > 6) return 0;
    Class tf = objc_getClass("NSTextField");
    if (tf && ((signed char (*)(id, SEL, Class))objc_msgSend)(v, sel_registerName("isKindOfClass:"), tf)) {
        id s = ((id (*)(id, SEL))objc_msgSend)(v, sel_registerName("stringValue"));
        const char *cs = s ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : NULL;
        if (cs && strstr(cs, "Magic Trackpad 2")) return 1;
    }
    id subs = ((id (*)(id, SEL))objc_msgSend)(v, sel_registerName("subviews"));
    unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < n; i++)
        if (cell_is_mt2(((id (*)(id, SEL, unsigned long))objc_msgSend)(
                subs, sel_registerName("objectAtIndex:"), i), depth + 1)) return 1;
    return 0;
}

/* The image swizzle may install AFTER the pane already bound the row icons (the osax loads once the
 * pane is up), so the MT2 row shows the stale original the binding cached (reloadData + KVO pokes
 * don't re-fire it). Fix: re-establish the image view's "value" binding to the SAME device-tied
 * keypath (objectValue.device.deviceIcon) — that forces a fresh read, now through our swizzle -> our
 * art. Not per-view painting: we reuse the pane's own binding + the device's own deviceIcon; from
 * here Apple re-reads deviceIcon (our value) on every future refresh, so it stays correct. One-shot. */
static int gIconRebound = 0;
static void kvo_refresh_mt2_icon(id view, int depth) {
    if (!view || depth > 16 || gIconRebound || !gOrigDeviceIcon) return;
    Class cellCls = objc_getClass("NSTableCellView");
    if (cellCls && ((signed char (*)(id, SEL, Class))objc_msgSend)(view, sel_registerName("isKindOfClass:"), cellCls)
        && cell_is_mt2(view, 0)) {
        id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
        unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
        Class ivCls = objc_getClass("NSImageView");
        for (unsigned long i = 0; i < n; i++) {
            id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
            if (!(ivCls && ((signed char (*)(id, SEL, Class))objc_msgSend)(sv, sel_registerName("isKindOfClass:"), ivCls)))
                continue;
            ((void (*)(id, SEL, id))objc_msgSend)(sv, sel_registerName("unbind:"), (id)CFSTR("value"));
            ((void (*)(id, SEL, id, id, id, id))objc_msgSend)(sv, sel_registerName("bind:toObject:withKeyPath:options:"),
                (id)CFSTR("value"), view, (id)CFSTR("objectValue.device.deviceIcon"), (id)NULL);
            gIconRebound = 1;
            LOG("bt-icon: rebound MT2 row icon (fresh read of device.image via swizzle)");
            return;
        }
    }
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < n; i++)
        kvo_refresh_mt2_icon(((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i), depth + 1);
}

/* The front window's contentView (any pane, incl. Bluetooth — gPane only tracks Trackpad). */
static id front_window_content(void) {
    Class app = objc_getClass("NSApplication");
    if (!app) return NULL;
    id nsapp = ((id (*)(Class, SEL))objc_msgSend)(app, sel_registerName("sharedApplication"));
    id win = ((id (*)(id, SEL))objc_msgSend)(nsapp, sel_registerName("mainWindow"));
    if (!win) win = ((id (*)(id, SEL))objc_msgSend)(nsapp, sel_registerName("keyWindow"));
    /* mainWindow/keyWindow are nil when System Prefs is backgrounded (our terminal has focus), which
     * was silently disabling the icon paint + dump. Fall back to the first visible window so both work
     * regardless of foreground — the pane's icon must hold even while another app is active. */
    if (!win) {
        id wins = ((id (*)(id, SEL))objc_msgSend)(nsapp, sel_registerName("windows"));
        unsigned long n = wins ? ((unsigned long (*)(id, SEL))objc_msgSend)(wins, sel_registerName("count")) : 0;
        for (unsigned long i = 0; i < n; i++) {
            id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
            if (((signed char (*)(id, SEL))objc_msgSend)(w, sel_registerName("isVisible"))) { win = w; break; }
        }
    }
    return win ? ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("contentView")) : NULL;
}

/* Always-on (not gated on Trackpad capture): keep the BT-icon vault swizzle installed, and service
 * the on-demand view-tree dump for WHATEVER pane is front (so the Bluetooth pane is coverable). */
static dispatch_source_t gAuxTick = NULL;
static void aux_tick_start(void) {
    if (gAuxTick) return;
    gAuxTick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(gAuxTick, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              (uint64_t)(1.5 * NSEC_PER_SEC), (uint64_t)(0.25 * NSEC_PER_SEC));
    dispatch_source_set_event_handler(gAuxTick, ^{
        install_device_icon_swizzle("auxtick");          /* belt: ensure the image swizzle is in */
        kvo_refresh_mt2_icon(front_window_content(), 0);  /* re-read if swizzled after the pane bound it */
        if (access("/tmp/mt2_pane_dump", F_OK) == 0) {
            unlink("/tmp/mt2_pane_dump");
            id root = front_window_content();
            if (root) { LOG("tree dump BEGIN (front window)"); dump_view_tree(root, 0); LOG("tree dump END"); }
        }
    });
    dispatch_resume(gAuxTick);
}

/* DEVICE-TIED icon fix: swizzle -[IOBluetoothDevice deviceIcon] to return the trackpad art for the
 * MT2 (CoD major 5 / minor 0x25). The BT pane binds the row icon to objectValue.device.deviceIcon
 * (RE'd), so overriding deviceIcon fixes it at the source — consistent in every view and every
 * connection state, exactly like Magic Mouse (whose deviceIcon returns a mouse). No per-view
 * painting. A genuine MT1 (different minor) is untouched. Installed the moment IOBluetooth loads (dyld
 * add-image hook) so it is active before the pane first reads deviceIcon. */
static id mt2_icon_for(id self, id (*orig)(id, SEL), SEL _cmd) {
    int mj = ((int (*)(id, SEL))objc_msgSend)(self, sel_registerName("getDeviceClassMajor"));
    int mn = ((int (*)(id, SEL))objc_msgSend)(self, sel_registerName("getDeviceClassMinor"));
    if (mj == 5 && mn == 0x25) { id img = mt2_trackpad_image(); if (img) return img; }
    return orig ? orig(self, _cmd) : NULL;
}
static id my_deviceIcon(id self, SEL _cmd)    { return mt2_icon_for(self, gOrigDeviceIcon, _cmd); }
static id my_getDeviceIcon(id self, SEL _cmd) { return mt2_icon_for(self, gOrigGetDeviceIcon, _cmd); }
static id my_image(id self, SEL _cmd)         { return mt2_icon_for(self, gOrigImage, _cmd); }
/* The row's icon binds to objectValue.device.deviceIcon, but the value it displays is
 * -[IOBluetoothDevice image] (the generic 512x512 for the MT2; RE'd via a method enumeration —
 * deviceIcon returns 243x243 but the binding shows `image`). Swizzle `image` (the real accessor) so
 * the MT2 shows our trackpad art device-tied, consistent in every view/state like Magic Mouse; also
 * swizzle deviceIcon/getDeviceIcon for any direct/KVC callers. All scoped to the MT2 CoD (5,0x25). */
static void install_device_icon_swizzle(const char *src) {
    if (gOrigImage) return;
    Class c = objc_getClass("IOBluetoothDevice");
    if (!c) return;                                  /* IOBluetooth not loaded yet */
    Method mi = class_getInstanceMethod(c, sel_registerName("image"));
    Method md = class_getInstanceMethod(c, sel_registerName("deviceIcon"));
    Method mg = class_getInstanceMethod(c, sel_registerName("getDeviceIcon"));
    if (mi) { gOrigImage = (id (*)(id, SEL))method_getImplementation(mi);
        method_setImplementation(mi, (IMP)my_image); }
    if (md) { gOrigDeviceIcon = (id (*)(id, SEL))method_getImplementation(md);
        method_setImplementation(md, (IMP)my_deviceIcon); }
    if (mg) { gOrigGetDeviceIcon = (id (*)(id, SEL))method_getImplementation(mg);
        method_setImplementation(mg, (IMP)my_getDeviceIcon); }
    if (mi) LOG("bt-icon: swizzled IOBluetoothDevice image/deviceIcon via %s (device-tied MT2 icon)", src);
}
static void mt2_dyld_added(const struct mach_header *mh, intptr_t slide) {
    (void)mh; (void)slide; install_device_icon_swizzle("dyld");
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
    /* Also swizzle willSelect (fires before didSelect) so the image swizzle is in before the Bluetooth
     * pane populates its device list — kills the first-open icon flash. */
    SEL wsel = sel_registerName("willSelect");
    Method wm = class_getInstanceMethod(c, wsel);
    if (wm && !gOrigWillSelect) {
        gOrigWillSelect = (didSelect_t)method_getImplementation(wm);
        method_setImplementation(wm, (IMP)my_willSelect);
        LOG("swizzled -[NSPreferencePane willSelect]");
    }
    install_device_icon_swizzle("inject");   /* device-tied BT-pane MT2 icon (if IOBluetooth already loaded) */
}

/* OSAX entry point named in Info.plist (OSAXHandlers Events MT2xload Handler). Put a MARKER in the
 * reply so the watcher can tell OUR handler ran (osax actually loaded) from System Prefs' default
 * handler, which ALSO returns noErr when our handler isn't registered yet (the reinstall load race).
 * Without this the watcher declares a false-positive "injected" and stops before the osax loads. */
#define MT2_INJECT_MARKER 0x4D543258   /* 'MT2X' */
OSErr MT2InjectHandler(const AppleEvent *evt, AppleEvent *reply, long refcon) {
    (void)evt; (void)refcon;
    LOG("inject handler invoked");
    mt2_activate("inject-handler");   /* same choke point as the constructor; inert if we lost the claim */
    if (reply && reply->descriptorType != typeNull) {
        SInt32 marker = MT2_INJECT_MARKER;
        AEPutParamPtr(reply, keyDirectObject, typeSInt32, &marker, sizeof(marker));
    }
    return noErr;
}

/* THE single activation choke point — the whole-payload owner decision, no per-swizzle granularity.
 * The FIRST payload image to reach here (the SIMBL plugin via [bundle load], or the osax via its
 * MT2InjectHandler) claims process-wide ownership via mt2_claim_single_load() and does EVERYTHING;
 * any other image that later reaches ANY entry point finds the claim taken and stays FULLY inert.
 * Idempotent within one image (gActivated). Because this is the only place activation lives and both
 * entry points funnel through it, a new swizzle/feature added here (or inside install_swizzle) is
 * automatically owned by exactly one image — nothing to remember to guard. */
static int gActivated = 0;
static void mt2_activate(const char *via) {
    if (gActivated) return;                    /* already active in THIS image (idempotent) */
    if (!mt2_claim_single_load()) {            /* another image already owns this process */
        LOG("payload: lost the single-load claim (via %s) — staying inert", via);
        return;
    }
    gActivated = 1;
    LOG("payload active in pid %d (via %s)", getpid(), via);
    install_swizzle();
    /* Device-tied BT-pane icon: install the deviceIcon swizzle the moment IOBluetooth loads (the hook
     * also replays already-loaded images), so it is in before the pane first reads deviceIcon. */
    _dyld_register_func_for_add_image(&mt2_dyld_added);
    /* Aux tick over the front window (works for any pane, incl. Bluetooth — gPane only tracks
     * Trackpad): swizzle belt + the on-demand /tmp/mt2_pane_dump view-tree dump. */
    dispatch_async(dispatch_get_main_queue(), ^{ aux_tick_start(); });
    /* Handle the direct-open case (already on Trackpad): retry a proactive capture
     * on the main queue once the app/pane are up. didSelect still handles navigation. */
    int delays[3] = {300, 1200, 3000};
    for (int i = 0; i < 3; i++) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delays[i] * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{ try_capture_current(); });
    }
}

/* Runs the instant our image loads into the host process (SIMBL [bundle load]). */
__attribute__((constructor))
static void mt2_image_loaded(void) {
    mt2_activate("constructor");
}
