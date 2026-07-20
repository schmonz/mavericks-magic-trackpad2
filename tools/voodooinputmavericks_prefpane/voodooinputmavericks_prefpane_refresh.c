/*
 * mavericks_prefpane_refresh — the payload injected into System Preferences that makes the
 * Trackpad + Bluetooth panes behave correctly for the Magic Trackpad 2 on 10.9. ONE
 * payload, shipped as a SIMBL plugin and loaded via its [bundle load] constructor.
 *
 * What it does, by concern — each is one numbered section below:
 *    1. Generic helpers          objc dispatch shims, front window, view-type probe
 *    2. Battery row              ONE renderer for both transports (USB HID read / BT node
 *                                BatteryPercent), painted into Apple's own control
 *    3. Magic-action replay      own _magicTrackpadAction so the SM decides WHEN to render
 *    4. State machine + observers live USB+BT IOKit observers -> SM -> one render owner
 *    5. Change-Batteries button  keep the sealed-battery AA swap prompt hidden
 *    6. Bluetooth-pane icon      device-tied MT2 trackpad art (swizzle deviceIcon/image)
 *    7. View-tree dump           on-demand RE aid (touch /tmp/mavericks_pane_dump)
 *    8. Aux tick                 front-window belt: icon swizzle + dump service
 *    9. Pane capture + nav       willSelect/didSelect swizzles; capture the live pane
 *   10. Updater launch           fire the shared Sparkle helper (Phase 4)
 *   11. Injection + activation   entry points funnel through ONE single-load choke point
 *
 * Sections are ordered leaf-first, so every definition precedes its use (no forward
 * declarations) and the file reads top-to-bottom; the entry points are last.
 *
 * WHY the pane needs us: the 10.9 Trackpad pane registers an IOServiceObserver only on
 * BNBTrackpadDevice (BT); AppleUSBMultitouchDriver (USB) is a one-shot presence check, so
 * an open pane misses a BT<->USB switch (RE: docs/mt-stack/open-questions.md). We add the
 * missing observers and OWN every render; Apple's pane still does all the actual drawing.
 *
 * GC-NEUTRAL: 10.9 System Preferences runs Obj-C garbage collection and refuses to load any
 * bundle carrying __objc_imageinfo. So this file is PURE C — all Cocoa is via the objc
 * runtime (objc_msgSend / sel_registerName), never @interface/@implementation/Obj-C literals.
 * Logging is syslog() (libc), not NSLog, to avoid linking Foundation. Do NOT introduce any
 * Obj-C-compiled code.
 *
 * Build: clang -bundle -mmacosx-version-min=10.9 \
 *        -framework IOKit -framework CoreFoundation -lobjc
 * Shipped as a SIMBL plugin (SIMBL-Info.plist); SIMBLAgent injects it into System Preferences at launch.
 */

#include <string.h>
#include <syslog.h>
#include <limits.h>
#include <unistd.h>
#include <sys/sysctl.h>
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
#include "mavericks_presence.h"
#include "mavericks_presence_observer.h"
#include "../mt2_cod_match.h"   /* mt2_cod_is_mt2 — device-class match that tolerates live service bits */

#define LOG(...) syslog(LOG_NOTICE, "[VoodooInputMavericksPane] " __VA_ARGS__)

/* CMake passes -DMAVERICKS_VERSION_STR="X.Y.Z" (lock-step with MAVERICKS_VERSION); keep the file self-compilable. */
#ifndef MAVERICKS_VERSION_STR
#define MAVERICKS_VERSION_STR "0.0.0"
#endif

/* Sparkle + the daily auto-check agent read this key from the updater's own prefs domain. */
#define MAVERICKS_UPDATER_DOMAIN CFSTR("com.schmonz.MavericksTrackpad2Updater")

/* ============================================================================================
 * 1. GENERIC HELPERS — objc dispatch shims + core cross-section state
 * ============================================================================================ */

static id          gPane = NULL;          /* the live Trackpad pane instance (captured on didSelect) */
static mavericks_presence_observer_t *gObs = NULL;  /* the shared presence observer; owns the SM + IOKit edges */

static int responds(id obj, SEL s) {
    return obj && ((signed char (*)(id, SEL, SEL))objc_msgSend)(
        obj, sel_registerName("respondsToSelector:"), s);
}

/* Launch the shared Sparkle updater helper on demand (one shared copy, same path for the osax + SIMBL
 * routes). Fired by the About tab's "Check for Updates…" button (section 5b).
 *
 * Launch via `open` (LaunchServices), NOT a direct fork+exec of the binary. Sparkle's update flow needs
 * the host to be a normal LaunchServices-registered app: Sparkle's Autoupdate helper waits for the host
 * to terminate (then installs the pkg + relaunches) through LaunchServices, so a direct-exec'd
 * (non-registered) host makes Autoupdate hang forever and the install NEVER happens (root-caused
 * on-device 2026-07-06: Autoupdate stuck waiting on a host pid that had already exited). The updater is
 * now a normal foreground app (not LSUIElement), so `open` also brings its Sparkle dialog to the front. */
static void mavericks_launch_updater(void) {
    const char *app = "/usr/local/lib/voodooinputmavericks/MavericksTrackpad2Updater.app";
    if (access(app, F_OK) != 0) { LOG("updater: %s not installed", app); return; }
    /* --args --user: this is an EXPLICIT summon, so the updater runs its interactive check and reports
     * status. Without it the opt-in updater treats the launch as a silent probe and shows nothing. */
    pid_t pid = fork();
    if (pid == 0) { execl("/usr/bin/open", "open", app, "--args", "--user", (char *)NULL); _exit(127); }
    LOG("updater: launched %s --user (via open)", app);
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

/* ============================================================================================
 * 2. BATTERY ROW — the ONE renderer for both transports
 * --------------------------------------------------------------------------------------------
 * Apple's USB Trackpad view HIDES the battery row (BatteryControl + percent + static label, in
 * the window's bottom bar) and shows "Set Up Bluetooth Trackpad…"; on BT it shows the row but
 * caches its own copy and never refreshes after a power-cycle. The MT2 reports battery on BOTH
 * transports (USB: Power-Device report 0x90; BT: the BNBTrackpadDevice node's BatteryPercent).
 * So we OWN the row for both: mavericks_render_battery paints Apple's own control from
 * mavericks_battery_now(), called from perform() (immediate, every transition) AND the aux tick (belt).
 * Painting the correct "NN%" overwrites any stale/charging leftover — no separate residue-strip.
 * ============================================================================================ */

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

/* Read the MT2 battery via HID (USB interface answers on the USB view). Returns 1 and fills
 * pct/charging on success. Fresh manager per read so cable cycles never leave a stale device ref.
 * kIOHIDOptionsTypeNone — never seizes the device. */
static int mavericks_usb_read_battery(int *pct, int *charging) {
    int got = 0;
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) return 0;
    /* Match ONLY the MT2 (ProductID 0x0265) — the HID manager returns just our device instead of
     * enumerating every HID device on the machine and post-filtering. (PID alone is enough; it's the
     * same on USB vid 05ac and BT vid 004c.) */
    { int pid = 0x0265;
      CFNumberRef pidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
      CFMutableDictionaryRef mm = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (mm && pidNum) CFDictionarySetValue(mm, CFSTR(kIOHIDProductIDKey), pidNum);
      IOHIDManagerSetDeviceMatching(mgr, mm);
      if (pidNum) CFRelease(pidNum);
      if (mm) CFRelease(mm); }
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

static int    gBattPainted   = 0;   /* we unhid + painted the battery row -> hide on NONE */
static int    gDevicePct     = -1;  /* the device's battery %, best-known; updated by the USB HID read AND
                                     * the BT node read, held across transports (one device either way) */
static int    gLastShownPct  = -1;  /* last value we painted -> HOLD keeps the display through removal */
static int    gLastShownChg  = 0;
static time_t gLastUsbRead   = 0;   /* throttle the (slow) USB HID read */
static int    gUsbReadInFlight = 0; /* a dispatched USB HID read is pending (don't stack) */

/* The debug.mt2_batt kext knob, read from the osax so a forced value applies on BOTH transports (the
 * kext only applies it on its own BT publish). -1 = off, 0-100 = forced value. */
static int mavericks_batt_override(void) {
    int v = -1; size_t sz = sizeof(v);
    if (sysctlbyname("debug.mt2_batt", &v, &sz, NULL, 0) != 0) return -1;
    return v;
}

/* "BatteryPercent" (0-100) off our fabricated AppleMultitouchDevice node, or -1 if absent (USB publishes
 * none; the BNBTrackpadDevice node is gone post-full-synthetic). Only our BT AMD carries BatteryPercent, so
 * iterate the AMDs and take the first that has it (robust if a co-connected genuine AMD is also present). */
static int mavericks_read_node_battery(void) {
    int pct = -1;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault,
            IOServiceMatching("AppleMultitouchDevice"), &it) == KERN_SUCCESS && it) {
        io_service_t svc;
        while (pct < 0 && (svc = IOIteratorNext(it))) {
            CFNumberRef n = (CFNumberRef)IORegistryEntryCreateCFProperty(svc, CFSTR("BatteryPercent"), kCFAllocatorDefault, 0);
            if (n) { CFNumberGetValue(n, kCFNumberIntType, &pct); CFRelease(n); }
            IOObjectRelease(svc);
        }
        IOObjectRelease(it);
    }
    return pct;
}

/* The device's battery % + whether it's charging. The % is ONE value regardless of transport (same
 * device), HELD so a transition shows it instantly; charging is a TRANSPORT property (on USB you're on
 * the cable -> charging, or charged at 100%; on BT you're on battery), inferred from the presence state rather than
 * waiting for the slow USB HID read. Debug override wins. A fresh BT node reads 0/absent until the
 * kext's first poll — treat that as the un-polled sentinel and hold the last value (a truly 0% MT2 is
 * dead + disconnected). Returns -1 only if the battery has never been known. */
