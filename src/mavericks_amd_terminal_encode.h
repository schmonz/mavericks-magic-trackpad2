#ifndef MAVERICKS_AMD_TERMINAL_ENCODE_H
#define MAVERICKS_AMD_TERMINAL_ENCODE_H
#include "mavericks_frame.h"

/* Encode the neutral frame as a Magic Trackpad 1 multitouch input report
 * (report id 0x28, 4-byte header + N*9-byte finger records), scaling
 * coordinates from MT2 space to MT1 space. Writes into buf (capacity cap).
 *
 * timestamp is the device frame timestamp; its low 18 bits are packed into the
 * header exactly where hid-magicmouse.c reads them
 * (ts = data[1]>>6 | data[2]<<2 | data[3]<<10). MultitouchSupport's gesture
 * engine REQUIRES a monotonically increasing timestamp (a zero/constant value
 * yields "timestamp invalid" and no gestures). Pass elapsed milliseconds.
 *
 * Returns the report length, or -1 if it won't fit. */
int mavericks_amd_construct_report(const MavericksTouchFrame *frame, uint8_t *buf, size_t cap, uint32_t timestamp);

#endif
