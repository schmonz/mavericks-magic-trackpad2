//
//  VoodooInput.hpp
//  VoodooInput
//
//  Copyright © 2019 Kishor Prins. All rights reserved.
//

#ifndef VOODOO_INPUT_HPP
#define VOODOO_INPUT_HPP

#include <IOKit/IOService.h>

class VoodooInputSimulatorDevice;
class VoodooInputActuatorDevice;
class TrackpointDevice;
#ifdef MAVERICKS_TERMINAL
class VoodooInputTerminal;
#endif

#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

class EXPORT VoodooInput : public IOService {
    OSDeclareDefaultStructors(VoodooInput);
    
    IOService* parentProvider;
    
    VoodooInputSimulatorDevice* simulator;
    VoodooInputActuatorDevice* actuator;
    TrackpointDevice* trackpoint;
#ifdef MAVERICKS_TERMINAL
    VoodooInputTerminal* legacyTerminal;
#endif
    
    UInt8 transformKey;
    
    UInt32 logicalMaxX = 0;
    UInt32 logicalMaxY = 0;
    UInt32 physicalMaxX = 0;
    UInt32 physicalMaxY = 0;
public:
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    bool willTerminate(IOService* provider, IOOptionBits options) override;
    
    UInt8 getTransformKey();

    UInt32 getPhysicalMaxX();
    UInt32 getPhysicalMaxY();

    UInt32 getLogicalMaxX();
    UInt32 getLogicalMaxY();

    bool updateProperties();

    IOReturn message(UInt32 type, IOService *provider, void *argument) override;

};

#endif