static int mavericks_battery_now(int *charging) {
    *charging = (presence_observer_state(gObs) == PRESENCE_USB);
    int ov = mavericks_batt_override();
    if (ov >= 0 && ov <= 100) return ov;
    if (presence_observer_state(gObs) == PRESENCE_BT) {
        int pct = mavericks_read_node_battery();
        if (pct >= 1 && pct <= 100) gDevicePct = pct;
    }
    return gDevicePct;
}

/* Paint the battery row: gauge (setFloatValue) + "NN%[ (charging|charged)]" + unhide the three views.
 * On the cable the device is "charging" below 100% and "charged" at 100% (Apple's wording). */
static void mavericks_paint_battery(id root, int pct, int charging) {
    id ctl = NULL, pf = NULL, sl = NULL;
    find_battery_row(root, 0, &ctl, &pf, &sl);
    if (!ctl || !pf || !sl) return;
    ((void (*)(id, SEL, float))objc_msgSend)(ctl, sel_registerName("setFloatValue:"), (float)pct / 100.0f);
    const char *suffix = charging ? (pct >= 100 ? " (charged)" : " (charging)") : "";
    char label[48];
    snprintf(label, sizeof(label), "%d%%%s", pct, suffix);
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, label, kCFStringEncodingUTF8);
    if (s) {
        ((void (*)(id, SEL, id))objc_msgSend)(pf, sel_registerName("setStringValue:"), (id)s);
        CFRelease(s);
        /* The nib sizes the field for "100%"; widen so " (charging)"/" (charged)" isn't clipped. */
        ((void (*)(id, SEL))objc_msgSend)(pf, sel_registerName("sizeToFit"));
    }
    id vs[3] = { ctl, pf, sl };
    for (int i = 0; i < 3; i++)
        ((void (*)(id, SEL, signed char))objc_msgSend)(vs[i], sel_registerName("setHidden:"), 0);
    gLastShownPct = pct; gLastShownChg = charging;
    if (!gBattPainted) { gBattPainted = 1; LOG("battery: row painted (%s)", label); }
}

/* Refresh the USB battery % via the HID read (matches only the MT2, but still off the transition
 * path): throttled (~30 s), dispatched to the next runloop so it never blocks a switch, then repaints
 * if still on USB. */
static void mavericks_refresh_usb_battery(void) {
    time_t now = time(NULL);
    if (gUsbReadInFlight || (gLastUsbRead && now - gLastUsbRead < 30)) return;
    gLastUsbRead = now; gUsbReadInFlight = 1;
    dispatch_async(dispatch_get_main_queue(), ^{
        gUsbReadInFlight = 0;
        int p, c;
        if (mavericks_usb_read_battery(&p, &c)) {          /* c (charging byte) is ignored — inferred from transport */
            if (p >= 1 && p != gDevicePct) LOG("usb-battery: read %d%%", p);
            if (p >= 1 && p <= 100) gDevicePct = p;
            if (presence_observer_state(gObs) == PRESENCE_USB) { int ch; int pct = mavericks_battery_now(&ch); if (pct >= 0) mavericks_paint_battery(front_window_content(), pct, ch); }
        }
    });
}

/* THE single battery-row renderer for the current transport state — the ONE place every battery
 * behavior lives, unified across transports. Called from perform() (immediate, on every transition)
 * AND the aux tick (belt):
 *   USB/BT    -> paint the row from mavericks_battery_now() (USB HID / BT node); USB also kicks an async HID
 *                refresh. Painting the correct "NN%" overwrites any leftover "(charging)" from a prior
 *                USB session, so there is no separate residue-strip and no stale value survives.
 *   NoTrackpad-> hide the row (NoTrackpad must not show battery)
 *   HOLD      -> keep the display through the removal window
 * Fast (BT read = a quick IOKit lookup; USB paints the cache), so perform() calls it synchronously —
 * the correct value lands in the SAME runloop turn as the render, so there is no flash. */
static void mavericks_render_battery(id root) {
    if (presence_observer_state(gObs) == PRESENCE_HOLD) return;   /* removal window: the _checkBatteryTimer swizzle corrects any 0 in-flight */
    if (presence_observer_state(gObs) == PRESENCE_NONE) {
        if (gBattPainted) {
            id ctl = NULL, pf = NULL, sl = NULL;
            find_battery_row(root, 0, &ctl, &pf, &sl);
            id vs[3] = { ctl, pf, sl };
            for (int i = 0; i < 3; i++) if (vs[i])
                ((void (*)(id, SEL, signed char))objc_msgSend)(vs[i], sel_registerName("setHidden:"), 1);
            LOG("battery: row re-hidden (device -> NoTrackpad)");
        }
        gBattPainted = 0; gLastUsbRead = 0;
        return;
    }
    int charging; int pct = mavericks_battery_now(&charging);
    if (pct >= 0) mavericks_paint_battery(root, pct, charging);
    if (presence_observer_state(gObs) == PRESENCE_USB) mavericks_refresh_usb_battery();
}

/* ============================================================================================
 * 3. MAGIC-ACTION REPLAY + OBSERVER SUPPRESSION
 * --------------------------------------------------------------------------------------------
 * We OWN _magicTrackpadAction (the pane's transport-render entry) and suppress the pane's own
 * device-connected observer, so the SM is the sole decider of WHEN a render happens; perform()
 * replays the captured (self, arg) with the SM's `connected`.
 * ============================================================================================ */

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

/* Find the live MTTrackpadController (the render target). Prefer the CURRENT view's controller
 * (gPane.mCurrentController.mController) over the singleton to avoid staleness; NULL on the NoTrackpad
 * view (no trackpad controller exists — nothing to replay; the view-change loadMainView handles it). */
static id find_mt_controller(void) {
    Class mtc = objc_getClass("MTTrackpadController");
    if (!mtc || !gPane) return NULL;
    Ivar cc = class_getInstanceVariable(object_getClass(gPane), "mCurrentController");
    id cur = cc ? object_getIvar(gPane, cc) : NULL;
    if (cur) {
        Ivar mc = class_getInstanceVariable(object_getClass(cur), "mController");
        id ctrl = mc ? object_getIvar(cur, mc) : NULL;
        if (ctrl && ((signed char (*)(id, SEL, Class))objc_msgSend)(
                ctrl, sel_registerName("isKindOfClass:"), mtc)) return ctrl;
    }
    if (((signed char (*)(id, SEL, SEL))objc_msgSend)(
            mtc, sel_registerName("respondsToSelector:"), sel_registerName("sharedController")))
        return ((id (*)(id, SEL))objc_msgSend)(mtc, sel_registerName("sharedController"));
    return NULL;
}

/* Eager-capture (self,arg) straight from the pane's ivars, so the faithful replay is ready BEFORE the
 * first transition. Without this, perform() fires on the presence edge before the pane's own
 * _magicTrackpadAction lands (my_magicAction), the replay skips, and a same-view switch (no loadMainView)
 * leaves stale art — the capture-race. self = the MTTrackpadController; arg = its
 * mMagicTrackpadServiceObserver (the IOServiceObserver whose armIterators the body calls). Both are stable
 * for the pane's life (so one capture is reusable). VERIFY arg respondsToSelector:armIterators before use,
 * so a wrong object can never doesNotRecognizeSelector (the Task-5 regression). No render, no loadMainView. */
static void eager_capture_magic(void) {
    if (gMagicCtrl && gMagicArg) return;
    id ctrl = find_mt_controller();
    if (!ctrl) { LOG("eager-capture: no MTTrackpadController yet"); return; }
    Ivar iv = class_getInstanceVariable(object_getClass(ctrl), "mMagicTrackpadServiceObserver");
    id obs = iv ? object_getIvar(ctrl, iv) : NULL;
    if (!obs) { LOG("eager-capture: %s has nil mMagicTrackpadServiceObserver (iv=%p)", object_getClassName(ctrl), (void*)iv); return; }
    if (!responds(obs, sel_registerName("armIterators"))) {
        LOG("eager-capture: observer %s does not respond to armIterators", object_getClassName(obs)); return;
    }
    gMagicCtrl = ctrl; gMagicArg = obs;
    LOG("eager-captured magic (self=%s, arg=%s) — replay ready before first transition",
        object_getClassName(ctrl), object_getClassName(obs));
}

/* Replay the pane's OWN _magicTrackpadAction faithfully: the captured (self=controller, arg) with the
 * SM's `connected`. The body does [arg armIterators], so arg MUST be the pane's observer, never the
 * controller (that was the doesNotRecognizeSelector regression). If the autonomous call hasn't captured
 * the pair yet, eager-capture it from the ivars first (fixes the same-view capture-race); only if that
 * also fails (e.g. the NoTrackpad view) do we skip and let the view-change loadMainView render natively. */
static void gOrigMagicAction_call(int connected) {
    if (!gMagicCtrl || !gMagicArg) eager_capture_magic();
    if (!gOrigMagicAction || !gPane || !gMagicCtrl || !gMagicArg) {
        LOG("gOrigMagicAction_call: (self,arg) not captured yet -> skip replay");
        return;
    }
    gOrigMagicAction(gMagicCtrl, sel_registerName("_magicTrackpadAction:deviceConnected:"),
                     gMagicArg, (signed char)(connected ? 1 : 0));
}

/* Suppress the pane's OWN observer-driven update ENTIRELY — we take ownership of when the display
 * updates. Without this the pane redraws on every BT edge (e.g. blanks to NoTrackpad the instant BT
 * drops during a BT->USB handoff, before USB is recognized). Our shared presence observer drives the SM,
 * which owns every render, so the pane holds its prior state through a switch, then updates once. */
static void (*gOrigDeviceConnected)(id, SEL, id, signed char) = NULL;  /* saved, intentionally never called */
static void my_deviceConnected(id self, SEL _cmd, id obs, signed char connected) {
    (void)self; (void)_cmd; (void)obs; (void)connected;
}

