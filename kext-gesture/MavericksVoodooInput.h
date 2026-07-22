#ifndef VOODOO_INPUT_MUX_H
#define VOODOO_INPUT_MUX_H
#include <IOKit/IOService.h>
#include "MavericksTerminalBackend.h"    // owned backend; forwards all input work
class com_schmonz_MavericksVoodooInput : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MavericksVoodooInput)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument) override;
    mavericks_amd_terminal_ctx *synthCtx() const {   // reader glue: MT2BTReader battery poll
        return fBackend ? fBackend->synthCtx() : 0;
    }
private:
    IOService                *fProvider;
    MavericksTerminalBackend *fBackend;
};
#endif
