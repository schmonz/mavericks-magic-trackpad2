#ifndef MT2_USB_READ_H
#define MT2_USB_READ_H
#include <stdint.h>
#include <stddef.h>

/* Delivered for each raw multitouch frame. frame[0] is the report id (0x02). */
typedef void (*mt2_frame_cb)(const uint8_t *frame, size_t len, void *ctx);

/* Claim the MT2 multitouch USB interface (#1), enable multitouch, and spawn a
 * background thread that delivers each raw frame via cb.
 *
 * Requires: running as root, and the MT2USBClaim kext loaded so IOUSBHIDDriver has
 * released interface 1. Returns 0 on success, -1 if the device/interface can't
 * be claimed (e.g. kext not loaded -> kIOReturnExclusiveAccess). */
int mt2_usb_read_start(mt2_frame_cb cb, void *ctx);

/* Block until the read thread exits on its own (device unplugged or fatal read
 * error). Returns immediately if no thread is running. */
void mt2_usb_read_wait(void);

/* Stop the read thread (if running) and release the interface/device. Safe to
 * call after mt2_usb_read_wait(). */
void mt2_usb_read_stop(void);

#endif