/* ============================================================================================
 * 4. STATE MACHINE + DEVICE OBSERVERS
 * --------------------------------------------------------------------------------------------
 * Live IOKit observers on both transports feed one SM event each; the SM decides the render
 * action; perform() applies it against the live pane and then makes the battery row correct.
 * ============================================================================================ */

/* Perform one SM action against the live pane. ON_* drives Apple's own controller update:
 * loadMainView only when the view type must change (NoTrackpad<->Trackpad), else just set the
 * battery via the real _magicTrackpadAction(connected). We OWN that selector (see my_magicAction),
 * so this is the only place it fires. */
static void mavericks_inject_about_tab(id root);   /* fwd: (re)inject the About tab after a render (idempotent) */
static void perform(presence_action_t a) {
    if (!gPane) return;
    switch (a) {
    case PRESENCE_ACT_ABSENT:
        if (current_view_is_trackpad()) {
            /* Clean any "(charging)" off the outgoing battery field NOW, while the trackpad view still
             * exists, BEFORE loadMainView switches to NoTrackpad. The field is reused when BT later
             * brings the view back, so pre-cleaning it here means it's already a plain "NN%" on the BT
             * wake -> no flash. (The one irreducible pre-teardown step; mavericks_render_battery on the BT
             * render then paints the live node value over it.) */
            if (gDevicePct >= 0) mavericks_paint_battery(front_window_content(), gDevicePct, 0);
            SEL lmv = sel_registerName("loadMainView");
            if (responds(gPane, lmv)) ((id (*)(id, SEL))objc_msgSend)(gPane, lmv);
        }
        LOG("perform: ABSENT (NoTrackpad)");
        break;
    case PRESENCE_ACT_ON_BT:
    case PRESENCE_ACT_ON_USB: {
        /* Rebuild the view only on a real change to/from the trackpad view (e.g. leaving NoTrackpad).
         * A BT<->USB switch stays on the trackpad view, so the faithful replay below does the in-place
         * transport update — no loadMainView, no movie-reload blink. */
        if (!current_view_is_trackpad()) {
            SEL lmv = sel_registerName("loadMainView");
            if (responds(gPane, lmv)) ((id (*)(id, SEL))objc_msgSend)(gPane, lmv);
        }
        gOrigMagicAction_call(a == PRESENCE_ACT_ON_BT ? 1 : 0);
        LOG("perform: %s", a == PRESENCE_ACT_ON_BT ? "ON_BT" : "ON_USB");
        break;
    }
    case PRESENCE_ACT_HOLD: LOG("perform: HOLD (keep view; arm window)"); break;
    case PRESENCE_ACT_NONE: break;
    }
    /* ONE place for every battery behavior: after any state render, make the battery row correct for
     * the new transport state (USB/BT paint / NoTrackpad hide). Fast + non-blocking (the HID read is
     * async inside mavericks_render_battery), so it lands in the SAME runloop turn as the render — no flash,
     * and the USB paint appears immediately instead of a tick later. The aux tick calls the same
     * function as the steady-state belt. */
    if (a == PRESENCE_ACT_ABSENT || a == PRESENCE_ACT_ON_BT || a == PRESENCE_ACT_ON_USB) {
        mavericks_render_battery(front_window_content());
        mavericks_inject_about_tab(front_window_content());  /* loadMainView rebuilds the tab strip on a
                                                        * reconnect -> re-inject the About tab NOW, not on the tick */
    }
}

/* The pane's presence-transition callback: render each SM action against the live pane. The presence
 * SM, the IOKit edges, and the removal-window HOLD timer now live in the shared presence observer
 * (src/mavericks_presence_observer.h); this callback is all that stays pane-specific. */
static void on_presence(presence_action_t a, presence_event_t e, void *ctx) { (void)e; (void)ctx; perform(a); }

/* ============================================================================================
 * 5. CHANGE-BATTERIES BUTTON — keep the sealed-battery AA swap prompt hidden
 * --------------------------------------------------------------------------------------------
 * The MT2 charges over Lightning (sealed battery), so the AA-era swap prompt never applies — yet
 * the pane RE-SHOWS it every _checkBatteryTimer tick whenever the level reads 0%. We hide it via
 * the ivar (swizzle path) AND a capture-free view walk (tick path), covering both.
 * ============================================================================================ */

/* Original MTTrackpadController _checkBatteryTimer: — we call through then force the button hidden. */
static void (*gOrigCheckBatteryTimer)(id, SEL, id) = NULL;

/* Force the pane's "Change Batteries" button hidden via its ivar. setHidden:(pct>0.0), threshold 0.0;
 * disasm Trackpad.prefPane @0x4bdf. mChangeBatteryButton is an MTTrackpadController ivar. */
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

/* Swizzled _checkBatteryTimer:: let the pane update the level/label as usual, then re-hide the
 * Change-Batteries button (it re-shows it at 0%). Caveat: hides for any trackpad the pane shows; on
 * this stack the connected device is always the MT2 (a co-connected genuine MT1 with AA batteries
 * would be a rare exception — future refinement could gate on the MT2 CoD minor 0x25). */
static void my_checkBatteryTimer(id self, SEL _cmd, id timer) {
    if (gOrigCheckBatteryTimer) gOrigCheckBatteryTimer(self, _cmd, timer);   /* Apple reads batteryPercent + paints */
    hide_change_battery_button(self);
    /* _checkBatteryTimer is the ONE method Apple uses to paint the battery (RE: _updateBTDevice + the
     * connect/disconnect/state-change notifications all funnel here). It does setFloatValue: on
     * mBTBatteryControl with batteryPercent — and on a BT power-off batteryPercent momentarily reads 0,
     * so it paints an empty battery. Intercede IN-FLIGHT (same runloop turn as Apple's paint, so nothing
     * draws in between): read the EXACT mBTBatteryControl ivar off self — reliable during teardown, no
     * front-window/tree walk — and if Apple left it empty while we still hold a real value, restore ours
     * on the control + its label. If Apple painted a real (non-zero) value, leave it (pass-through). */
    if (gDevicePct >= 1) {
        Ivar iv = class_getInstanceVariable(object_getClass(self), "mBTBatteryControl");
        id ctl = iv ? object_getIvar(self, iv) : NULL;
        if (ctl && ((double (*)(id, SEL))objc_msgSend)(ctl, sel_registerName("doubleValue")) <= 0.0) {
            ((void (*)(id, SEL, float))objc_msgSend)(ctl, sel_registerName("setFloatValue:"), (float)gDevicePct / 100.0f);
            Ivar lv = class_getInstanceVariable(object_getClass(self), "mBTBatteryControlLabel");
            id lbl = lv ? object_getIvar(self, lv) : NULL;
            if (lbl) {
                char t[16]; snprintf(t, sizeof(t), "%d%%", gDevicePct);
                CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, t, kCFStringEncodingUTF8);
                if (s) { ((void (*)(id, SEL, id))objc_msgSend)(lbl, sel_registerName("setStringValue:"), (id)s); CFRelease(s); }
            }
        }
    }
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

/* ---- Updater "Check for Updates" action ----------------------------------------------------
 * The update entry point now lives on the About tab (section 5b), not a repurposed Apple button.
 * This selector + IMP are the action wired to the About tab's "Check for Updates…" button; the
 * former Set-Up-button repurpose (relabel setupBTMouse: -> mt2CheckForUpdates:) has been retired so
 * Apple's "Set Up Bluetooth Trackpad…" button keeps its original title + onboarding action. */

/* The button's action IMP, installed on the pane class -> launch the shared updater helper. */
static void mavericks_check_updates_imp(id self, SEL _cmd, id sender) {
    (void)self; (void)_cmd; (void)sender;
    LOG("updater: Check-for-Updates clicked");
    mavericks_launch_updater();
}

/* Add -[<pane> mt2CheckForUpdates:] once — a real selector so the button's target/action reaches our
 * C IMP (we can't @implementation under GC, but class_addMethod with an IMP is fine). */
static int gUpdaterActionInstalled = 0;
static void install_updater_action(void) {
    if (gUpdaterActionInstalled || !gPane) return;
    Class c = object_getClass(gPane);
    class_addMethod(c, sel_registerName("mt2CheckForUpdates:"), (IMP)mavericks_check_updates_imp, "v@:@");
    gUpdaterActionInstalled = 1;
    LOG("updater: installed mt2CheckForUpdates: on %s", class_getName(c));
}

/* ============================================================================================
 * 5b. ABOUT TAB — a 4th tab on the Trackpad pane's NSTabView
 * --------------------------------------------------------------------------------------------
 * Adds an "About" tab carrying: the product/version label, a "Check for Updates…" button (the update
 * entry point, replacing the retired Set-Up-button repurpose), an "Automatically check for updates"
 * checkbox (persisted to the updater's SUEnableAutomaticChecks in its own prefs domain), and a
 * "View on GitHub" link. All controls are built through the objc runtime (GC-neutral pure C) and laid
 * out with frames. Injection is idempotent (keyed on the "MavericksAbout" tab identifier) and re-asserted on
 * every pane render, so a loadMainView rebuild never duplicates it.
 * ============================================================================================ */

/* The two extra About-tab action IMPs (Check-for-Updates reuses mt2CheckForUpdates: from above). */
static void mavericks_toggle_autocheck_imp(id self, SEL _cmd, id sender) {
    (void)self; (void)_cmd;
    /* sender is the checkbox; NSOnState == 1. Persist to the UPDATER's domain where Sparkle + the daily
     * agent read it, then synchronize so the change is durable immediately. */
    long state = ((long (*)(id, SEL))objc_msgSend)(sender, sel_registerName("state"));
    CFPreferencesSetAppValue(CFSTR("SUEnableAutomaticChecks"),
                             state ? kCFBooleanTrue : kCFBooleanFalse, MAVERICKS_UPDATER_DOMAIN);
    CFPreferencesAppSynchronize(MAVERICKS_UPDATER_DOMAIN);
    LOG("about: SUEnableAutomaticChecks -> %s", state ? "on" : "off");
}
static void mavericks_open_github_imp(id self, SEL _cmd, id sender) {
    (void)self; (void)_cmd; (void)sender;
    Class wsCls = objc_getClass("NSWorkspace");
    id ws = wsCls ? ((id (*)(Class, SEL))objc_msgSend)(wsCls, sel_registerName("sharedWorkspace")) : NULL;
    Class urlCls = objc_getClass("NSURL");
    id url = urlCls ? ((id (*)(Class, SEL, id))objc_msgSend)(urlCls, sel_registerName("URLWithString:"),
                        (id)CFSTR("https://github.com/schmonz/mavericks-magic-trackpad2")) : NULL;
    if (ws && url) ((signed char (*)(id, SEL, id))objc_msgSend)(ws, sel_registerName("openURL:"), url);
    LOG("about: View on GitHub clicked");
}

