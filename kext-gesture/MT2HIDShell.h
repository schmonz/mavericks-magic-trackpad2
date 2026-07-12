#ifndef MT2HIDSHELL_H
#define MT2HIDSHELL_H
#include <IOKit/hid/IOHIDDevice.h>

/*
 * MT2HIDShell - an in-kernel IOHIDDevice that presents the real Magic Trackpad
 * MT1 HID report descriptor (the same bytes src/vhid_mt1.c presents from
 * userspace). Its ONLY purpose is to be a real IOHIDInterface provider that
 * Apple's AppleMultitouchHIDEventDriver (BNBTrackpadEventDriver personality)
 * matches and binds, so that a *started* IOHIDEventService (with an IOHIDPointing
 * nub that reaches IOHIDSystem) comes into existence AS A DESCENDANT OF OUR NUB.
 *
 * Why under our nub: AppleMultitouchDevice::start (non-fake) and its
 * hidEventDriverPublished handler only wire the in-kernel actuation wrapper to an
 * event driver/service that lives in the device's provider subtree (RE'd: the
 * published handler walks the driver's IOService-plane ancestors looking for our
 * nub). A standalone userspace IOHIDUserDevice lives under IOHIDResource, not our
 * nub, so it is rejected. Creating the HID device in-kernel under our nub puts the
 * resulting started event driver where the wrapper-wiring can find it.
 *
 * We do NOT send HID input reports here; the pointing shim is built from the
 * descriptor's elements at IOHIDEventService::start time. Real touch data still
 * flows feeder -> user client -> AppleMultitouchDevice::handleTouchFrame.
 *
 * The device works property-only (like IOHIDUserDevice): the default
 * IOHIDDevice::new*() accessors read VendorID/ProductID/VendorIDSource/Transport/
 * PrimaryUsage[Page] from the property table we pass to init(); we override only
 * the pure-virtual newReportDescriptor().
 */
class com_schmonz_MT2HIDShell : public IOHIDDevice {
    OSDeclareDefaultStructors(com_schmonz_MT2HIDShell)
public:
    virtual IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;

    /* IOHIDDevice::publishProperties sources the IOHIDInterface match keys from
     * these virtual accessors (NOT from the property table); the defaults return
     * NULL, so without these overrides the interface carries no VendorID/
     * VendorIDSource/Transport and AppleMultitouchHIDEventDriver never matches.
     * Each returns a freshly-retained object the caller releases. */
    virtual OSString *newTransportString() const override;
    virtual OSString *newProductString() const override;
    virtual OSString *newManufacturerString() const override;
    virtual OSNumber *newVendorIDNumber() const override;
    virtual OSNumber *newProductIDNumber() const override;
    virtual OSNumber *newVendorIDSourceNumber() const override;
};
#endif
