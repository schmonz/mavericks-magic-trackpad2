#ifndef VOODOO_WIRE_H
#define VOODOO_WIRE_H
/* Our single access point to the vendored, verbatim VoodooInput wire ABI
 * (third_party/VoodooInput/, GPL-2.0). Provides the primitive-type environment the
 * pristine upstream headers assume, then includes them. The vendored files are never edited. */
#ifdef KERNEL
#  include <IOKit/IOTypes.h>          /* real UInt8/UInt32/SInt32/AbsoluteTime in the kext */
#else
#  include "voodoo_wire_hosttypes.h"  /* off-device shim for host unit tests */
#endif
#include "VoodooInputMessages.h"       /* pulls in VoodooInputTransducer.h + VoodooInputEvent.h */
#endif
