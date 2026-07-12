#ifndef VOODOO_INPUT_MUX_H
#define VOODOO_INPUT_MUX_H
#include <IOKit/IOService.h>
class com_schmonz_VoodooInput : public IOService {
    OSDeclareDefaultStructors(com_schmonz_VoodooInput)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument) override;
private:
    IOService *fProvider;          // the satellite nub we matched on
    uint32_t   fLogicalMaxX;
    uint32_t   fLogicalMaxY;
};
#endif
