/* MT2USBReader — in-kernel USB interrupt transport for the Magic Trackpad 2.
 *
 * A thin decoder, the USB sibling of MT2BTReader. Matches interface 1 of the
 * cabled MT2 at a high probe score (beating IOUSBHIDDriver), opens the
 * interrupt-IN pipe, sends the SET_REPORT enable-multitouch control request
 * (the same {0x02,0x01} payload the old userspace mt2_usb_read.c used), and
 * async-reads frames. Each frame is decoded (mt2_usb_decode) and pushed to the
 * MT2Gesture nub via submitFrame(MT2_STREAMING). The shared mt2_session owns the
 * settle gate and all post-decode logic — there is no decision logic here.
 *
 * Replaces the inert MT2USBClaim kext plus the userspace feeder loop.
 */
#include <IOKit/IOLib.h>
#include "MT2USBReader.h"
#include "MT2Gesture.h"
#include "mt2_pipeline.h"
#include "mt2_usb_decode.h"

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2USBReader, IOService)

bool com_schmonz_MT2USBReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fIntf = OSDynamicCast(IOUSBInterface, provider);
    if (!fIntf) { IOLog("MT2USBReader: provider not IOUSBInterface\n"); return false; }
    fStopping = false; fPipe = 0; fMaxPacket = 0; fBuf = 0;

    if (!fIntf->open(this)) { IOLog("MT2USBReader: open failed\n"); return false; }

    IOUSBFindEndpointRequest req;
    req.type = kUSBInterrupt; req.direction = kUSBIn;
    req.maxPacketSize = 0; req.interval = 0;
    fPipe = fIntf->FindNextPipe(0, &req);
    if (!fPipe) { IOLog("MT2USBReader: no interrupt-IN pipe\n"); fIntf->close(this); return false; }
    fMaxPacket = fPipe->GetMaxPacketSize();

    /* enable multitouch: SET_REPORT feature, report id 0x02, payload {0x02,0x01}
       (identical to mt2_usb_read.c). */
    IOUSBDevRequest en;
    uint8_t payload[2] = {0x02, 0x01};
    en.bmRequestType = 0x21; en.bRequest = 0x09;
    en.wValue = 0x0302; en.wIndex = 1; en.wLength = sizeof(payload); en.pData = payload;
    fIntf->DeviceRequest(&en);

    fBuf = IOBufferMemoryDescriptor::withCapacity(256, kIODirectionIn);
    if (!fBuf) { fIntf->close(this); return false; }

    if (gActiveMT2Gesture) gActiveMT2Gesture->connectionEstablished(this, MT2_STREAMING);
    IOLog("MT2USBReader: interface open, multitouch enabled, reading (mps=%u)\n",
          (unsigned)fMaxPacket);
    registerService();
    armRead();
    return true;
}

void com_schmonz_MT2USBReader::armRead(void) {
    if (fStopping || !fPipe || !fBuf) return;
    IOUSBCompletion c;
    c.target = this; c.action = &com_schmonz_MT2USBReader::readComplete; c.parameter = 0;
    fBuf->setLength(fMaxPacket ? fMaxPacket : 64);
    IOReturn r = fPipe->Read(fBuf, 0, 0, &c);
    if (r != kIOReturnSuccess) IOLog("MT2USBReader: Read arm failed 0x%x\n", r);
}

void com_schmonz_MT2USBReader::readComplete(void *target, void * /*param*/,
                                            IOReturn status, UInt32 remaining) {
    com_schmonz_MT2USBReader *self = (com_schmonz_MT2USBReader *)target;
    if (!self || self->fStopping) return;
    if (status == kIOReturnSuccess) {
        UInt32 cap = self->fMaxPacket ? self->fMaxPacket : 64;
        UInt32 n = cap - remaining;
        if (n > 0) {
            uint8_t buf[256];
            if (n > sizeof(buf)) n = sizeof(buf);
            self->fBuf->readBytes(0, buf, n);
            touch_frame_t tf;
            if (mt2_usb_decode(buf, n, &tf) == 0 && gActiveMT2Gesture)
                gActiveMT2Gesture->submitFrame(self, &tf);   /* self = this reader */
        }
        self->armRead();
    } else if (status != kIOReturnAborted) {
        IOLog("MT2USBReader: read ended 0x%x\n", status);
    }
}

/* Relinquish our exclusive hold on the interface so the provider subtree can
 * finalize. MUST run during the willTerminate handshake (device unplug / re-
 * enumerate): IOKit does not call stop() until our open() is released, so closing
 * only in stop() deadlocks teardown — the reader stays inactive/busy and pins the
 * whole dead device subtree (leaked, never freed; one per unplug). Idempotent.
 * We drop fPipe WITHOUT Abort(): it is unowned and may already be torn down with
 * the device; close() lets the interface abort+close its own pipes. */
void com_schmonz_MT2USBReader::releaseInterface(void) {
    fStopping = true;
    fPipe = 0;
    if (fIntf) { fIntf->close(this); fIntf = 0; }
}

bool com_schmonz_MT2USBReader::willTerminate(IOService *provider, IOOptionBits options) {
    releaseInterface();
    return IOService::willTerminate(provider, options);
}

void com_schmonz_MT2USBReader::stop(IOService *provider) {
    releaseInterface();                          /* no-op if willTerminate already did it */
    if (fBuf) { fBuf->release(); fBuf = 0; }     /* safe now: interface closed, no I/O in flight */
    IOLog("MT2USBReader: stopped\n");
    IOService::stop(provider);
}
