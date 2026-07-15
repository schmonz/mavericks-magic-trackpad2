/* MT2USBReader — the USB transport reader for the Magic Trackpad 2.
 *
 * Two halves. PER-TRANSPORT (the ~3%): match interface 1 of the cabled MT2, open the
 * interrupt-IN pipe, send the MT2 0x02 USB multitouch-enable, and async-read frames.
 * SHARED ENGINE (the ~97%) is what it feeds. There is ONE seam: readComplete decodes
 * each raw MT2 0x02 report (mt2_usb_decode -> mt2_frame) and hands it to the shared
 * session engine (policy row mt2_policy_default) whose registered kUsbSink re-encodes
 * and drives the fabricated AMD directly. No AppleUSBMultitouchDriver is ever started.
 * No decision logic lives in this file.
 *
 * Load-bearing RE prose is in docs/mt-stack/explanation.md "MT2USBReader bring-up";
 * the code keeps one-line pointers.
 *
 * Replaces the inert MT2USBClaim kext plus the userspace feeder loop.
 */
#include <IOKit/IOLib.h>
#include <IOKit/hid/IOHIDDevice.h>     /* IOHIDReportType */
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>                /* clock_get_system_microtime */
#include "MT2USBReader.h"
#include "mt2_usb_decode.h"            /* mt2_usb_decode -> mt2_frame (the decode seam) */
#include "mt1_encode.h"
#include "MT2Gesture.h"                /* engine: connectionEstablished/submitFrame + sink type */
#include "mt2_synth_amd.h"           /* mt2_synth_amd_build/amd/teardown — fabricated AMD */
#include "../src/mt2_coordinator.h"    /* transport-coordinator seam (no-op for MT2) */
#include "mt2_diag.h"                  /* shared per-transport stream diagnostics (report id / first frame / edge / gap) */
#include "mt2_log.h"                   /* MT2_DLOG (runtime debug.mt2_log) */

/* Settle after the enable, before starting the AMD, or a mouse-mode getReport storm can panic —
 * see explanation.md "MT2USBReader bring-up". 50ms measured sufficient; don't delete it. */
#define MT2_USB_ENABLE_SETTLE_MS     50

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

/* The reader instance = the session's source token (registered at connectionEstablished).
 * Single device -> one global, like gUsbAmdCtx. */
static IOService *gUsbReader = 0;

/* The fabricated AppleMultitouchDevice context for USB.
 * Non-NULL only when the connection is up and the build succeeded.
 * A single global (one device at a time) — cleared on stop after the session deregisters. */
static mt2_synth_amd_ctx *gUsbAmdCtx = 0;

static uint32_t usb_ts_22bit(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint32_t)(((uint64_t)s * 1000 + u / 1000) & 0x3FFFFF);   /* monotonic ms, 22-bit */
}

/* USB fabricated-AMD sink (mirrors SP2's kBtSink).
 * Delivery target = the fabricated AMD (gUsbAmdCtx). NULL-guards drop deliveries during bring-up.
 * Calls arrive under the session lock. */
static void *usb_sink_amd(void) { return (void *)mt2_synth_amd_amd(gUsbAmdCtx); }
static void usb_sink_feed_frame(void *ctx, const mt2_frame *frame) {
    (void)ctx;
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)usb_sink_amd();
    if (!amd) return;
    if (frame->contact_count > 0)
        MT2_DLOG(2, "feed x=%d y=%d -> usbAMD", frame->transducers[0].currentCoordinates.x,
                 frame->transducers[0].currentCoordinates.y);
    uint8_t mt1[256];
    int n = mt1_encode(frame, mt1, sizeof(mt1), usb_ts_22bit());
    if (n <= 0) return;
    amd->handleTouchFrame(mt1, (unsigned int)n);
}
static void usb_sink_post_button_edge(void *ctx, unsigned mask) {
    (void)ctx;
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)usb_sink_amd();
    if (!amd) return;
    MT2_DLOG(2, "post_button_edge mask=0x%x -> usbAMD", mask);
    amd->handlePointerEventFromDevice(0, 0, mask, 0);
}
static IOReturn usb_sink_inject(void *ctx, const unsigned char *bytes, unsigned int len) {
    (void)ctx;
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)usb_sink_amd();
    if (!amd) return kIOReturnNotReady;
    return amd->handleTouchFrame((unsigned char *)bytes, len);
}
static const mt2_transport_sink_t kUsbSink =
    { usb_sink_feed_frame, usb_sink_post_button_edge, usb_sink_inject, 0 };

OSDefineMetaClassAndStructors(com_schmonz_MT2USBReader, IOService)

