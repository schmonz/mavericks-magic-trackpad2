/* MT2BTReader — in-kernel Bluetooth transport for the Magic Trackpad 2.
 *
 * Mirrors the USB path (MT2USBClaim frees the interface; userspace decodes + feeds)
 * but in-kernel, because the MT2's multitouch frames are unreachable over BT from
 * userspace (the BT HID descriptor is boot-mouse-only; see the BT findings doc). We
 * bind the L2CAP channel directly (proven feasible by the bnbinject experiment),
 * enable multitouch with the 0xF1 SET_REPORT (the MT2's command; Apple's stock
 * BNBTrackpadDevice sends the MT1 0xD7 and so never completes), decode each frame
 * (mt2_bt_decode), re-encode as an MT1 report (mt1_encode), and feed the MT2Gesture
 * nub in-process. Decode/encode are the same pure C the USB path uses.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include "bt_l2cap_shim.h"
#include "MT2BTReader.h"
#include "MT2Gesture.h"

/* Compiled as C++ under the kext toolchain (so are mt2_bt_decode.c / mt1_encode.c),
 * so these resolve with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt1_encode.h"

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

static uint32_t uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* Per-connection settle window. Frames are dropped (not fed) until uptime passes this,
 * armed when the reader binds the channel. Suppresses the connect-transition contact
 * burst the device emits at (re)connect, which would otherwise fling the cursor. The
 * MT2 over BT sends NO idle/all-up frames, so a temporal gate is the only honest signal
 * (same conclusion as the USB feeder's debounce). */
#define MT2_BT_SETTLE_MS 2000u
static uint32_t gFeedSettleUntilMs = 0;

/* Deceleration mimic. After this much silence following a contact, the device is done;
 * we then replay the held position twice (zero velocity) and lift, so the engine sees a
 * STOP+lift rather than a flick. IDLE = silence detect; DECEL = spacing between replays. */
#define MT2_BT_IDLE_MS  35
#define MT2_BT_DECEL_MS 20

static void feed_frame(const touch_frame_t *tf) {
    uint8_t mt1[256];
    int n = mt1_encode(tf, mt1, sizeof(mt1), uptime_ms());
    if (n > 0 && gActiveMT2Gesture) gActiveMT2Gesture->feedFrame(mt1, (unsigned int)n);
}

void com_schmonz_MT2BTReader::incomingData(IOService *target,
                                           IOBluetoothL2CAPChannel *channel,
                                           unsigned short length, void *data) {
    const uint8_t *b = (const uint8_t *)data;
    if (!b || length < 2) return;

    (void)channel;
    /* On the wire each interrupt frame is 0xA1 (HID-over-BT input header) + the HID
     * report (report id 0x31). Strip a leading 0xA1 if listenAt delivers it. */
    const uint8_t *report = b;
    size_t rlen = length;
    if (b[0] == 0xA1) { report = b + 1; rlen = length - 1; }

    touch_frame_t tf;
    if (mt2_bt_decode(report, rlen, &tf) != 0) return;

    /* Settle gate: ignore frames for MT2_BT_SETTLE_MS after the reader binds, so the
     * device's connect-transition contact burst can't fling the cursor. */
    if (uptime_ms() < gFeedSettleUntilMs) return;

    /* Drop lifted contacts (size 0). */
    int kept = 0;
    for (int i = 0; i < tf.ntouches; i++) {
        if (tf.touches[i].size > 0) {
            if (kept != i) tf.touches[kept] = tf.touches[i];
            kept++;
        }
    }
    tf.ntouches = kept;

    com_schmonz_MT2BTReader *self = OSDynamicCast(com_schmonz_MT2BTReader, target);

    if (kept > 0) {
        /* Real contact: feed it, remember it for the held replay, and (re)arm the idle
         * timer. While the gesture continues, each frame pushes the timer out. */
        feed_frame(&tf);
        if (self) {
            self->fHeldFrame = tf;
            self->fDecelStep = 0;
            if (self->fIdleTimer) self->fIdleTimer->setTimeoutMS(MT2_BT_IDLE_MS);
        }
    } else {
        /* The device's liftoff frame. Do NOT feed an abrupt zero-contact frame here
         * (that reads as a flick → momentum). Let the idle timer run the deceleration:
         * replay the held position at zero velocity, then a clean lift. */
        if (self) {
            self->fDecelStep = 0;
            if (self->fIdleTimer) self->fIdleTimer->setTimeoutMS(MT2_BT_IDLE_MS);
        }
    }
}

