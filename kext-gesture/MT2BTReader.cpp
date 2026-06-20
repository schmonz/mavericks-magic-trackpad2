/* MT2BTReader — in-kernel Bluetooth transport for the Magic Trackpad 2.
 *
 * A thin decoder. The MT2's multitouch frames are unreachable over BT from userspace
 * (the BT HID descriptor is boot-mouse-only; see the BT findings doc). We bind the
 * L2CAP channel directly (proven feasible by the bnbinject experiment), enable
 * multitouch with the 0xF1 SET_REPORT (the MT2's command; Apple's stock
 * BNBTrackpadDevice sends the MT1 0xD7 and so never completes), decode each frame
 * (mt2_bt_decode), and push the touch_frame_t to the MT2Gesture nub via
 * submitFrame(MT2_EVENT_DRIVEN). The shared mt2_session owns the settle gate,
 * lift-drop, deceleration/clean-lift, and click — there is no decision logic here.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include "bt_l2cap_shim.h"
#include "MT2BTReader.h"
#include "MT2Gesture.h"

/* Compiled as C++ under the kext toolchain (so is mt2_bt_decode.c), so these resolve
 * with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt2_pipeline.h"   /* MT2_EVENT_DRIVEN */

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

/* PHASE-1 EXPERIMENT FLAG (branch genuine-bnbtrackpad-citizen). When true, our reader does NOT
 * register its incoming-data delegate (listenAt) — so the single channel delegate slot is free for
 * a forming Apple BNBTrackpadDevice to own, eliminating the reader-vs-BNB delegate-slot race (the one
 * real panic vector of the ID-injection FORM test). Side effect: OUR data path is inert while set, so
 * the trackpad does NOT move the cursor during this test — expected; reverts on reload. Set false (or
 * delete) to restore normal operation. */
static const bool kGenuineBnbFormTest = true;

/* PHASE-A EXPERIMENT FLAG (branch genuine-bnbtrackpad-citizen, findings S2.2c). IOKit matching can't
 * be tricked into binding Apple's BNBTrackpadDevice to our channel (matchesDevicePropertyInController
 * reads the real DID 76/613 from controller-internal state, not the channel's registry property). So
 * we BYPASS matching: on the control channel, manually allocClassWithName -> init -> attach -> start a
 * genuine BNBTrackpadDevice directly on the real L2CAP channel. start() runs the full IOHIDDevice start
 * chain on a REAL provider, so the IOHIDDevice internal state initializes (no publish-handler panic).
 * FORM-only: input not wired (BNB can't parse MT2's 0x31 yet); a non-panicking "device registered + pane
 * lit" is success. Requires kGenuineBnbFormTest too (our delegate must be off so BNB owns the data slot). */
static const bool kGenuineBnbManualStart = true;

void com_schmonz_MT2BTReader::incomingData(IOService *target,
                                           IOBluetoothL2CAPChannel *channel,
                                           unsigned short length, void *data) {
    (void)channel;
    const uint8_t *b = (const uint8_t *)data;
    if (!b || length < 2) return;
    const uint8_t *report = b;
    size_t rlen = length;
    if (b[0] == 0xA1) { report = b + 1; rlen = length - 1; }   /* strip transport byte */
    touch_frame_t tf;
    if (mt2_bt_decode(report, rlen, &tf) != 0) return;
    if (gActiveMT2Gesture) gActiveMT2Gesture->submitFrame(target, &tf);   /* target = this reader */
}

/* Runs in-gate (the channel's Bluetooth workloop): the only place IOBluetoothFamily
 * allows IOBluetoothObject calls. arg0 is the reader. */