/* The pane's tab delegate CRASHES on a foreign 4th tab. tabView:willSelectTabViewItem: does
 * [[self _allControllers] objectAtIndex:<tabIndex>], and _allControllers has one entry per ORIGINAL tab
 * (3) — so selecting our index-3 tab is objectAtIndex:3 on a 3-element array => NSRangeException, which
 * wedges tab switching. tabView:didSelectTabViewItem: also persists "lastselectedtab"=our index (would
 * crash on the next open's restore). Fix: swizzle both to SKIP the pane's per-tab logic for OUR item
 * (identifier "MavericksAbout") and pass through for its own tabs — NSTabView still shows our item's view via
 * its built-in display, and mGestureViewController stays on the last real tab. RE'd 2026-07-06 (disasm
 * of tabView:willSelectTabViewItem: @0x6eab). */
static void (*gOrigTabWillSelect)(id, SEL, id, id) = NULL;
static void (*gOrigTabDidSelect)(id, SEL, id, id) = NULL;
static int mavericks_is_about_item(id item) {
    if (!item) return 0;
    id ident = ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("identifier"));
    return ident && CFGetTypeID((CFTypeRef)ident) == CFStringGetTypeID()
        && CFEqual((CFStringRef)ident, CFSTR("MavericksAbout"));
}
static void my_tab_will_select(id self, SEL _cmd, id tv, id item) {
    if (mavericks_is_about_item(item)) return;              /* skip: [_allControllers objectAtIndex:3] would crash */
    if (gOrigTabWillSelect) gOrigTabWillSelect(self, _cmd, tv, item);
}
static void my_tab_did_select(id self, SEL _cmd, id tv, id item) {
    if (mavericks_is_about_item(item)) return;              /* skip: don't persist our tab as lastselectedtab */
    if (gOrigTabDidSelect) gOrigTabDidSelect(self, _cmd, tv, item);
}

/* Register the three About-tab action selectors on the live pane class once (class_addMethod with a C
 * IMP — the GC-safe substitute for @implementation). mt2CheckForUpdates: is added by
 * install_updater_action(); add the two new ones here. Also swizzle the tab delegate (see above). */
static int gAboutActionsInstalled = 0;
static void install_about_actions(void) {
    if (!gPane) return;
    install_updater_action();                       /* mt2CheckForUpdates: (idempotent) */
    if (gAboutActionsInstalled) return;
    Class c = object_getClass(gPane);
    class_addMethod(c, sel_registerName("mt2ToggleAutoCheck:"), (IMP)mavericks_toggle_autocheck_imp, "v@:@");
    class_addMethod(c, sel_registerName("mt2OpenGitHub:"),      (IMP)mavericks_open_github_imp,      "v@:@");
    gAboutActionsInstalled = 1;
    LOG("about: installed mt2ToggleAutoCheck:/mt2OpenGitHub: on %s", class_getName(c));
}

/* Swizzle the TAB DELEGATE's class so our foreign tab can't crash its per-tab logic. The delegate is the
 * MTTrackpadController that owns the NSTabView — a DIFFERENT object from gPane (the NSPreferencePane), so
 * we must swizzle [tabview delegate]'s class, not gPane's. Installed once, from mavericks_inject_about_tab
 * where the live NSTabView is in hand. */
static int gTabSwizzleInstalled = 0;
static void install_tab_delegate_swizzle(id tabview) {
    if (gTabSwizzleInstalled || !tabview) return;
    id delegate = ((id (*)(id, SEL))objc_msgSend)(tabview, sel_registerName("delegate"));
    if (!delegate) return;
    Class dc = object_getClass(delegate);
    Method mw = class_getInstanceMethod(dc, sel_registerName("tabView:willSelectTabViewItem:"));
    if (mw) { gOrigTabWillSelect = (void (*)(id, SEL, id, id))method_getImplementation(mw);
              method_setImplementation(mw, (IMP)my_tab_will_select); }
    Method md = class_getInstanceMethod(dc, sel_registerName("tabView:didSelectTabViewItem:"));
    if (md) { gOrigTabDidSelect = (void (*)(id, SEL, id, id))method_getImplementation(md);
              method_setImplementation(md, (IMP)my_tab_did_select); }
    if (mw) gTabSwizzleInstalled = 1;
    LOG("about: tab-delegate swizzle on %s (will=%d did=%d)", class_getName(dc),
        gOrigTabWillSelect != NULL, gOrigTabDidSelect != NULL);
}

/* Is the updater's automatic-check preference ON? Reads the updater domain; DEFAULT OFF if unset. */
static int mavericks_autocheck_enabled(void) {
    int on = 0;
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("SUEnableAutomaticChecks"), MAVERICKS_UPDATER_DOMAIN);
    if (v) {
        if (CFGetTypeID(v) == CFBooleanGetTypeID()) on = CFBooleanGetValue((CFBooleanRef)v);
        CFRelease(v);
    }
    return on;
}

/* Small control builders. Controls pin to the container's TOP (autoresize: flexible bottom margin) so
 * the layout survives the NSTabView resizing the container to its content rect. */
#define MAVERICKS_AR_TOP 8   /* NSViewMinYMargin — bottom margin flexible => fixed distance from the top edge */

