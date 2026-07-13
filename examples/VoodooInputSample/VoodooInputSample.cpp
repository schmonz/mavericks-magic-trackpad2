#include "VoodooInputSample.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include "voodoo_wire.h"   // VOODOO_INPUT_LOGICAL_MAX_X_KEY/_Y_KEY

#define VSAMPLE_LMAX 1000u

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
    registerService();   // -> our mux matches VoodooInputSupported + attaches as our client
    IOLog("VoodooInputSample: up (VoodooInputSupported, LMAX=%u)\n", VSAMPLE_LMAX);
    return true;
}

void com_schmonz_VoodooInputSample::stop(IOService *provider) {
    if (fTimer) { fTimer->cancelTimeout(); if (fWorkLoop) fWorkLoop->removeEventSource(fTimer);
                  fTimer->release(); fTimer = 0; }
    if (fWorkLoop) { fWorkLoop->release(); fWorkLoop = 0; }
    IOService::stop(provider);
}

void com_schmonz_VoodooInputSample::tick(OSObject *, IOTimerEventSource *) { /* Task 4 */ }
