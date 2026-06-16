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

void com_schmonz_MT2BTReader::incomingData(IOService * /*target*/,
                                           IOBluetoothL2CAPChannel * /*channel*/,
                                           unsigned short length, void *data) {
    const uint8_t *b = (const uint8_t *)data;
    if (!b || length < 2) return;

    /* On the wire each interrupt frame is 0xA1 (HID-over-BT input header) + the HID
     * report (report id 0x31). Strip a leading 0xA1 if listenAt delivers it. */
    const uint8_t *report = b;
    size_t rlen = length;
    if (b[0] == 0xA1) { report = b + 1; rlen = length - 1; }

    touch_frame_t tf;
    if (mt2_bt_decode(report, rlen, &tf) != 0) return;

    uint8_t mt1[256];
    int n = mt1_encode(&tf, mt1, sizeof(mt1), uptime_ms());
    if (n > 0 && gActiveMT2Gesture) {
        gActiveMT2Gesture->feedFrame(mt1, (unsigned int)n);
    }
}

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;

    /* Enable multitouch streaming. The MT2's Bluetooth enable is the feature report
     * 0xF1 {0xF1,0x02,0x01} (confirmed live via IOHIDDeviceSetReport). Over the raw
     * L2CAP control channel a SET_REPORT(feature) is the HIDP transaction byte
     * (SET_REPORT<<4)|FEATURE = 0x53 followed by the report. EXACT PDU + whether this
     * is the control vs interrupt channel are to be confirmed on-device. */
    static const uint8_t kEnable[] = { 0x53, 0xF1, 0x02, 0x01 };
    fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, this, 0, 0);

    /* Receive interrupt frames. */
    fChannel->listenAt(this, &com_schmonz_MT2BTReader::incomingData);

    IOLog("MT2BTReader: bound L2CAP channel, enabled multitouch (0xF1), listening\n");
    registerService();
    return true;
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    fChannel = 0;
    IOService::stop(provider);
}
