#include "VoodooInputSample.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include "voodoo_wire.h"   // VOODOO_INPUT_LOGICAL_MAX_X_KEY/_Y_KEY
#include "vinput_demo_path.h"
#include <sys/sysctl.h>
#include <string.h>

#define VSAMPLE_LMAX 1000u

static int gVInputDemo = 0;
SYSCTL_INT(_debug, OID_AUTO, vinput_demo, CTLFLAG_RW | CTLFLAG_LOCKED, &gVInputDemo, 0,
           "VoodooInput sample: 1 = circle a demo contact, 0 = idle");

OSDefineMetaClassAndStructors(com_schmonz_VoodooInputSample, IOService)

bool com_schmonz_VoodooInputSample::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fMux = 0; fPhase = 0;
    setProperty("VoodooInputSupported", kOSBooleanTrue);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, (unsigned long long)VSAMPLE_LMAX, 32);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, (unsigned long long)VSAMPLE_LMAX, 32);
    fWorkLoop = IOWorkLoop::workLoop();
    fTimer = fWorkLoop ? IOTimerEventSource::timerEventSource(this, &tick) : 0;
    if (fTimer) fWorkLoop->addEventSource(fTimer);
    sysctl_register_oid(&sysctl__debug_vinput_demo);
    registerService();   // -> our mux matches VoodooInputSupported + attaches as our client
    if (fTimer) fTimer->setTimeoutMS(33);
    IOLog("VoodooInputSample: up (VoodooInputSupported, LMAX=%u)\n", VSAMPLE_LMAX);
    return true;
}

void com_schmonz_VoodooInputSample::stop(IOService *provider) {
    sysctl_unregister_oid(&sysctl__debug_vinput_demo);
    if (fTimer) { fTimer->cancelTimeout(); if (fWorkLoop) fWorkLoop->removeEventSource(fTimer);
                  fTimer->release(); fTimer = 0; }
    if (fWorkLoop) { fWorkLoop->release(); fWorkLoop = 0; }
    IOService::stop(provider);
}

void com_schmonz_VoodooInputSample::tick(OSObject *owner, IOTimerEventSource *sender) {
    com_schmonz_VoodooInputSample *self = OSDynamicCast(com_schmonz_VoodooInputSample, owner);
    if (!self) return;

    /* Lazily find the mux — our client carrying VOODOO_INPUT_IDENTIFIER (it attaches async). */
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

    if (gVInputDemo && self->fMux) {
        unsigned x = 0, y = 0;
        vinput_demo_point(self->fPhase, VSAMPLE_LMAX, VSAMPLE_LMAX, &x, &y);
        VoodooInputEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.contact_count = 1;
        ev.transducers[0].isTransducerActive = true;
        ev.transducers[0].secondaryId = 1;
        ev.transducers[0].currentCoordinates.x = x;
        ev.transducers[0].currentCoordinates.y = y;
        ev.transducers[0].currentCoordinates.pressure = 40;   /* >0: the engine's drop_lifted keeps it */
        self->messageClient(kIOMessageVoodooInputMessage, self->fMux, &ev, sizeof ev);
        self->fPhase = (self->fPhase + 1) % VINPUT_DEMO_PERIOD;
    } else if (self->fMux && self->fPhase != 0) {
        /* just turned off: one clean lift, then idle */
        VoodooInputEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.contact_count = 0;
        self->messageClient(kIOMessageVoodooInputMessage, self->fMux, &ev, sizeof ev);
        self->fPhase = 0;
    }
    sender->setTimeoutMS(33);   /* ~30 fps; always re-arm so a later toggle is picked up */
}