static id mavericks_make_label(CGRect frame, CFStringRef text) {
    Class cls = objc_getClass("NSTextField");
    id tf = ((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
    tf = ((id (*)(id, SEL, CGRect))objc_msgSend)(tf, sel_registerName("initWithFrame:"), frame);
    ((void (*)(id, SEL, signed char))objc_msgSend)(tf, sel_registerName("setEditable:"), 0);
    ((void (*)(id, SEL, signed char))objc_msgSend)(tf, sel_registerName("setSelectable:"), 0);
    ((void (*)(id, SEL, signed char))objc_msgSend)(tf, sel_registerName("setBezeled:"), 0);
    ((void (*)(id, SEL, signed char))objc_msgSend)(tf, sel_registerName("setDrawsBackground:"), 0);
    if (text) ((void (*)(id, SEL, id))objc_msgSend)(tf, sel_registerName("setStringValue:"), (id)text);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(tf, sel_registerName("setAutoresizingMask:"), MAVERICKS_AR_TOP);
    return tf;
}
static id mavericks_make_button(CGRect frame, CFStringRef title, SEL action) {
    Class cls = objc_getClass("NSButton");
    id b = ((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
    b = ((id (*)(id, SEL, CGRect))objc_msgSend)(b, sel_registerName("initWithFrame:"), frame);
    ((void (*)(id, SEL, id))objc_msgSend)(b, sel_registerName("setTitle:"), (id)title);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(b, sel_registerName("setBezelStyle:"), 1);   /* NSRoundedBezelStyle */
    ((void (*)(id, SEL, id))objc_msgSend)(b, sel_registerName("setTarget:"), gPane);
    ((void (*)(id, SEL, SEL))objc_msgSend)(b, sel_registerName("setAction:"), action);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(b, sel_registerName("setAutoresizingMask:"), MAVERICKS_AR_TOP);
    return b;
}

/* Autoresize masks (NSAutoresizingMaskOptions): MinXMargin=1, WidthSizable=2, MaxXMargin=4,
 * MinYMargin=8, HeightSizable=16, MaxYMargin=32. Centered+top-anchored = 1|4|8; full-width+top = 2|8;
 * bottom-right-pinned = 1|32. Text alignment (old NSTextAlignment): left=0, right=1, center=2. */
#define MAVERICKS_AR_CENTER_TOP  (1|4|8)
#define MT2_W  520
#define MT2_H  260
#define MAVERICKS_TAG_VERLINK 55501   /* the "GitHub | <ver>" link button */
#define MAVERICKS_TAG_HINT    55502   /* the "Update available" hint label */
#define MAVERICKS_TAG_AUTOCHECK 55503 /* the "Check automatically" checkbox */

/* The version actually installed on disk = the pkg-updated updater app's CFBundleShortVersionString.
 * The About tab shows THIS (not the compile-baked MAVERICKS_VERSION_STR) so it reflects reality after an update
 * even though this osax binary may be older. Returns a +1 CFString (caller releases) or NULL -> fall back
 * to compile-baked. CF-only (no ObjC syntax) so it stays GC-neutral. */
static CFStringRef mavericks_installed_version_copy(void) {
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
        CFSTR("/usr/local/lib/voodooinputmavericks/MavericksTrackpad2Updater.app/Contents/Info.plist"),
        kCFURLPOSIXPathStyle, false);
    if (!url) return NULL;
    CFReadStreamRef s = CFReadStreamCreateWithFile(kCFAllocatorDefault, url);
    CFRelease(url);
    if (!s) return NULL;
    CFStringRef ver = NULL;
    if (CFReadStreamOpen(s)) {
        CFPropertyListRef pl = CFPropertyListCreateWithStream(kCFAllocatorDefault, s, 0,
                                   kCFPropertyListImmutable, NULL, NULL);
        if (pl) {
            if (CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
                CFStringRef v = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)pl,
                                    CFSTR("CFBundleShortVersionString"));
                if (v && CFGetTypeID(v) == CFStringGetTypeID()) ver = CFStringCreateCopy(kCFAllocatorDefault, v);
            }
            CFRelease(pl);
        }
        CFReadStreamClose(s);
    }
    CFRelease(s);
    return ver;
}

/* Fill buf with the installed version (updater Info.plist), or the compile-baked fallback. */
static void mavericks_version_cstr(char *buf, unsigned long n) {
    CFStringRef iv = mavericks_installed_version_copy();
    if (iv && CFStringGetCString(iv, buf, (CFIndex)n, kCFStringEncodingUTF8)) { CFRelease(iv); return; }
    if (iv) CFRelease(iv);
    snprintf(buf, n, "%s", MAVERICKS_VERSION_STR);
}

static char gAboutVer[48] = "";   /* version currently shown in the About link (change-detect for refresh) */

/* Build the About tab's container view + its controls, CENTERED. Big bold title on top, the update
 * controls + GitHub hyperlink centered, and the version tucked in the lower-right corner. Version from
 * MAVERICKS_VERSION_STR (compile time); the checkbox reflects SUEnableAutomaticChecks (default OFF). */
static id mavericks_build_about_view(void) {
    Class viewCls = objc_getClass("NSView");
    id v = ((id (*)(Class, SEL))objc_msgSend)(viewCls, sel_registerName("alloc"));
    v = ((id (*)(id, SEL, CGRect))objc_msgSend)(v, sel_registerName("initWithFrame:"), CGRectMake(0, 0, MT2_W, MT2_H));
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(v, sel_registerName("setAutoresizingMask:"), 2 | 16);
    SEL addSub = sel_registerName("addSubview:");
    SEL setMask = sel_registerName("setAutoresizingMask:");
    SEL setAlign = sel_registerName("setAlignment:");

    /* Title — large, bold, centered, full width. */
    id title = mavericks_make_label(CGRectMake(0, 200, MT2_W, 36), CFSTR("Mavericks Trackpad 2"));
    Class fontCls = objc_getClass("NSFont");
    id font = fontCls ? ((id (*)(Class, SEL, double))objc_msgSend)(fontCls, sel_registerName("boldSystemFontOfSize:"), 22.0) : NULL;
    if (font) ((void (*)(id, SEL, id))objc_msgSend)(title, sel_registerName("setFont:"), font);
    ((void (*)(id, SEL, long))objc_msgSend)(title, setAlign, 2);       /* center */
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(title, setMask, 2 | 8);   /* width-sizable, top */
    ((void (*)(id, SEL, id))objc_msgSend)(v, addSub, title);

    /* Check for Updates — centered button; standard control font so it matches the pane's own buttons. */
    id upd = mavericks_make_button(CGRectMake((MT2_W-200)/2, 148, 200, 32), CFSTR("Check for Updates…"),
                             sel_registerName("mt2CheckForUpdates:"));
    Class fontCls2 = objc_getClass("NSFont");
    id ctlfont = fontCls2 ? ((id (*)(Class, SEL, double))objc_msgSend)(fontCls2, sel_registerName("systemFontOfSize:"), 13.0) : NULL;
    if (ctlfont) ((void (*)(id, SEL, id))objc_msgSend)(upd, sel_registerName("setFont:"), ctlfont);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(upd, setMask, MAVERICKS_AR_CENTER_TOP);
    ((void (*)(id, SEL, id))objc_msgSend)(v, addSub, upd);

    /* Check automatically — checkbox centered directly under the Check for Updates button; same font. */
    id chk = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSButton"), sel_registerName("alloc"));
    chk = ((id (*)(id, SEL, CGRect))objc_msgSend)(chk, sel_registerName("initWithFrame:"), CGRectMake((MT2_W-148)/2, 116, 148, 22));
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(chk, sel_registerName("setButtonType:"), 3);
    ((void (*)(id, SEL, id))objc_msgSend)(chk, sel_registerName("setTitle:"), (id)CFSTR("Check automatically"));
    if (ctlfont) ((void (*)(id, SEL, id))objc_msgSend)(chk, sel_registerName("setFont:"), ctlfont);
    ((void (*)(id, SEL, long))objc_msgSend)(chk, sel_registerName("setTag:"), MAVERICKS_TAG_AUTOCHECK);
    ((void (*)(id, SEL, long))objc_msgSend)(chk, sel_registerName("setState:"), mavericks_autocheck_enabled() ? 1 : 0);
    ((void (*)(id, SEL, id))objc_msgSend)(chk, sel_registerName("setTarget:"), gPane);
    ((void (*)(id, SEL, SEL))objc_msgSend)(chk, sel_registerName("setAction:"), sel_registerName("mt2ToggleAutoCheck:"));
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(chk, setMask, MAVERICKS_AR_CENTER_TOP);
    ((void (*)(id, SEL, id))objc_msgSend)(v, addSub, chk);

    /* "Update available" hint — centered below the checkbox, HIDDEN until an update is pending. The
     * updater records MT2AvailableUpdateVersion when Sparkle finds one; mavericks_about_refresh() (reconcile
     * tick) fills + shows/hides this by tag. */
    id hint = mavericks_make_label(CGRectMake(0, 88, MT2_W, 18), NULL);
    ((void (*)(id, SEL, long))objc_msgSend)(hint, setAlign, 2);            /* center */
    if (ctlfont) ((void (*)(id, SEL, id))objc_msgSend)(hint, sel_registerName("setFont:"), ctlfont);
    ((void (*)(id, SEL, long))objc_msgSend)(hint, sel_registerName("setTag:"), MAVERICKS_TAG_HINT);
    ((void (*)(id, SEL, signed char))objc_msgSend)(hint, sel_registerName("setHidden:"), 1);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(hint, setMask, 2 | 8); /* width-sizable, top */
    ((void (*)(id, SEL, id))objc_msgSend)(v, addSub, hint);

    /* Lower-right corner: just the version number, plain + inert, right-aligned so it hugs the edge as the
     * version changes. Carries the tag the reconcile tick updates after an install. (No GitHub link — the
     * repo link lived here but its click area/placement read poorly; dropped per user.) */
    char vbuf[48];
    mavericks_version_cstr(vbuf, sizeof vbuf);
    snprintf(gAboutVer, sizeof gAboutVer, "%s", vbuf);
    CFStringRef vs = CFStringCreateWithCString(kCFAllocatorDefault, vbuf, kCFStringEncodingUTF8);
    id ver = mavericks_make_label(CGRectMake(MT2_W-80, 12, 68, 16), vs);
    if (vs) CFRelease(vs);
    Class vfCls = objc_getClass("NSFont");
    id vfont = vfCls ? ((id (*)(Class, SEL, double))objc_msgSend)(vfCls, sel_registerName("systemFontOfSize:"), 11.0) : NULL;
    if (vfont) ((void (*)(id, SEL, id))objc_msgSend)(ver, sel_registerName("setFont:"), vfont);
    ((void (*)(id, SEL, long))objc_msgSend)(ver, setAlign, 1);          /* right — hugs the edge */
    ((void (*)(id, SEL, long))objc_msgSend)(ver, sel_registerName("setTag:"), MAVERICKS_TAG_VERLINK);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(ver, setMask, 1 | 32);   /* pinned bottom-right */
    ((void (*)(id, SEL, id))objc_msgSend)(v, addSub, ver);
    return v;
}

/* Find the pane's tab strip: the NSTabView in the pane window's view tree (Trackpad view only — the
 * NoTrackpad view has no tab strip). Matched by class OR by answering numberOfTabViewItems. */
static id find_tabview(id view, int depth) {
    if (!view || depth > 14) return NULL;
    Class tv = objc_getClass("NSTabView");
    if (tv && ((signed char (*)(id, SEL, Class))objc_msgSend)(view, sel_registerName("isKindOfClass:"), tv))
        return view;
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < n; i++) {
        id r = find_tabview(((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i), depth + 1);
        if (r) return r;
    }
    return NULL;
}

/* (Re)inject the About tab. Idempotent: keyed on the "MavericksAbout" tab identifier — if the strip already
 * carries it, do nothing. Called from every pane render + the reconcile tick, so a loadMainView that
 * rebuilds the tab strip re-adds it once, never duplicating. No-op on NoTrackpad (no NSTabView). */
static void mavericks_inject_about_tab(id root) {
    if (!gPane || !root) return;
    id tabview = find_tabview(root, 0);
    if (!tabview) return;
    install_about_actions();
    install_tab_delegate_swizzle(tabview);   /* swizzle the REAL delegate (MTTrackpadController), not gPane */
    long idx = ((long (*)(id, SEL, id))objc_msgSend)(
        tabview, sel_registerName("indexOfTabViewItemWithIdentifier:"), (id)CFSTR("MavericksAbout"));
    if (idx != LONG_MAX) return;   /* NSNotFound == NSIntegerMax; anything else => already present */

    Class itemCls = objc_getClass("NSTabViewItem");
    id item = ((id (*)(Class, SEL))objc_msgSend)(itemCls, sel_registerName("alloc"));
    item = ((id (*)(id, SEL, id))objc_msgSend)(item, sel_registerName("initWithIdentifier:"), (id)CFSTR("MavericksAbout"));
    ((void (*)(id, SEL, id))objc_msgSend)(item, sel_registerName("setLabel:"), (id)CFSTR("About"));
    ((void (*)(id, SEL, id))objc_msgSend)(item, sel_registerName("setView:"), mavericks_build_about_view());
    ((void (*)(id, SEL, id))objc_msgSend)(tabview, sel_registerName("addTabViewItem:"), item);
    LOG("about: injected About tab (4th) on %s", object_getClassName(tabview));
}

/* Live-refresh the About tab's dynamic bits from the reconcile tick: the displayed version tracks the
 * INSTALLED version (right after an update, no reopen needed), and the "Update available" hint shows/hides
 * from the version the updater recorded. Controls are re-found by TAG each call (no dangling refs across a
 * pane rebuild). No-op if the About tab isn't present. */
static void mavericks_about_refresh(id root) {
    if (!root) return;
    id tabview = find_tabview(root, 0);
    if (!tabview) return;
    long idx = ((long (*)(id, SEL, id))objc_msgSend)(
        tabview, sel_registerName("indexOfTabViewItemWithIdentifier:"), (id)CFSTR("MavericksAbout"));
    if (idx == LONG_MAX) return;                        /* About tab not present */
    id item = ((id (*)(id, SEL, long))objc_msgSend)(tabview, sel_registerName("tabViewItemAtIndex:"), idx);
    id view = item ? ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("view")) : NULL;
    if (!view) return;

    char cur[48];
    mavericks_version_cstr(cur, sizeof cur);

    /* installed version changed -> update the " | <ver>" label (right-aligned, so no reposition needed) */
    if (strcmp(cur, gAboutVer) != 0) {
        id lbl = ((id (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("viewWithTag:"), (long)MAVERICKS_TAG_VERLINK);
        if (lbl) {
            CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, cur, kCFStringEncodingUTF8);
            if (s) { ((void (*)(id, SEL, id))objc_msgSend)(lbl, sel_registerName("setStringValue:"), (id)s); CFRelease(s); }
            snprintf(gAboutVer, sizeof gAboutVer, "%s", cur);
        }
    }

    /* keep the "Check automatically" checkbox in sync with the persisted setting, so an external change
     * (the daily agent, a second window, a defaults write) never leaves it showing a stale state. */
    id box = ((id (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("viewWithTag:"), (long)MAVERICKS_TAG_AUTOCHECK);
    if (box) ((void (*)(id, SEL, long))objc_msgSend)(box, sel_registerName("setState:"), mavericks_autocheck_enabled() ? 1 : 0);

    /* "Update available" hint: show only if the updater recorded an available version that differs from
     * what's installed (a lingering flag whose version == installed means we already updated -> hide). */
    id hint = ((id (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("viewWithTag:"), (long)MAVERICKS_TAG_HINT);
    if (!hint) return;
    int show = 0;
    CFPreferencesAppSynchronize(MAVERICKS_UPDATER_DOMAIN);
    CFStringRef av = CFPreferencesCopyAppValue(CFSTR("MT2AvailableUpdateVersion"), MAVERICKS_UPDATER_DOMAIN);
    if (av && CFGetTypeID(av) == CFStringGetTypeID()) {
        char avc[48];
        if (CFStringGetCString(av, avc, sizeof avc, kCFStringEncodingUTF8) && avc[0] && strcmp(avc, cur) != 0) {
            char hbuf[80]; snprintf(hbuf, sizeof hbuf, "Update available: %s", avc);
            CFStringRef hs = CFStringCreateWithCString(kCFAllocatorDefault, hbuf, kCFStringEncodingUTF8);
            if (hs) { ((void (*)(id, SEL, id))objc_msgSend)(hint, sel_registerName("setStringValue:"), (id)hs); CFRelease(hs); }
            show = 1;
        }
    }
    if (av) CFRelease(av);
    ((void (*)(id, SEL, signed char))objc_msgSend)(hint, sel_registerName("setHidden:"), show ? 0 : 1);
}

/* Hide the button — called from our own 2s reconcile tick so it fires regardless of whether the
 * pane's battery timer re-runs. Walk from the pane's mainView (covers whichever controller/tab owns
 * the button right now), and refresh the battery row on the same tick (belt for the render owner). */
static void hide_battery_button_now(void) {
    if (!gPane) return;
    id mv = ((id (*)(id, SEL))objc_msgSend)(gPane, sel_registerName("mainView"));
    if (!mv) return;
    /* The battery row (level + Change Batteries) is NOT inside mainView — it sits in the window's
     * bottom bar (verified: a mainView-only scan finds just the gesture buttons). Walk the whole
     * window contentView; fall back to mainView if the pane isn't in a window yet. */
    id win = ((id (*)(id, SEL))objc_msgSend)(mv, sel_registerName("window"));
    id root = win ? ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("contentView")) : mv;
    if (root) { walk_hide_battery_button(root, 0); gLoggedButtons = 1; mavericks_render_battery(root); mavericks_inject_about_tab(root); mavericks_about_refresh(root); }
    /* NB: the /tmp/mavericks_pane_dump view-tree dump lives ONLY in the aux tick (front window) so it works
     * on any pane, incl. Bluetooth; a second trigger here would consume the flag first on the Trackpad
     * pane and dump the wrong window. */
}

/* ============================================================================================
 * 6. BLUETOOTH-PANE MT2 ICON — device-tied trackpad art
 * --------------------------------------------------------------------------------------------
 * The MT2's CoD (major 5, minor 0x25) misses IOBluetoothDeviceImageVault, so the BT pane draws the
 * generic BT logo (RE: docs/mt-stack/explanation.md "Picture"). Fix at the source: swizzle
 * -[IOBluetoothDevice image]/deviceIcon to return Apple's trackpad art for (5,0x25) — device-tied,
 * consistent in every view/state like Magic Mouse; a genuine MT1 (different minor) is untouched.
 * ============================================================================================ */

static id (*gOrigDeviceIcon)(id, SEL)    = NULL;   /* saved -[IOBluetoothDevice deviceIcon] */
static id (*gOrigGetDeviceIcon)(id, SEL) = NULL;   /* saved -[IOBluetoothDevice getDeviceIcon] */
static id (*gOrigImage)(id, SEL)         = NULL;   /* saved -[IOBluetoothDevice image] (row icon accessor) */

/* Our MT2 art, prepared like the vault does for bundle-loaded images (scalable, 32x32 pt row). */
static id mavericks_trackpad_image(void) {
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

/* The MT2 row's battery glyph in the Bluetooth pane's device list is a BT_BatteryControl the pane
 * SHOWS (hidden bound to objectValue.showBattery) but never feeds a value for our device — it sits at
 * -1 (empty), the same stale pattern as the Trackpad pane's control (RE: the view-tree dump). We set
 * it ourselves to the device's battery fraction, matched to the MT2 row by its "Magic Trackpad 2"
 * label. Value scale matches the Trackpad control (0.0-1.0). */
static void set_bt_battery_control(id view, int depth, float frac) {
    if (!view || depth > 8) return;
    if (strstr(object_getClassName(view), "BT_BatteryControl")) {
        ((void (*)(id, SEL, float))objc_msgSend)(view, sel_registerName("setFloatValue:"), frac);
        return;
    }
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < n; i++)
        set_bt_battery_control(((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i), depth + 1, frac);
}
static void mavericks_paint_bt_list_battery(id view, int depth, float frac) {
    if (!view || depth > 16) return;
    Class cellCls = objc_getClass("NSTableCellView");
    if (cellCls && ((signed char (*)(id, SEL, Class))objc_msgSend)(view, sel_registerName("isKindOfClass:"), cellCls)
        && cell_is_mt2(view, 0)) {
        set_bt_battery_control(view, 0, frac);   /* the MT2 row — set its control, don't recurse further */
        return;
    }
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    unsigned long n = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < n; i++)
        mavericks_paint_bt_list_battery(((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i), depth + 1, frac);
}

/* The image swizzle may install AFTER the pane already bound the row icons (the osax loads once the
 * pane is up), so the MT2 row shows the stale original the binding cached (reloadData + KVO pokes
 * don't re-fire it). Fix: re-establish the image view's "value" binding to the SAME device-tied
 * keypath (objectValue.device.deviceIcon) — that forces a fresh read, now through our swizzle -> our
 * art. Not per-view painting: we reuse the pane's own binding + the device's own deviceIcon; from
 * here Apple re-reads deviceIcon (our value) on every future refresh, so it stays correct. One-shot. */
static int gIconRebound = 0;
static void kvo_refresh_mavericks_icon(id view, int depth) {
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
        kvo_refresh_mavericks_icon(((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i), depth + 1);
}

/* deviceIcon/image swizzle body: trackpad art for the MT2 CoD (5,0x25), else the original. */
static id mavericks_icon_for(id self, id (*orig)(id, SEL), SEL _cmd) {
    int mj = ((int (*)(id, SEL))objc_msgSend)(self, sel_registerName("getDeviceClassMajor"));
    int mn = ((int (*)(id, SEL))objc_msgSend)(self, sel_registerName("getDeviceClassMinor"));
    if (mj == 5 && mn == 0x25) { id img = mavericks_trackpad_image(); if (img) return img; }
    return orig ? orig(self, _cmd) : NULL;
}
static id my_deviceIcon(id self, SEL _cmd)    { return mavericks_icon_for(self, gOrigDeviceIcon, _cmd); }
static id my_getDeviceIcon(id self, SEL _cmd) { return mavericks_icon_for(self, gOrigGetDeviceIcon, _cmd); }
static id my_image(id self, SEL _cmd)         { return mavericks_icon_for(self, gOrigImage, _cmd); }

/* The row's icon binds to objectValue.device.deviceIcon, but the value it displays is
 * -[IOBluetoothDevice image] (the generic 512x512 for the MT2; RE'd via a method enumeration —
 * deviceIcon returns 243x243 but the binding shows `image`). Swizzle `image` (the real accessor) so
 * the MT2 shows our trackpad art device-tied, consistent in every view/state like Magic Mouse; also
 * swizzle deviceIcon/getDeviceIcon for any direct/KVC callers. All scoped to the MT2 CoD (5,0x25).
 * IOBluetoothUI loads LAZILY (only when the BT pane opens), so this is idempotent and driven from a
 * dyld add-image hook (which replays already-loaded images at registration) + the aux tick as a belt. */
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
static void mavericks_dyld_added(const struct mach_header *mh, intptr_t slide) {
    (void)mh; (void)slide; install_device_icon_swizzle("dyld");
}

/* ============================================================================================
 * 7. VIEW-TREE DUMP — on-demand RE aid
 * ============================================================================================ */

/* `touch /tmp/mavericks_pane_dump` -> the next aux tick dumps the whole window view tree (class, hidden,
 * frame, and any title/stringValue/image) to syslog, once, then removes the flag. Used to map the
 * battery-row controls / icon views in each transport state. */
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
    /* Binding hunt: what keypath does this view bind value/image/hidden to? Reveals what a glyph reads
     * (e.g. the BT device-list battery indicator -> objectValue.device.<something>). */
    char bindinfo[192]; bindinfo[0] = 0;
    if (responds(view, sel_registerName("infoForBinding:"))) {
        CFStringRef bkeys[3] = { CFSTR("value"), CFSTR("image"), CFSTR("hidden") };
        const char  *bnames[3] = { "value", "image", "hidden" };
        char *bp = bindinfo; int rem = (int)sizeof(bindinfo);
        for (int b = 0; b < 3; b++) {
            id info = ((id (*)(id, SEL, id))objc_msgSend)(view, sel_registerName("infoForBinding:"), (id)bkeys[b]);
            if (!info) continue;
            id kp = ((id (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("objectForKey:"), (id)CFSTR("NSObservedKeyPath"));
            const char *kps = kp ? ((const char *(*)(id, SEL))objc_msgSend)(kp, sel_registerName("UTF8String")) : NULL;
            if (kps && rem > 4) { int w = snprintf(bp, (size_t)rem, " %s->%s", bnames[b], kps); if (w > 0) { bp += w; rem -= w; } }
        }
    }
    LOG("tree %s%s hidden=%d val=%.2f \"%s\"%s%s", pad, cn, hid, fv, txt ? txt : "", imginfo, bindinfo);
    id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    if (!subs) return;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++)
        dump_view_tree(((id (*)(id, SEL, unsigned long))objc_msgSend)(
            subs, sel_registerName("objectAtIndex:"), i), depth + 1);
}

/* ============================================================================================
 * 7b. NAME MIRROR — pane Rename -> on-device name (HID Feature report 0x55)
 * ============================================================================================
 * The Bluetooth pane's right-click Rename writes ONLY the host `displayName` alias (the pane binary
 * never touches the device; blued persists the alias). To make a rename FOLLOW the device we mirror:
 * watch the MT2's displayName; when the user changes it, push it onto the device as the persistent
 * name and then clear the alias so the on-device name shows through. Mechanism (RE'd + proven on-device
 * 2026-07-05, docs/mt-stack/explanation.md "Rename routing + the mirror"): the name store is the MT2's
 * one declared Feature report, the 64-byte vendor report 0x55; SET_REPORT(Feature,0x55,[id][name])
 * writes it verbatim (NVRAM, follows the device across hosts). remoteNameRequest: refreshes the host
 * `Name` cache live; setDisplayName:nil clears the alias. The enable report is 0xF1 (untouched).
 *
 * SAFETY: the SET_REPORT runs inside System Preferences (the process in the 2026-07-04 getReport-panic
 * backtrace). The write self-gates to a present BLUETOOTH-transport MT2 (mavericks_name_write_onboard skips
 * USB, which has no room anyway); since the MT2 drives ONE transport at a time (cabling USB drops BT),
 * "a BT HID is present" means no USB bring-up storm is in flight. We additionally skip a known in-flight
 * transition (presence_observer_state==PRESENCE_HOLD). So a single deliberate write never lands during churn. NB the
 * gate is NOT presence==PRESENCE_BT: renames happen on the Bluetooth pane, where the Trackpad-pane SM that drives
 * the presence observer may never have armed (state==PRESENCE_NONE) — BT presence is the correct, pane-independent signal. */

/* The paired MT2 IOBluetoothDevice (CoD 0x594 = peripheral+pointing, digitizer minor 0x25), or NULL.
 * IOBluetooth is resident whenever the BT pane has been shown (where Rename happens). */
static id mavericks_bt_device(void) {
    Class c = objc_getClass("IOBluetoothDevice");
    if (!c) return NULL;
    id arr = ((id (*)(Class, SEL))objc_msgSend)(c, sel_registerName("pairedDevices"));
    if (!arr) return NULL;
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(arr, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++) {
        id d = ((id (*)(id, SEL, unsigned long))objc_msgSend)(arr, sel_registerName("objectAtIndex:"), i);
        unsigned cod = ((unsigned (*)(id, SEL))objc_msgSend)(d, sel_registerName("getClassOfDevice"));
        if (mt2_cod_is_mt2(cod)) return d;   /* mask service bits (live CoD may be 0x2594) */
    }
    return NULL;
}

/* SET_REPORT(Feature, 0x55, [id][name]) onto the MT2 over Bluetooth (USB has no room:
 * MaxFeatureReportSize==1). Returns 1 if the write landed. Mirrors tools/mavericks_name_write.c. */
static int mavericks_name_write_onboard(const char *name) {
    size_t nl = name ? strlen(name) : 0;
    if (nl == 0 || nl > 63) return 0;
    int ok = 0;
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) return 0;
    { int pid = 0x0265;
      CFNumberRef pidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
      CFMutableDictionaryRef mm = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (mm && pidNum) CFDictionarySetValue(mm, CFSTR(kIOHIDProductIDKey), pidNum);
      IOHIDManagerSetDeviceMatching(mgr, mm);
      if (pidNum) CFRelease(pidNum);
      if (mm) CFRelease(mm); }
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (devs) {
        CFIndex n = CFSetGetCount(devs);
        IOHIDDeviceRef stack[64];
        if (n > 0 && n <= 64) {
            CFSetGetValues(devs, (const void **)stack);
            for (CFIndex i = 0; i < n && !ok; i++) {
                CFStringRef tr = (CFStringRef)IOHIDDeviceGetProperty(stack[i], CFSTR(kIOHIDTransportKey));
                if (!tr || CFStringCompare(tr, CFSTR("Bluetooth"), 0) != kCFCompareEqualTo) continue;
                if (IOHIDDeviceOpen(stack[i], kIOHIDOptionsTypeNone) != kIOReturnSuccess) continue;
                uint8_t buf[64]; memset(buf, 0, sizeof buf);
                buf[0] = 0x55; memcpy(buf + 1, name, nl);
                IOReturn r = IOHIDDeviceSetReport(stack[i], kIOHIDReportTypeFeature, 0x55, buf, (CFIndex)(1 + nl));
                if (r == kIOReturnSuccess) ok = 1;
                else LOG("name-mirror: SET_REPORT 0x55 error 0x%08x", r);
                IOHIDDeviceClose(stack[i], kIOHIDOptionsTypeNone);
            }
        }
        CFRelease(devs);
    }
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return ok;
}

static char gLastDisplayName[128];   /* last displayName we've reconciled (baseline for change-detect) */
static int  gNameMirrorInit = 0;     /* seeded the baseline (never act on the value first seen) */
static int  gNameDeferLogged = 0;    /* logged a pending rename we're holding for BT (avoid spam) */

/* One reconcile pass, called from the aux tick (~1.5s). Detects a user Rename (displayName change),
 * pushes it onboard, refreshes the host name, and clears the alias. Read-only + idempotent otherwise. */
static void mavericks_name_mirror_tick(void) {
    id d = mavericks_bt_device();
    if (!d) return;
    id dn = ((id (*)(id, SEL))objc_msgSend)(d, sel_registerName("getDisplayName"));
    const char *s = dn ? ((const char *(*)(id, SEL))objc_msgSend)(dn, sel_registerName("UTF8String")) : "";
    if (!s) s = "";

    if (!gNameMirrorInit) {                       /* first sight: baseline only, never act */
        strlcpy(gLastDisplayName, s, sizeof gLastDisplayName);
        gNameMirrorInit = 1;
        return;
    }
    if (strcmp(s, gLastDisplayName) == 0) return; /* no change */

    if (s[0] == 0) {                              /* cleared (probably by us after a push) — accept baseline */
        strlcpy(gLastDisplayName, s, sizeof gLastDisplayName);
        gNameDeferLogged = 0;
        return;
    }
    /* A real user rename to a non-empty name. */
    if (presence_observer_state(gObs) == PRESENCE_HOLD) {                        /* known transport transition in flight — never write during churn */
        if (!gNameDeferLogged) { LOG("name-mirror: pending rename \"%s\" (transition in flight) — waiting", s); gNameDeferLogged = 1; }
        return;                                   /* leave baseline unchanged so we retry next tick */
    }
    /* Cap to the 63-byte report payload (0x55 is 64 bytes incl. the report id) on a UTF-8 char
     * boundary — never split a multibyte char, and never let an over-long rename wedge us in an
     * unwritable-forever retry. The host reads the full length back (no 32-char cap on read). */
    char wname[64];
    size_t wl = strlen(s);
    if (wl > 63) { wl = 63; while (wl > 0 && ((unsigned char)s[wl] & 0xC0) == 0x80) wl--; }  /* back off a continuation byte */
    memcpy(wname, s, wl); wname[wl] = 0;

    LOG("name-mirror: displayName \"%s\" -> \"%s\"; writing onboard (0x55)", gLastDisplayName, wname);
    if (mavericks_name_write_onboard(wname)) {          /* self-gates to a present Bluetooth-transport MT2 */
        ((int (*)(id, SEL, id))objc_msgSend)(d, sel_registerName("remoteNameRequest:"), NULL); /* refresh host name cache */
        ((void (*)(id, SEL, id))objc_msgSend)(d, sel_registerName("setDisplayName:"), NULL);    /* clear alias -> on-device name shows */
        strlcpy(gLastDisplayName, s, sizeof gLastDisplayName);   /* mark THIS displayName handled: if the clear ever fails, we won't rewrite in a loop */
        gNameDeferLogged = 0;
        LOG("name-mirror: pushed \"%s\" onboard + refreshed + cleared alias", wname);
    } else {                                       /* no BT MT2 present (or error) — leave baseline, retry next tick */
        if (!gNameDeferLogged) { LOG("name-mirror: no BT MT2 present or write failed — will retry"); gNameDeferLogged = 1; }
    }
}

/* ============================================================================================
 * 8. AUX TICK — front-window belt (any pane, incl. Bluetooth; gPane only tracks Trackpad)
 * ============================================================================================ */

/* Reassert everything we inject into WHATEVER pane is front — the BT-icon swizzle + rebind, and the
 * Bluetooth device-list battery glyph. Called on every pane show (willSelect/didSelect) and once at
 * startup, NOT only on the aux tick, so none of it waits up to a tick to appear. */
static void aux_reassert(void) {
    install_device_icon_swizzle("reassert");          /* belt: ensure the image swizzle is in */
    kvo_refresh_mavericks_icon(front_window_content(), 0);  /* re-read if swizzled after the pane bound it */
    /* Bluetooth pane device-list battery glyph (the pane leaves it empty for the MT2). */
    int ov = mavericks_batt_override();
    int gpct = (ov >= 0 && ov <= 100) ? ov : mavericks_read_node_battery();
    if (gpct >= 1 && gpct <= 100) gDevicePct = gpct;
    if (gDevicePct >= 0) mavericks_paint_bt_list_battery(front_window_content(), 0, (float)gDevicePct / 100.0f);
}

/* Keep the BT-icon swizzle installed + serviced, and run the on-demand view-tree dump for WHATEVER
 * pane is front. Distinct from capture_pane's 2s reconcile tick (which needs the captured pane). */
static dispatch_source_t gAuxTick = NULL;
static void aux_tick_start(void) {
    if (gAuxTick) return;
    gAuxTick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(gAuxTick, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              (uint64_t)(1.5 * NSEC_PER_SEC), (uint64_t)(0.25 * NSEC_PER_SEC));
    dispatch_source_set_event_handler(gAuxTick, ^{
        aux_reassert();                                  /* belt for the front-window UI */
        mavericks_name_mirror_tick();                          /* pane Rename -> on-device name (report 0x55) */
        if (access("/tmp/mavericks_pane_dump", F_OK) == 0) {
            unlink("/tmp/mavericks_pane_dump");
            /* Walk EVERY window's contentView (not just the front one) — the Bluetooth pane's device
             * list can live in a window the front-window probe missed, which is why an earlier dump
             * came up empty. Each window is labelled by title. */
            LOG("tree dump BEGIN (all windows)");
            Class appc = objc_getClass("NSApplication");
            id nsapp = appc ? ((id (*)(Class, SEL))objc_msgSend)(appc, sel_registerName("sharedApplication")) : NULL;
            id wins = nsapp ? ((id (*)(id, SEL))objc_msgSend)(nsapp, sel_registerName("windows")) : NULL;
            unsigned long wn = wins ? ((unsigned long (*)(id, SEL))objc_msgSend)(wins, sel_registerName("count")) : 0;
            for (unsigned long i = 0; i < wn; i++) {
                id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
                const char *wt = "";
                if (responds(w, sel_registerName("title"))) {
                    id t = ((id (*)(id, SEL))objc_msgSend)(w, sel_registerName("title"));
                    if (t) wt = ((const char *(*)(id, SEL))objc_msgSend)(t, sel_registerName("UTF8String"));
                }
                signed char vis = ((signed char (*)(id, SEL))objc_msgSend)(w, sel_registerName("isVisible"));
                LOG("tree window[%lu] \"%s\" visible=%d", i, wt ? wt : "", vis);
                id cv = ((id (*)(id, SEL))objc_msgSend)(w, sel_registerName("contentView"));
                if (cv) dump_view_tree(cv, 0);
            }
            LOG("tree dump END");
        }
    });
    dispatch_resume(gAuxTick);
}

/* Force the trackpad view for our all-synthetic (Voodoo satellite) AMD. Apple's `-[Trackpad loadMainView]`
 * detect is CLASS-based: it sets `mFoundBTTrackpad` only for a genuine `BNBTrackpadDevice`/
 * `AppleUSBMultitouchDriver` (RE'd — decisions.md "pane detect is CLASS-based"), then picks the nib
 * "MTTrackpadController" iff that ivar is set, else the NoTrackpad base nib. Our fabricated
 * `AppleMultitouchDevice` matches none, so the pane would show NoTrackpad. We are the sole presence
 * authority (the reader classes via `gObs`); when a device is present, remap any non-trackpad nib request
 * to "MTTrackpadController" so the pane instantiates the real trackpad controller. This is DOWNSTREAM of
 * loadMainView's detect (the ivar it read is already spent), so nothing overwrites it. The ① fabricate-AMD
 * terminal loses identity above the AMD by design; this is that terminal's pane-force (decisions.md
 * "Terminal design space"). */
static id (*gOrigControlerForNIB)(id, SEL, id) = NULL;
static id my_controlerForNIB(id self, SEL _cmd, id nibName) {
    if (nibName && gObs && presence_observer_state(gObs) != PRESENCE_NONE) {
        signed char isMT = ((signed char (*)(id, SEL, id))objc_msgSend)(
            nibName, sel_registerName("isEqualToString:"), (id)CFSTR("MTTrackpadController"));
        if (!isMT) {
            LOG("controlerForNIB: device present -> forcing MTTrackpadController (was other nib)");
            nibName = (id)CFSTR("MTTrackpadController");
        }
    }
    return gOrigControlerForNIB(self, _cmd, nibName);
}

/* ============================================================================================
 * 9. PANE CAPTURE + NAVIGATION SWIZZLES
 * ============================================================================================ */

/* Capture the live Trackpad pane: swizzle the pane's deviceConnected: (NoTrackpad suppression) and
 * MTTrackpadController's _magicTrackpadAction (own the render), arm the USB+BT observers, reconcile
 * to current truth, and start the 2s reconcile/button-hide tick. */
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
    if (!gOrigControlerForNIB) {
        Class c = object_getClass(self);
        SEL sel = sel_registerName("_controlerForNIBName:");
        Method m = class_getInstanceMethod(c, sel);
        if (m) {
            gOrigControlerForNIB = (id (*)(id, SEL, id))method_getImplementation(m);
            method_setImplementation(m, (IMP)my_controlerForNIB);
            LOG("swizzled _controlerForNIBName: (force trackpad view for synthetic AMD)");
        } else {
            LOG("_controlerForNIBName: not found (trackpad-view force disabled)");
        }
    }
    install_battery_timer_swizzle("capture");   /* belt; willSelect installs it earlier (no flash) */
    if (!gObs) gObs = presence_observer_create(CFRunLoopGetCurrent(), 1300, on_presence, NULL);
    presence_observer_reconcile(gObs);   /* sync to current truth immediately */
    static dispatch_source_t tick;
    if (!tick) {
        tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                                  2 * NSEC_PER_SEC, (uint64_t)(0.25 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(tick, ^{ presence_observer_reconcile(gObs); hide_battery_button_now(); });
        dispatch_resume(tick);
    }
    hide_battery_button_now();   /* run once NOW (button-hide + About-tab injection) so they don't wait
                                  * up to 2 s for the first tick on a fresh pane open */
}

/* Swizzled -[NSPreferencePane willSelect] (fires BEFORE didSelect): install the image swizzle here,
 * before the pane populates its device list, so the MT2 row's first draw already reads our art (no
 * bowtie->trackpad flash). didSelect proved too late — the Bluetooth pane populates earlier. */
typedef void (*didSelect_t)(id, SEL);
static didSelect_t gOrigDidSelect  = NULL;
static didSelect_t gOrigWillSelect = NULL;
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
    aux_reassert();   /* any pane just appeared -> reassert the front-window UI NOW (icon + glyph), not on the next tick */
}

/* Capture-race fix: if System Preferences was ALREADY showing Trackpad when we injected (its
 * didSelect fired before our swizzle), proactively ask it for the current pane and capture it. The
 * controller exposes -currentPrefPaneInstance (RE'd), reachable via the app delegate. Retried a few
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

/* ============================================================================================
 * 10. INJECTION + SINGLE-LOAD ACTIVATION — the entry points
 * ============================================================================================ */

/* Install the pane-navigation swizzles (willSelect fires before didSelect, so the icon swizzle is in
 * before the Bluetooth pane populates its device list — kills the first-open icon flash). */
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
    SEL wsel = sel_registerName("willSelect");
    Method wm = class_getInstanceMethod(c, wsel);
    if (wm && !gOrigWillSelect) {
        gOrigWillSelect = (didSelect_t)method_getImplementation(wm);
        method_setImplementation(wm, (IMP)my_willSelect);
        LOG("swizzled -[NSPreferencePane willSelect]");
    }
    install_device_icon_swizzle("inject");   /* device-tied BT-pane MT2 icon (if IOBluetooth already loaded) */
}

/* THE single activation choke point — the whole-payload owner decision, no per-swizzle granularity.
 * The payload loads once, as a SIMBL plugin, via its [bundle load] constructor, which reaches here and
 * installs the swizzles. There is exactly ONE loader now (SIMBL; the standalone osax was retired), and
 * SIMBL injects a plugin once per process, so no cross-loader arbitration is needed — gActivated alone
 * keeps activation idempotent within the image. */
static int gActivated = 0;
static void mavericks_activate(const char *via) {
    if (gActivated) return;                    /* already active in THIS image (idempotent) */
    gActivated = 1;
    LOG("payload active in pid %d (via %s)", getpid(), via);
    install_swizzle();
    /* Device-tied BT-pane icon: install the deviceIcon swizzle the moment IOBluetooth loads (the hook
     * also replays already-loaded images), so it is in before the pane first reads deviceIcon. */
    _dyld_register_func_for_add_image(&mavericks_dyld_added);
    /* Aux tick over the front window (works for any pane, incl. Bluetooth — gPane only tracks
     * Trackpad): swizzle belt + the on-demand /tmp/mavericks_pane_dump view-tree dump. */
    dispatch_async(dispatch_get_main_queue(), ^{ aux_tick_start(); });
    /* Handle the direct-open case (already on Trackpad): retry a proactive capture on the main queue
     * once the app/pane are up. didSelect still handles navigation. */
    int delays[3] = {300, 1200, 3000};
    for (int i = 0; i < 3; i++) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delays[i] * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{ try_capture_current(); });
    }
}

/* Runs the instant our image loads into the host process (SIMBL [bundle load]). */
__attribute__((constructor))
static void mavericks_image_loaded(void) {
    mavericks_activate("constructor");
}
