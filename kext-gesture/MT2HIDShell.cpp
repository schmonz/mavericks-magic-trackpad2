/*
 * MT2HIDShell - in-kernel IOHIDDevice presenting the Magic Trackpad MT1 report
 * descriptor. See MT2HIDShell.h for why this exists (subtree-local started event
 * driver for AppleMultitouchDevice's wrapper wiring).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "MT2HIDShell.h"

OSDefineMetaClassAndStructors(com_schmonz_MT2HIDShell, IOHIDDevice)

/* The REAL Apple Magic Trackpad HID report descriptor (verbatim, identical to
 * src/vhid_mt1.c kMT1Desc). Top-level Generic Desktop / Mouse collection so it
 * matches the BNBTrackpadEventDriver personality's DeviceUsagePairs {1,2}. */
static const unsigned char kMT1Desc[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01,
    0x85,0x02,
    0x05,0x09, 0x19,0x01, 0x29,0x02,
    0x15,0x00, 0x25,0x01, 0x95,0x02, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x06, 0x81,0x03,
    0x05,0x01, 0x09,0x01, 0xa1,0x00,
    0x15,0x81, 0x25,0x7f, 0x09,0x30, 0x09,0x31, 0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xc0,
    0x05,0x06, 0x09,0x20, 0x85,0x47,
    0x15,0x00, 0x25,0x64, 0x75,0x08, 0x95,0x01, 0xb1,0xa2,
    0x06,0x02,0xff, 0x09,0x55, 0x85,0x55,
    0x15,0x00, 0x26,0xff,0x00, 0x75,0x08, 0x95,0x40, 0xb1,0xa2,
    0x85,0x13,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01,
    0x06,0x01,0xff, 0x09,0x0a, 0x81,0x02,
    0x06,0x01,0xff, 0x09,0x0c, 0x81,0x22,
    0x75,0x01, 0x95,0x06, 0x81,0x01,
    0xc0
};

IOReturn com_schmonz_MT2HIDShell::newReportDescriptor(IOMemoryDescriptor **descriptor) const {
    if (!descriptor) {
        return kIOReturnBadArgument;
    }
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withBytes(
        kMT1Desc, sizeof(kMT1Desc), kIODirectionOut);
    if (!md) {
        return kIOReturnNoMemory;
    }
    *descriptor = md;   /* released by the caller */
    return kIOReturnSuccess;
}

OSString *com_schmonz_MT2HIDShell::newTransportString() const {
    return OSString::withCString("Bluetooth");
}
OSString *com_schmonz_MT2HIDShell::newProductString() const {
    return OSString::withCString("Magic Trackpad");
}
OSString *com_schmonz_MT2HIDShell::newManufacturerString() const {
    return OSString::withCString("Apple Inc.");
}
OSNumber *com_schmonz_MT2HIDShell::newVendorIDNumber() const {
    return OSNumber::withNumber((unsigned long long)1452, 32);
}
OSNumber *com_schmonz_MT2HIDShell::newProductIDNumber() const {
    return OSNumber::withNumber((unsigned long long)782, 32);
}
OSNumber *com_schmonz_MT2HIDShell::newVendorIDSourceNumber() const {
    return OSNumber::withNumber((unsigned long long)2, 32);
}
