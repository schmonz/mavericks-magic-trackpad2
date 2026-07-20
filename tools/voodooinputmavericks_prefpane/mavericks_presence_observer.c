/* IOKit adapter for mavericks_presence — see mavericks_presence_observer.h. Lifted verbatim from the prefpane
 * osax's former inline sm_event/sm_reconcile/dev_changed/arm_observer so the pane and the USB->BT
 * handoff can share one presence observer. Pure decision logic stays in mavericks_presence (host-tested);
 * this file is the IOKit-bound half (no host test — validated on-device via the transport matrix). */
#include <stdlib.h>
#include <syslog.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "mavericks_presence_observer.h"

#define LOG(...) syslog(LOG_NOTICE, "[MavericksPresence] " __VA_ARGS__)

/* Per-notification refcon: which observer + which SM event this edge maps to (+ a log tag). */
typedef struct { struct mavericks_presence_observer *o; presence_event_t ev; const char *tag; } obs_cbctx_t;

struct mavericks_presence_observer {
    presence_state_t state;   /* the SM state; the observer's only decision state */
    int gen;                  /* serializes the pending HOLD timer (supersession) */
    int removal_ms;           /* the removal-window (coalescing) duration */
    CFRunLoopRef runloop;
    presence_on_transition_t cb;
    void *ctx;
    IONotificationPortRef notify;
    io_iterator_t iters[4];
    obs_cbctx_t cbctx[4];
};

static int service_present(const char *cls) {
    CFMutableDictionaryRef m = IOServiceMatching(cls);
    if (!m) return 0;
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, m);  /* consumes m */
    if (s) { IOObjectRelease(s); return 1; }
    return 0;
}

static void drain(io_iterator_t it) {
    io_object_t o;
    while ((o = IOIteratorNext(it))) IOObjectRelease(o);
}

/* Run one event through the SM and report its action. gen serializes the pending HOLD timer: entering
 * HOLD arms a window timer tagged with `my`; any LATER resolving event bumps gen so the in-flight
 * timer's `my != gen` guard fires and it no-ops (superseded). A no-op event (PRESENCE_ACT_NONE — a
 * duplicate/stale edge) must NOT bump gen: doing so would cancel a live HOLD timer without
 * rescheduling, stranding the SM in PRESENCE_HOLD (reconcile won't resolve HOLD->NONE by design). So
 * only HOLD arms, and only a real resolution supersedes. */
static void obs_sm_event(mavericks_presence_observer_t *o, presence_event_t e) {
    presence_result_t r = presence_step(o->state, e);
    o->state = r.next;
    o->cb(r.action, e, o->ctx);
    if (r.action == PRESENCE_ACT_HOLD) {
        int my = ++o->gen;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)o->removal_ms * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            if (my != o->gen) return;
            presence_result_t rr = presence_step(o->state, PRESENCE_EV_REMOVAL_ELAPSED);
            o->state = rr.next; o->cb(rr.action, PRESENCE_EV_REMOVAL_ELAPSED, o->ctx);
        });
    } else if (r.action != PRESENCE_ACT_NONE) {
        ++o->gen;   /* a real resolution supersedes a pending hold timer; a no-op leaves it alone */
    }
}

/* One callback for every appear/disappear of either transport. The consumer suppresses the pane's own
 * updates (see my_deviceConnected), so WE are the sole driver: each edge becomes one SM event. */
static void obs_dev_changed(void *ref, io_iterator_t it) {
    obs_cbctx_t *c = (obs_cbctx_t *)ref;
    drain(it);
    LOG("device change: %s", c->tag);
    obs_sm_event(c->o, c->ev);
}

