#ifndef VOODOO_WIRE_H
#define VOODOO_WIRE_H
/* Our single access point to the vendored, verbatim VoodooInput wire ABI
 * (third_party/VoodooInput/, GPL-2.0). Sets the off-device type environment, then
 * includes the pristine upstream header. The vendored files are never edited. */
#ifndef KERNEL
#  include "voodoo_wire_hosttypes.h"
#endif
#include "VoodooInputMessages.h"   /* pulls in VoodooInputTransducer.h + VoodooInputEvent.h */
#endif
