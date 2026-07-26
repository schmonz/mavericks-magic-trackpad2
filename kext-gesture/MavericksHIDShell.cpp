/*
 * MavericksHIDShell - in-kernel IOHIDDevice presenting the Magic Trackpad MT1 report
 * descriptor. See MavericksHIDShell.h for why this exists (subtree-local started event
 * driver for AppleMultitouchDevice's wrapper wiring).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "MavericksHIDShell.h"
#include "MT2BTReader.h"       /* MT2BTReader::writeDeviceName — route the 0x55 name to the control channel */
#include "mavericks_stack.h"   /* MAVERICKS_NAME_REPORT_ID (0x55) */

OSDefineMetaClassAndStructors(MavericksHIDShell, IOHIDDevice)

/* The REAL Apple Magic Trackpad HID report descriptor (verbatim, identical to
 * src/mavericks_vhid_mt1.c kMT1Desc). Top-level Generic Desktop / Mouse collection so it
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

IOReturn MavericksHIDShell::newReportDescriptor(IOMemoryDescriptor **descriptor) const {
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

OSString *MavericksHIDShell::newTransportString() const {
    return OSString::withCString("Bluetooth");
}
OSString *MavericksHIDShell::newProductString() const {
    return OSString::withCString("Magic Trackpad");
}
OSString *MavericksHIDShell::newManufacturerString() const {
    return OSString::withCString("Apple Inc.");
}
OSNumber *MavericksHIDShell::newVendorIDNumber() const {
    return OSNumber::withNumber((unsigned long long)1452, 32);
}
OSNumber *MavericksHIDShell::newProductIDNumber() const {
    return OSNumber::withNumber((unsigned long long)782, 32);
}
OSNumber *MavericksHIDShell::newVendorIDSourceNumber() const {
    return OSNumber::withNumber((unsigned long long)2, 32);
}

/* We advertise exactly one Feature report in kMT1Desc: 0x55, the 64-byte on-device name. IOHIDManager's
 * IOHIDDeviceSetReport(Feature, 0x55, [0x55][name]) lands here; route it to the BT reader's control channel
 * so the rename FOLLOWS the device. The default IOHIDDevice::setReport returns kIOReturnUnsupported (which
 * is exactly why naming silently no-op'd under the satellite). Any other report falls through to super. */
IOReturn MavericksHIDShell::setReport(IOMemoryDescriptor *report, IOHIDReportType reportType,
                                      IOOptionBits options) {
    if (reportType == kIOHIDReportTypeFeature && report) {
        uint8_t buf[65];
        IOByteCount len = report->getLength();
        if (len >= 2 && len <= (IOByteCount)sizeof(buf)) {
            report->readBytes(0, buf, len);
            if (buf[0] == MAVERICKS_NAME_REPORT_ID) {   /* [0x55][name...] */
                IOReturn r = MT2BTReader::writeDeviceName(buf + 1, (unsigned int)(len - 1));
                IOLog("MavericksHIDShell: setReport(Feature 0x55, %u name bytes) -> control write 0x%08x\n",
                      (unsigned)(len - 1), r);
                return r;
            }
        }
    }
    return IOHIDDevice::setReport(report, reportType, options);
}

/* Service GET(Feature, 0x55): read the on-device name back over the BT control channel and hand it up as
 * [0x55][name]. The report ID rides in the low byte of `options` (IOHIDDevice convention); only 0x55 is
 * ours (0x47 battery GET, if any, falls through). Without this, GET(0x55) returned kIOReturnUnsupported —
 * which is why `tools/re mt2-name` couldn't read the name under the satellite. */
IOReturn MavericksHIDShell::getReport(IOMemoryDescriptor *report, IOHIDReportType reportType,
                                      IOOptionBits options) {
    if (reportType == kIOHIDReportTypeFeature && report &&
        (unsigned char)(options & 0xff) == MAVERICKS_NAME_REPORT_ID) {
        unsigned char name[64]; unsigned int nl = 0;
        IOReturn r = MT2BTReader::readDeviceName(name, &nl, sizeof name);
        if (r == kIOReturnSuccess) {
            unsigned char buf[64]; memset(buf, 0, sizeof buf);
            buf[0] = MAVERICKS_NAME_REPORT_ID;
            if (nl > sizeof(buf) - 1) nl = sizeof(buf) - 1;
            memcpy(buf + 1, name, nl);
            IOByteCount cap = report->getLength();
            IOByteCount w = cap < (IOByteCount)(1 + nl) ? cap : (IOByteCount)(1 + nl);
            report->writeBytes(0, buf, w);
            IOLog("MavericksHIDShell: getReport(Feature 0x55) -> %u name bytes\n", nl);
            return kIOReturnSuccess;
        }
        IOLog("MavericksHIDShell: getReport(Feature 0x55) -> read 0x%08x\n", r);
        return r;
    }
    return IOHIDDevice::getReport(report, reportType, options);
}
