#ifndef AMD_SHIM_H
#define AMD_SHIM_H
#include <IOKit/IOService.h>

/* Opaque until Task 2 RE pins the layout; only pointers are passed around for now. */
struct AMDDeviceReportStruct;

/* Minimal redeclaration so we can call AppleMultitouchDevice's exported, non-virtual
 * methods by symbol. The object is created via allocClassWithName (not new), so the
 * real vtable is intact; these direct (non-virtual) calls resolve to the kext's
 * exported mangled symbols at load via the OSBundleLibrary dependency. The class
 * single-inherits IOService at offset 0, so an IOService* reinterpret needs no
 * pointer adjustment. We never OSDynamicCast to this shim (no metaClass), only to
 * IOService; this type exists solely to name the exported methods. */
class AppleMultitouchDevice : public IOService {
public:
    void setEnableMultitouchHandler(int (*handler)(bool, void *), void *target);
    void setGetReportHandler(int (*handler)(AMDDeviceReportStruct *, unsigned char, void *), void *target);
    void setSetReportHandler(int (*handler)(AMDDeviceReportStruct *, unsigned char, void *), void *target);
    void setSwapButtonsHandler(int (*handler)(unsigned char, unsigned char, void *), void *target);
    bool handleTouchFrame(unsigned char *frame, unsigned int length);
};
#endif
