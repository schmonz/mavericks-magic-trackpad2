/* MT2USBReader — the USB transport reader for the Magic Trackpad 2.
 *
 * Two halves. PER-TRANSPORT (the ~3%): match interface 1 of the cabled MT2, open the
 * interrupt-IN pipe, send the MT2 0x02 USB multitouch-enable, and async-read frames.
 * SHARED INTERFACE (the ~97%): this reader is a VoodooInput SATELLITE, symmetric with
 * MT2BTReader — on bring-up it advertises VoodooInputSupported + its coordinate span +
 * Transport=USB and registerService()s; the mux (com_schmonz_VoodooInput) attaches as our
 * client. readComplete decodes each raw MT2 0x02 report (mt2_usb_decode -> MavericksTouchFrame),
 * mt2_voodoo_from_frame's it to a VoodooInputEvent, and messageClient()s the mux, which owns
 * the terminal AMD + conditioning. No AppleUSBMultitouchDriver is ever hosted; no fabricated
 * AMD is built here. No decision logic lives in this file.
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
#include "mt2_usb_decode.h"            /* mt2_usb_decode -> MavericksTouchFrame (the decode seam) */
#include "mt2_voodoo_translate.h"      /* mt2_voodoo_from_frame (satellite emit) */
#include "voodoo_wire.h"               /* VoodooInputEvent + VOODOO_INPUT_* keys + kIOMessageVoodooInputMessage */
#include "../src/mt2_coord_range.h"    /* MT2_SPAN_X / MT2_SPAN_Y */
#include "mt2_synth_amd.h"           /* mt2_synth_amd_build/amd/teardown — fabricated AMD */
#include "../src/mt2_coordinator.h"    /* transport-coordinator seam (no-op for MT2) */
#include "mt2_diag.h"                  /* shared per-transport stream diagnostics (report id / first frame / edge / gap) */
#include "mt2_log.h"                   /* MT2_DLOG (runtime debug.mt2_log) */

/* Settle after the enable, before starting the AMD, or a mouse-mode getReport storm can panic —
 * see explanation.md "MT2USBReader bring-up". 50ms measured sufficient; don't delete it. */
#define MT2_USB_ENABLE_SETTLE_MS     50

/* The reader instance (single device -> one global). Set/cleared across start/stop. */
static IOService *gUsbReader = 0;

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
    fMux = 0;
    /* Become a VoodooInput satellite (mirrors MT2BTReader): advertise support + coordinate span +
     * our transport, then registerService so the mux attaches as our client. readComplete emits
     * VoodooInputEvent to it; the mux owns the terminal AMD + conditioning. */
    setProperty("VoodooInputSupported", kOSBooleanTrue);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, (unsigned long long)MT2_SPAN_X, 32);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, (unsigned long long)MT2_SPAN_Y, 32);
    /* We don't advertise Transport=USB here, so the mux defaults the fabricated AMD to Bluetooth transport.
     * This is NOT because of any "USB built-in gating" — that earlier belief was a MYTH (disproven
     * 2026-07-20, open-questions.md "CRACKED: USB gestures"). Every layer (kernel AMD, MultitouchSupport,
     * MultitouchHID.plugin) is transport-blind; USB gestures were dead only because our AMD appears into an
     * already-running hidd that never opens its frames client — fixed by kicking hidd once at bring-up
     * (mt2d-run HIDD_KICK). Advertising Transport=USB (for correct osax-pane presentation) is a safe
     * follow-up to re-test now that the real gate is understood. */

    IOLog("MT2USBReader: VoodooInput satellite up (LMAX %u x %u)\n",
          (unsigned)MT2_SPAN_X, (unsigned)MT2_SPAN_Y);
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
            MavericksTouchFrame tf;
            if (mt2_usb_decode(buf, n, &tf) == 0) {
                /* Emit to the mux (found lazily — attaches async after registerService; pre-attach
                 * frames drop). The mux owns terminal conditioning + the fabricated AMD. */
                if (!self->fMux) {
                    OSIterator *it = self->getClientIterator();
                    if (it) {
                        OSObject *o;
                        while ((o = it->getNextObject())) {
                            IOService *c = OSDynamicCast(IOService, o);
                            if (c && c->getProperty(VOODOO_INPUT_IDENTIFIER)) { self->fMux = c; break; }
                        }
                        it->release();
                    }
                }
                if (self->fMux) {
                    VoodooInputEvent ev = mt2_voodoo_from_frame(&tf, MT2_SPAN_X, MT2_SPAN_Y);
                    self->messageClient(kIOMessageVoodooInputMessage, self->fMux, &ev, sizeof ev);
                }
            }
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

    /* We are a VoodooInput satellite: as this provider stops, IOKit detaches + stops the mux
     * (our client), which tears down its own terminal AMD. Just drop our borrowed reference. */
    fMux = 0;
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

    /* No terminal AMD to tear down here — the mux owns it and cleans up when it detaches as our
     * client (above). */
    if (fBuf) { fBuf->release(); fBuf = 0; }
    IOLog("MT2USBReader: stopped\n");
    IOService::stop(provider);
}
