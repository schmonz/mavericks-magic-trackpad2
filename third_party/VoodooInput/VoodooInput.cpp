//
//  VoodooInput.сpp
//  VoodooInput
//
//  Copyright © 2019 Kishor Prins. All rights reserved.
// Copyright (c) 2020 Leonard Kleinhans <leo-labs>
//

#include "VoodooInput.hpp"
#include "VoodooInputIDs.hpp"
#include "VoodooInputMultitouch/VoodooInputMessages.h"
#include "VoodooInputSimulator/VoodooInputActuatorDevice.hpp"
#include "VoodooInputSimulator/VoodooInputSimulatorDevice.hpp"
#include "Trackpoint/TrackpointDevice.hpp"
#ifdef MAVERICKS_TERMINAL
#include <IOKit/IOLib.h>
#include "../../kext-gesture/MavericksTerminalBackend.h"
#include <libkern/c++/OSString.h>
#endif

#include "libkern/version.h"

#define super IOService
OSDefineMetaClassAndStructors(VoodooInput, IOService);

bool VoodooInput::start(IOService *provider) {
    if (!super::start(provider)) {
        IOLog("Kishor VoodooInput could not super::start!\n");
        return false;
    }
    
    parentProvider = provider;

    if (!updateProperties()) {
        IOLog("VoodooInput could not get provider properties!\n");
        return false;
    }

#ifdef MAVERICKS_TERMINAL
    // 10.9 (< El Capitan): the OS multitouch AMD spawns only from a real USB transport driver, so upstream's
    // simulator/actuator/trackpoint can't dispatch here (U2, see docs/mt-stack). Drive our fabricated-AMD
    // terminal instead and skip the simulator subsystem. Same VoodooInputEvent stream (MavericksTerminalBackend).
    if (version_major < kVoodooInputVersionElCapitan) {
        mavericks_amd_terminal_transport_t xport = MAVERICKS_AMD_TERMINAL_XPORT_BT;
        OSString* tp = OSDynamicCast(OSString, provider->getProperty("MT2 Transport"));
        if (tp && tp->isEqualTo("USB")) xport = MAVERICKS_AMD_TERMINAL_XPORT_USB;
        backend = OSTypeAlloc(MavericksTerminalBackend);
        if (backend && !backend->start(this, xport, logicalMaxX, logicalMaxY)) { backend->release(); backend = 0; }
        if (!backend) IOLog("VoodooInput(MavericksTerminal): backend start failed; no cursor\n");
        goto terminals_ready;
    }
#endif

    // Allocate the simulator and actuator devices
    simulator = OSTypeAlloc(VoodooInputSimulatorDevice);
    actuator = OSTypeAlloc(VoodooInputActuatorDevice);
    trackpoint = OSTypeAlloc(TrackpointDevice);

    if (!simulator || !actuator || !trackpoint) {
        IOLog("VoodooInput could not alloc simulator, actuator or trackpoint!\n");
        OSSafeReleaseNULL(simulator);
        OSSafeReleaseNULL(actuator);
        OSSafeReleaseNULL(trackpoint);
        return false;
    }

    // Initialize simulator device
    if (!simulator->init(NULL) || !simulator->attach(this)) {
        IOLog("VoodooInput could not attach simulator!\n");
        goto exit;
    }
    else if (!simulator->start(this)) {
        IOLog("VoodooInput could not start simulator!\n");
        simulator->detach(this);
        goto exit;
    }

    // Initialize actuator device
    if (!actuator->init(NULL) || !actuator->attach(this)) {
        IOLog("VoodooInput could not init or attach actuator!\n");
        goto exit;
    }
    else if (!actuator->start(this)) {
        IOLog("VoodooInput could not start actuator!\n");
        actuator->detach(this);
        goto exit;
    }

    // Initialize trackpoint device
    if (!trackpoint->init(NULL) || !trackpoint->attach(this)) {
        IOLog("VoodooInput could not init or attach trackpoint!\n");
        goto exit;
    }
    else if (!trackpoint->start(this)) {
        IOLog("VoodooInput could not start trackpoint!\n");
        trackpoint->detach(this);
        goto exit;
    }

#ifdef MAVERICKS_TERMINAL
terminals_ready:
#endif

    setProperty(VOODOO_INPUT_IDENTIFIER, kOSBooleanTrue);
    
    if (!parentProvider->open(this)) {
        IOLog("VoodooInput could not open!\n");
        return false;
    };
    
    return true;

exit:
    return false;
}

bool VoodooInput::willTerminate(IOService* provider, IOOptionBits options) {
    if (parentProvider->isOpen(this)) {
        parentProvider->close(this);
    }

    return super::willTerminate(provider, options);
}