/* On silence after a contact: replay the held position (zero velocity) so the engine
 * registers the finger as STOPPED, then feed a clean lift. No momentum, no phantom. */
void com_schmonz_MT2BTReader::idleTimeout(OSObject *owner, IOTimerEventSource * /*sender*/) {
    com_schmonz_MT2BTReader *self = OSDynamicCast(com_schmonz_MT2BTReader, owner);
    if (!self) return;

    if (self->fDecelStep < 2) {
        feed_frame(&self->fHeldFrame);          /* held: same position -> zero velocity */
        self->fDecelStep++;
        if (self->fIdleTimer) self->fIdleTimer->setTimeoutMS(MT2_BT_DECEL_MS);
    } else if (self->fDecelStep == 2) {
        touch_frame_t lift;
        lift.ntouches = 0;
        lift.button = 0;
        lift.timestamp = 0;
        feed_frame(&lift);                       /* clean lift -> engine releases */
        self->fDecelStep = 3;                    /* done; do not rearm */
    }
}

/* Runs in-gate (the channel's Bluetooth workloop): the only place IOBluetoothFamily
 * allows IOBluetoothObject calls. arg0 is the reader. */
IOReturn com_schmonz_MT2BTReader::setupInGate(OSObject * /*owner*/, void *arg0,
                                              void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;

    unsigned short psm = self->fChannel->getPSM();

    /* Arm the settle window from bind time: drop frames until it passes so the
     * connect-transition contact burst doesn't reach the gesture engine. */
    gFeedSettleUntilMs = uptime_ms() + MT2_BT_SETTLE_MS;

    /* The multitouch frames arrive on the interrupt channel (PSM 19); listen on all the
     * channels we win — harmless, and the decoder rejects non-0x31 reports. */
    self->fChannel->listenAt(self, &com_schmonz_MT2BTReader::incomingData);

    /* Enable multitouch streaming on the CONTROL channel only (PSM 17 = 0x11). The
     * MT2's Bluetooth enable is feature report 0xF1 {0xF1,0x02,0x01} (confirmed via
     * IOHIDDeviceSetReport). Over raw L2CAP a SET_REPORT(feature) is the HIDP
     * transaction byte (SET_REPORT<<4)|FEATURE = 0x53 then the report. */
    if (psm == 0x11) {
        static const uint8_t kEnable[] = { 0x53, 0xF1, 0x02, 0x01 };
        self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
    }
    IOLog("MT2BTReader: setup on PSM=%u (enable sent=%d)\n", psm, psm == 0x11);
    return kIOReturnSuccess;
}

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;
    fIdleTimer = 0;
    fDecelStep = 0;

    /* Deceleration timer on the channel's workloop (serializes with incomingData/feed). */
    IOWorkLoop *wl = fChannel->getWorkLoop();
    if (wl) {
        fIdleTimer = IOTimerEventSource::timerEventSource(this, &com_schmonz_MT2BTReader::idleTimeout);
        if (fIdleTimer && wl->addEventSource(fIdleTimer) != kIOReturnSuccess) {
            fIdleTimer->release();
            fIdleTimer = 0;
        }
    }

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
    if (self && self->fChannel) self->fChannel->listenAt(self, 0);
    return kIOReturnSuccess;
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    /* Tear down the idle timer first: cancel any pending fire and remove it from the
     * workloop before we can be freed, or it could fire on a freed object. */
    if (fIdleTimer) {
        fIdleTimer->cancelTimeout();
        IOWorkLoop *wl = fChannel ? fChannel->getWorkLoop() : 0;
        if (wl) wl->removeEventSource(fIdleTimer);
        fIdleTimer->release();
        fIdleTimer = 0;
    }

    /* Deregister our incoming-data callback in-gate BEFORE we can be freed, or the
     * channel's newDataIn will dereference a dangling pointer (use-after-free panic
     * seen when the installer unloaded the live kext while the trackpad streamed). */
    if (fChannel) {
        IOCommandGate *gate = fChannel->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::teardownInGate, this);
    }
    fChannel = 0;
    IOService::stop(provider);
}