IOReturn com_schmonz_MT2BTReader::setupInGate(OSObject * /*owner*/, void *arg0,
                                              void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;

    unsigned short psm = self->fChannel->getPSM();

    /* Arm the shared session from bind time: it owns the settle window (drops the
     * connect-transition contact burst) and all post-decode logic. Only the INTERRUPT
     * channel (PSM 19 = 0x13) delivers touch frames, so only it registers as the
     * session's active frame source. BT binds two L2CAP channels (control 0x11 +
     * interrupt 0x13) as two separate reader instances; if the control reader claimed
     * the source instead, the session's single-active guard would reject the interrupt
     * reader's frames (== dead cursor). The control channel only sends the enable below. */
    if (psm == 0x13 && gActiveMT2Gesture)
        gActiveMT2Gesture->connectionEstablished(self, MT2_EVENT_DRIVEN);

    /* The multitouch frames arrive on the interrupt channel (PSM 19); listen on all the
     * channels we win — harmless, and the decoder rejects non-0x31 reports.
     * FORM TEST: skip our delegate so a forming BNBTrackpadDevice owns the slot uncontended. */
    if (!kGenuineBnbFormTest)
        self->fChannel->listenAt(self, &com_schmonz_MT2BTReader::incomingData);

    /* Enable multitouch streaming on the CONTROL channel only (PSM 17 = 0x11). The
     * MT2's Bluetooth enable is feature report 0xF1 {0xF1,0x02,0x01} (confirmed via
     * IOHIDDeviceSetReport). Over raw L2CAP a SET_REPORT(feature) is the HIDP
     * transaction byte (SET_REPORT<<4)|FEATURE = 0x53 then the report. */
    self->fIsControl = (psm == 0x11);
    if (psm == 0x11) {
        /* Keep sending the MT2's 0xF1 multitouch enable — a genuine BNBTrackpadDevice only knows
         * the MT1 0xD7 enable, which the MT2 ignores, so WITHOUT this the pad never streams. */
        static const uint8_t kEnable[] = { 0x53, 0xF1, 0x02, 0x01 };
        self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
    }
    IOLog("MT2BTReader: setup on PSM=%u (enable sent=%d)\n", psm, psm == 0x11);
    return kIOReturnSuccess;
}

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    fIsControl = false;
    fManualBnb = 0;

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;

    /* IOBluetoothFamily REQUIREs every IOBluetoothObject call to run inside its
     * workloop gate (mWorkLoop->inGate()) — calling sendTo/listenAt directly from this
     * start() thread panics ("NOT called in IOWorkLoop"). Marshal onto the channel's
     * own command gate, which enters that workloop. */
    IOCommandGate *gate = fChannel->getCommandGate();
    if (!gate) {
        IOLog("MT2BTReader: channel has no command gate; cannot enable\n");
        return false;
    }
    gate->runAction(&com_schmonz_MT2BTReader::setupInGate, this);

    /* FORM TEST (S2.2c): on the control channel, manually build a genuine BNBTrackpadDevice on this
     * real L2CAP channel — bypassing IOKit matching (which can't be tricked). Done from this normal
     * start() thread, NOT in the channel command gate: the BT-HID start chain can block waiting for
     * channel handshakes, and blocking while holding the gate would deadlock. start() runs the full
     * IOHIDDevice chain on a real provider, so the IOHIDDevice state initializes (no publish-handler
     * panic). Best-effort; failure leaves our reader untouched. */
    if (kGenuineBnbManualStart && fIsControl) {
        OSObject *bo = OSMetaClass::allocClassWithName("BNBTrackpadDevice");
        IOService *bnb = OSDynamicCast(IOService, bo);
        if (!bnb) {
            IOLog("MT2BTReader: allocClassWithName(BNBTrackpadDevice) NULL (AppleBluetoothMultitouch loaded?)\n");
            if (bo) bo->release();
        } else {
            OSDictionary *props = OSDictionary::withCapacity(2);   /* non-null: init bails on NULL props */
            bool ok = props && bnb->init(props);
            if (props) props->release();
            if (ok && bnb->attach(fChannel)) {
                if (bnb->start(fChannel)) {
                    fManualBnb = bnb;
                    IOLog("MT2BTReader: MANUAL genuine BNBTrackpadDevice start OK (FORM milestone)\n");
                } else {
                    IOLog("MT2BTReader: manual BNBTrackpadDevice start() FAILED\n");
                    bnb->detach(fChannel);
                    bnb->release();
                }
            } else {
                IOLog("MT2BTReader: manual BNBTrackpadDevice init/attach FAILED (init=%d)\n", ok);
                bnb->release();
            }
        }
    }

    IOLog("MT2BTReader: bound L2CAP channel, enabled multitouch (0xF1), listening\n");
    registerService();
    return true;
}

/* In-gate: null our listenAt callback. newDataIn bails when the callback (channel+0x110)
 * is NULL before it derefs the target (channel+0x118), so this makes it safe for the
 * channel to outlive us. Same-target listenAt(self, NULL) is accepted by the family. */
IOReturn com_schmonz_MT2BTReader::teardownInGate(OSObject * /*owner*/, void *arg0,
                                                 void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    /* FORM TEST: we never set our delegate, so don't clear the slot — it may belong to a
     * forming BNBTrackpadDevice; clearing it would clobber Apple's delegate on our unload. */
    if (!kGenuineBnbFormTest && self && self->fChannel) self->fChannel->listenAt(self, 0);
    return kIOReturnSuccess;
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    /* Deregister our incoming-data callback in-gate BEFORE we can be freed, or the
     * channel's newDataIn will dereference a dangling pointer (use-after-free panic
     * seen when the installer unloaded the live kext while the trackpad streamed). */
    if (fChannel) {
        IOCommandGate *gate = fChannel->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::teardownInGate, this);
    }
    /* FORM TEST: tear down the manually-started genuine BNBTrackpadDevice before we go away, or it
     * outlives the channel it was started on. terminate() drives the normal async IOService teardown
     * (detach from provider + stop); release() drops our retained reference. */
    if (fManualBnb) {
        fManualBnb->terminate();
        fManualBnb->release();
        fManualBnb = 0;
        IOLog("MT2BTReader: manual BNBTrackpadDevice terminated + released\n");
    }
    fChannel = 0;
    IOService::stop(provider);
}
