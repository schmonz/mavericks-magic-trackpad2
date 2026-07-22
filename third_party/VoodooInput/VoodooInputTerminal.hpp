//
//  VoodooInputTerminal.hpp
//  A pluggable terminal for VoodooInput: on OSes where the multitouch stack won't bind a virtual
//  IOHIDDevice (e.g. < El Capitan), a provider may advertise a concrete VoodooInputTerminal subclass
//  (by class name, key "VoodooInputLegacyTerminalClass") for the mux to drive instead of the simulator.
//  Proposed-upstream interface (authored locally; see PROVENANCE).
//
#ifndef VOODOO_INPUT_TERMINAL_HPP
#define VOODOO_INPUT_TERMINAL_HPP

#include <IOKit/IOService.h>
#include "VoodooInputMultitouch/VoodooInputMessages.h"   // VoodooInputEvent

class VoodooInputTerminal : public OSObject {
    OSDeclareAbstractStructors(VoodooInputTerminal)
public:
    // mux = the VoodooInput multiplexer (nub for any published devices); provider = the satellite that
    // advertised VoodooInputSupported (the terminal reads any device-specific config from it).
    virtual bool start(IOService* mux, IOService* provider) = 0;
    virtual void handleEvent(const VoodooInputEvent* ev) = 0;
    virtual void updateDimensions(uint32_t logicalMaxX, uint32_t logicalMaxY) = 0;
    virtual void stop(IOService* mux) = 0;
};

#endif // VOODOO_INPUT_TERMINAL_HPP