void VoodooInput::stop(IOService *provider) {
#ifdef MAVERICKS_TERMINAL
    if (backend) { backend->stop(this); OSSafeReleaseNULL(backend); }
#endif
    if (simulator) {
        simulator->stop(this);
        simulator->detach(this);
        OSSafeReleaseNULL(simulator);
    }

    if (actuator) {
        actuator->stop(this);
        actuator->detach(this);
        OSSafeReleaseNULL(actuator);
    }

    if (trackpoint) {
        trackpoint->stop(this);
        trackpoint->detach(this);
        OSSafeReleaseNULL(trackpoint);
    }
    super::stop(provider);
}

bool VoodooInput::updateProperties() {
    OSNumber* transformNumber = OSDynamicCast(OSNumber, getProperty(VOODOO_INPUT_TRANSFORM_KEY, gIOServicePlane));
    OSNumber* logicalMaxXNumber = OSDynamicCast(OSNumber, getProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, gIOServicePlane));
    OSNumber* logicalMaxYNumber = OSDynamicCast(OSNumber, getProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, gIOServicePlane));
    OSNumber* physicalMaxXNumber = OSDynamicCast(OSNumber, getProperty(VOODOO_INPUT_PHYSICAL_MAX_X_KEY, gIOServicePlane));
    OSNumber* physicalMaxYNumber = OSDynamicCast(OSNumber, getProperty(VOODOO_INPUT_PHYSICAL_MAX_Y_KEY, gIOServicePlane));

    if (transformNumber == nullptr || logicalMaxXNumber == nullptr || logicalMaxYNumber == nullptr ||
        physicalMaxXNumber == nullptr || physicalMaxYNumber == nullptr) {
        return false;
    }

    transformKey = transformNumber->unsigned8BitValue();
    logicalMaxX = logicalMaxXNumber->unsigned32BitValue();
    logicalMaxY = logicalMaxYNumber->unsigned32BitValue();
    physicalMaxX = physicalMaxXNumber->unsigned32BitValue();
    physicalMaxY = physicalMaxYNumber->unsigned32BitValue();

    return true;
}

UInt8 VoodooInput::getTransformKey() {
    return transformKey;
}

UInt32 VoodooInput::getPhysicalMaxX() {
    return physicalMaxX;
}

UInt32 VoodooInput::getPhysicalMaxY() {
    return physicalMaxY;
}

UInt32 VoodooInput::getLogicalMaxX() {
    return logicalMaxX;
}

UInt32 VoodooInput::getLogicalMaxY() {
    return logicalMaxY;
}

IOReturn VoodooInput::message(UInt32 type, IOService *provider, void *argument) {
    switch (type) {
        case kIOMessageVoodooInputMessage:
#ifdef MAVERICKS_TERMINAL
            if (provider == parentProvider && argument && backend) {
                backend->handleEvent((const VoodooInputEvent*)argument);
                break;
            }
#endif
            if (provider == parentProvider && argument && simulator)
                simulator->constructReport(*(VoodooInputEvent*)argument);
            break;
            
        case kIOMessageVoodooInputUpdateDimensionsMessage:
            if (provider == parentProvider && argument) {
                const VoodooInputDimensions& dimensions = *(VoodooInputDimensions*)argument;
                logicalMaxX = dimensions.max_x - dimensions.min_x;
                logicalMaxY = dimensions.max_y - dimensions.min_y;
#ifdef MAVERICKS_TERMINAL
                if (backend) backend->updateDimensions(logicalMaxX, logicalMaxY);
#endif
            }
            break;
            
        case kIOMessageVoodooInputUpdatePropertiesNotification:
            updateProperties();
            break;

        case kIOMessageVoodooTrackpointRelativePointer: {
            if (trackpoint) {
                const RelativePointerEvent& event = *(RelativePointerEvent*)argument;
                trackpoint->updateRelativePointer(event.dx, event.dy, event.buttons, event.timestamp);
            }
            break;
        }
        case kIOMessageVoodooTrackpointScrollWheel: {
            if (trackpoint) {
                const ScrollWheelEvent& event = *(ScrollWheelEvent*)argument;
                trackpoint->updateScrollwheel(event.deltaAxis1, event.deltaAxis2, event.deltaAxis3, event.timestamp);
            }
            break;
        }
        case kIOMessageVoodooTrackpointMessage:
            if (trackpoint) {
                trackpoint->reportPacket(*(TrackpointReport *)argument);
            }
            break;
        case kIOMessageVoodooTrackpointUpdatePropertiesNotification:
            if (trackpoint) {
                trackpoint->updateTrackpointProperties();
            }
            break;
    }

    return super::message(type, provider, argument);
}

int VoodooInputGetProductId() {
    if (version_major >= kVoodooInputVersionMonterey) {
        return kVoodooInputProductMacbookAir10_1;
    }
    
    return kVoodooInputProductMacbook8_1;
}

#ifdef MAVERICKS_TERMINAL
void VoodooInput::publishBattery(uint8_t pct) { if (backend) backend->publishBattery(pct); }
#endif
