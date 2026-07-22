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
    /* Reader glue: MT2BTReader parses the BT battery report and hands us the capacity; we forward it to
     * the backend, which owns the fabricated AMD node it lands on. The reader never touches that node. */
    void publishBattery(uint8_t pct) { if (fBackend) fBackend->publishBattery(pct); }
private:
    IOService                *fProvider;
    MavericksTerminalBackend *fBackend;
};
#endif
