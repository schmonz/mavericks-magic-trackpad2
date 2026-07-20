#ifndef VOODOO_INPUT_MUX_H
#define VOODOO_INPUT_MUX_H
#include <IOKit/IOService.h>
#include "mavericks_session.h"       // mavericks_session_t
#include "MavericksAMDTerminal.h"     // mavericks_amd_terminal_ctx (opaque) + feed/button/inject helpers
class IOWorkLoop; class IOTimerEventSource;
class com_schmonz_MavericksVoodooInput : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MavericksVoodooInput)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument) override;
    mavericks_amd_terminal_ctx    *synthCtx()  const { return fSynth; }   // sink glue: pass ctx to shared feed helpers
    void armIdle(uint32_t ms);                                    // sink glue
private:
    IOService          *fProvider;
    uint32_t            fLogicalMaxX;
    uint32_t            fLogicalMaxY;
    mavericks_amd_terminal_ctx  *fSynth;
    mavericks_session_t       fSession;
    IOWorkLoop         *fWL;
    IOTimerEventSource *fIdle;
    IOLock             *fLock;
    static void idleTick(OSObject *owner, IOTimerEventSource *sender);
};
#endif
