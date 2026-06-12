#ifndef MT2_READER_H
#define MT2_READER_H
#include <stdint.h>
#include <stddef.h>

/* Delivered for each raw multitouch frame. frame[0] is the report id (0x02). */
typedef void (*mt2_frame_cb)(const uint8_t *frame, size_t len, void *ctx);

/* Claim the MT2 multitouch USB interface (#1), enable multitouch, and spawn a
 * background thread that delivers each raw frame via cb.
 *
 * Requires: running as root, and the MT2Claim kext loaded so IOUSBHIDDriver has
 * released interface 1. Returns 0 on success, -1 if the device/interface can't
 * be claimed (e.g. kext not loaded -> kIOReturnExclusiveAccess). */
int mt2_reader_start(mt2_frame_cb cb, void *ctx);

/* Stop the read thread and release the interface. */
void mt2_reader_stop(void);

#endif
