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
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
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

/* Called from the tick with the window root. state==PSM_USB: read (throttled 30 s) + paint.
 * Leaving USB: ->NONE re-hides the row (NoTrackpad must not show battery); ->BT just stops
 * painting (Apple's own BT render owns the row again and repaints real BT values). */
static void usb_battery_tick(id root) {
    static time_t lastRead = 0;
    static int pct = -1, charging = 0;
    if (gPsm != PSM_USB) {
        if (gUsbBattPainted && gPsm == PSM_NONE) {
            id ctl = NULL, pf = NULL, sl = NULL;
            find_battery_row(root, 0, &ctl, &pf, &sl);
            id vs[3] = { ctl, pf, sl };
            for (int i = 0; i < 3; i++) if (vs[i])
                ((void (*)(id, SEL, signed char))objc_msgSend)(vs[i], sel_registerName("setHidden:"), 1);
            LOG("usb-battery: row re-hidden (left USB -> NoTrackpad)");
        }
        if (gPsm != PSM_HOLD) { gUsbBattPainted = 0; lastRead = 0; }   /* HOLD: keep display through the window */
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
    LOG("tree %s%s hidden=%d val=%.2f \"%s\"", pad, cn, hid, fv, txt ? txt : "");
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
    if (root && access("/tmp/mt2_pane_dump", F_OK) == 0) {
        unlink("/tmp/mt2_pane_dump");
        LOG("tree dump BEGIN (flag /tmp/mt2_pane_dump)");
        dump_view_tree(root, 0);
        LOG("tree dump END");
    }
}

/* Swizzled _checkBatteryTimer:: let the pane update the level/label as usual, then re-hide the
 * Change-Batteries button (it re-shows it at 0%). Caveat: hides for any trackpad the pane shows; on
 * this stack the connected device is always the MT2 (a co-connected genuine MT1 with AA batteries
 * would be a rare exception — future refinement could gate on the MT2 CoD minor 0x25). */
static void my_checkBatteryTimer(id self, SEL _cmd, id timer) {
    if (gOrigCheckBatteryTimer) gOrigCheckBatteryTimer(self, _cmd, timer);
    hide_change_battery_button(self);
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
    if (!gOrigCheckBatteryTimer) {
        Class c = objc_getClass("MTTrackpadController");
        SEL sel = sel_registerName("_checkBatteryTimer:");
        Method m = c ? class_getInstanceMethod(c, sel) : NULL;
        if (m) {
            gOrigCheckBatteryTimer = (void (*)(id, SEL, id))method_getImplementation(m);
            method_setImplementation(m, (IMP)my_checkBatteryTimer);
            LOG("swizzled MTTrackpadController _checkBatteryTimer: (hide Change Batteries — sealed MT2 battery)");
        } else {
            LOG("MTTrackpadController _checkBatteryTimer: not found (button-hide disabled)");
        }
    }
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
