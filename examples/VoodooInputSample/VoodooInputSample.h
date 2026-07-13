#ifndef VOODOO_INPUT_SAMPLE_H
#define VOODOO_INPUT_SAMPLE_H
#include <IOKit/IOService.h>
class IOWorkLoop; class IOTimerEventSource;
class com_schmonz_VoodooInputSample : public IOService {
    OSDeclareDefaultStructors(com_schmonz_VoodooInputSample)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
private:
    IOWorkLoop         *fWorkLoop;
    IOTimerEventSource *fTimer;
    IOService          *fMux;      // the bound mux (found lazily by VOODOO_INPUT_IDENTIFIER) — Task 4
    unsigned            fPhase;
    static void tick(OSObject *owner, IOTimerEventSource *sender);
};
#endif
