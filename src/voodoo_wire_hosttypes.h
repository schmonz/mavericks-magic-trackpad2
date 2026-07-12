#ifndef VOODOO_WIRE_HOSTTYPES_H
#define VOODOO_WIRE_HOSTTYPES_H
/* Minimal IOKit-primitive typedefs so the pristine upstream VoodooInput headers
 * compile in host unit tests (off-device). Kernel builds use the real OSTypes. */
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t  UInt8;
typedef uint32_t UInt32;
typedef int32_t  SInt32;
typedef uint64_t AbsoluteTime;
#endif