bool com_schmonz_MT2USBReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fIntf = OSDynamicCast(IOUSBInterface, provider);
    if (!fIntf) { IOLog("MT2USBReader: provider not IOUSBInterface\n"); return false; }
    fPipe = 0; fBuf = 0; fMaxPacket = 0; fStopping = false;

    /* Direct interrupt-pipe read loop: open the interface, find the interrupt-IN pipe, send the
     * MT2 multitouch-enable, allocate the read buffer, build a fabricated AMD, register with the
     * session, arm the first async read.  Mirrors SP2 (89cad00, MT2BTReader). */
    if (!fIntf->open(this)) { IOLog("MT2USBReader: interface open failed\n"); return false; }

    IOUSBFindEndpointRequest req;
    req.type = kUSBInterrupt; req.direction = kUSBIn;
    req.maxPacketSize = 0; req.interval = 0;
    fPipe = fIntf->FindNextPipe(0, &req);
    if (!fPipe) {
        IOLog("MT2USBReader: no interrupt-IN pipe\n");
        fIntf->close(this); return false;
    }
    fMaxPacket = fPipe->GetMaxPacketSize();

    /* Enable multitouch: SET_REPORT feature, report id 0x02, payload {0x02, 0x01}
     * (same as the old mt2_usb_read.c and the genuine-USB sendEnable step). */
    {
        IOUSBDevRequest en;
        uint8_t payload[2] = {0x02, 0x01};
        en.bmRequestType = 0x21; en.bRequest = 0x09;
        en.wValue = 0x0302; en.wIndex = 1; en.wLength = sizeof(payload); en.pData = payload;
        IOReturn enrc = fIntf->DeviceRequest(&en);
        IOLog("MT2USBReader: multitouch-enable SET_REPORT rc=0x%08x (pre-start)\n", enrc);
    }

    /* Settle: let the device leave mouse mode before the AMD probes it (panic hardening;
     * see explanation.md "MT2USBReader bring-up"). */
    IOSleep(MT2_USB_ENABLE_SETTLE_MS);

    fBuf = IOBufferMemoryDescriptor::withCapacity(fMaxPacket ? fMaxPacket : 64, kIODirectionIn);
    if (!fBuf) { IOLog("MT2USBReader: buffer alloc failed\n"); fIntf->close(this); return false; }

    gUsbReader = this;
    gUsbAmdCtx = mt2_synth_amd_build(gActiveMT2Gesture);
    if (!gUsbAmdCtx) IOLog("MT2USBReader: fabricated AMD build FAILED - no cursor\n");

    if (gActiveMT2Gesture)
        gActiveMT2Gesture->connectionEstablished(this, MT2_STREAMING, &mt2_policy_default, &kUsbSink);
    else
        IOLog("MT2USBReader: ENGINE NOT PUBLISHED at bring-up — input will be dead until replug\n");

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
            mt2_frame tf;
            if (mt2_usb_decode(buf, n, &tf) == 0 && gActiveMT2Gesture)
                gActiveMT2Gesture->submitFrame(self, &tf);   /* self = this reader */
        }
        self->armRead();
    } else if (status != kIOReturnAborted) {
        IOLog("MT2USBReader: read ended 0x%x\n", status);
    }
}

/* Relinquish our hold on the interface and deregister from the engine. Idempotent. MUST run
 * during willTerminate (device unplug / re-enumerate): IOKit defers stop() until the interface
 * is released, so a stop()-only teardown deadlocks and leaks the dead device subtree.
 *
 * We opened fIntf ourselves, so close it here; abort the pipe so the async read callback
 * returns kIOReturnAborted and stops re-arming. The fabricated AMD teardown is deferred to
 * stop() (mirrors SP2) so quiesceDelivery runs after connectionClosed. */
void com_schmonz_MT2USBReader::releaseInterface(void) {
    fStopping = true;

    /* Abort the in-flight async read so readComplete stops re-arming.
     * Safe to call even if fPipe is already NULL (first guard). */
    if (fPipe) { fPipe->Abort(); fPipe = 0; }

    /* Deregister from the engine: after connectionClosed returns, no session effect can reach
     * kUsbSink. */
    if (gActiveMT2Gesture) gActiveMT2Gesture->connectionClosed(this);
    gUsbReader = 0;

    if (fIntf) {
        /* We opened fIntf, so close it here. */
        fIntf->close(this);
        fIntf = 0;
    }
}

bool com_schmonz_MT2USBReader::willTerminate(IOService *provider, IOOptionBits options) {
    releaseInterface();
    return IOService::willTerminate(provider, options);
}

void com_schmonz_MT2USBReader::stop(IOService *provider) {
    releaseInterface();                          /* no-op if willTerminate already did it */

    /* Tear down the fabricated AMD. NULL gUsbAmdCtx first so an in-flight delivery sees no AMD
     * before we quiesce, then free after the drain. Mirrors SP2 stop(). */
    if (gUsbAmdCtx) {
        mt2_synth_amd_ctx *usbAmdToTear = gUsbAmdCtx;
        gUsbAmdCtx = 0;
        if (gActiveMT2Gesture) gActiveMT2Gesture->quiesceDelivery();
        mt2_synth_amd_teardown(gActiveMT2Gesture, usbAmdToTear);
        IOLog("MT2USBReader: fabricated AMD torn down\n");
    }

    if (fBuf) { fBuf->release(); fBuf = 0; }
    IOLog("MT2USBReader: stopped\n");
    IOService::stop(provider);
}
