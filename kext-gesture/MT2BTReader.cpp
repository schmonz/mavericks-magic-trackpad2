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
     * connect-transition contact burst) and all post-decode logic. */
    if (gActiveMT2Gesture) gActiveMT2Gesture->connectionEstablished(self, MT2_EVENT_DRIVEN);

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