mavericks_presence_observer_t *presence_observer_create(void *runloop, int removal_ms,
                                                  presence_on_transition_t cb, void *ctx) {
    mavericks_presence_observer_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->state = PRESENCE_NONE;
    o->removal_ms = removal_ms;
    o->runloop = (CFRunLoopRef)runloop;
    o->cb = cb;
    o->ctx = ctx;
    o->notify = IONotificationPortCreate(kIOMasterPortDefault);
    if (!o->notify) { LOG("IONotificationPortCreate failed"); free(o); return NULL; }
    CFRunLoopAddSource(o->runloop, IONotificationPortGetRunLoopSource(o->notify),
                       kCFRunLoopCommonModes);

    /* Arm live IOKit observers on BOTH transports, FirstMatch + Terminated. IOServiceMatching consumes
     * the dict, so build a fresh one per call. Drain the initial iterators WITHOUT acting (already
     * present services, not live events). */
    /* Post-full-synthetic: no BNBTrackpadDevice / AppleUSBMultitouchDriver exists anymore — we drive our
     * OWN fabricated AMD on both transports. Watch our per-transport READER classes instead: they are the
     * 1:1 transport-presence signal (BT reader binds the L2CAP channel; USB reader binds interface 1). The
     * BT side has TWO reader instances (control PSM17 + interrupt PSM19), so appear/remove fire twice — the
     * SM's duplicate-edge tolerance (PRESENCE_ACT_NONE) coalesces them. */
    static const struct { const char *cls; const char *type; presence_event_t ev; const char *tag; } specs[4] = {
        {"com_schmonz_MT2USBReader", kIOFirstMatchNotification, PRESENCE_EV_USB_APPEAR, "USB+"},
        {"com_schmonz_MT2USBReader", kIOTerminatedNotification, PRESENCE_EV_USB_REMOVE, "USB-"},
        {"com_schmonz_MT2BTReader",  kIOFirstMatchNotification, PRESENCE_EV_BT_APPEAR,  "BT+"},
        {"com_schmonz_MT2BTReader",  kIOTerminatedNotification, PRESENCE_EV_BT_REMOVE,  "BT-"},
    };
    for (int i = 0; i < 4; i++) {
        o->cbctx[i].o = o; o->cbctx[i].ev = specs[i].ev; o->cbctx[i].tag = specs[i].tag;
        IOServiceAddMatchingNotification(o->notify, specs[i].type,
            IOServiceMatching(specs[i].cls), obs_dev_changed, &o->cbctx[i], &o->iters[i]);
        drain(o->iters[i]);
    }
    LOG("armed live observers (USB + BT, first-match + terminated)");
    return o;
}

/* Both current consumers keep their observer for the process lifetime, so this is never called and the
 * HOLD-timer block's captured `o` never dangles. If a caller ever destroys an observer, it MUST do so
 * only when no removal-window timer is pending (else the in-flight dispatch_after block touches freed
 * memory) — bump o->gen won't help since the block reads o itself. Quiesce, then destroy. */
void presence_observer_destroy(mavericks_presence_observer_t *o) {
    if (!o) return;
    if (o->notify) {
        CFRunLoopRemoveSource(o->runloop, IONotificationPortGetRunLoopSource(o->notify),
                              kCFRunLoopCommonModes);
        IONotificationPortDestroy(o->notify);
    }
    for (int i = 0; i < 4; i++) if (o->iters[i]) IOObjectRelease(o->iters[i]);
    free(o);
}

presence_state_t presence_observer_state(const mavericks_presence_observer_t *o) {
    return o ? o->state : PRESENCE_NONE;
}

/* Reconcile against device truth (poll): self-heal for missed/manually-started edges. */
void presence_observer_reconcile(mavericks_presence_observer_t *o) {
    if (!o) return;
    int bt  = service_present("com_schmonz_MT2BTReader");    /* our fabricated-AMD readers = the transport */
    int usb = service_present("com_schmonz_MT2USBReader");   /* signal now (BNB/AppleUSBMultitouch are gone) */
    presence_result_t r = presence_reconcile(o->state, bt, usb);
    if (r.action != PRESENCE_ACT_NONE) LOG("reconcile bt=%d usb=%d -> action", bt, usb);
    o->state = r.next;
    o->cb(r.action, PRESENCE_EV_REMOVAL_ELAPSED, o->ctx);  /* poll correction: no discrete edge */
}
