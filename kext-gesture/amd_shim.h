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
    /* Returns an IOReturn: 0 (kIOReturnSuccess) when the frame was enqueued to the
     * user client, or 0xE00002BC etc. if the device is not yet ready / no client.
     * (C++ mangling ignores return type, so this matches the exported symbol
     * __ZN21AppleMultitouchDevice16handleTouchFrameEPhj regardless of bool vs int.) */
    IOReturn handleTouchFrame(unsigned char *frame, unsigned int length);
    /* The native DEVICE-BUTTON input path (exported symbol
     * __ZN21AppleMultitouchDevice28handlePointerEventFromDeviceEiijj). A real
     * trackpad's physical HID button drives this; it writes the device-button
     * field (this+0xb0 struct +0) that the gesture engine ORs into every
     * dispatched pointer event. We call it on physical-button edges so a real
     * click registers. dx/dy=0 for a button-only change; arg4 is event metadata
     * (0 is fine). */
    void handlePointerEventFromDevice(int dx, int dy, unsigned int button, unsigned int arg4);
};
#endif
