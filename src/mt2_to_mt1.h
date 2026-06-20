#ifndef MT2_TO_MT1_H
#define MT2_TO_MT1_H
#include <stdint.h>
#include <stddef.h>

/* Translate ONE raw MT2 Bluetooth interrupt report into a BT-HID MT1 input report.
 * `in`/`inlen` = the bytes IOBluetoothL2CAPChannel::newDataIn delivers (optionally
 * prefixed with the 0xA1 HID input-transport byte, then the 0x31 MT2 report). `out`
 * receives 0xA1 followed by the MT1 report from mt1_encode. Returns the number of
 * bytes written to `out` (>0) on success, or 0 on decode failure / non-multitouch
 * input (caller drops the frame — never forwards garbage to Apple's parser). */
int mt2_to_mt1(const uint8_t *in, size_t inlen,
               uint8_t *out, size_t outcap, uint32_t timestamp);

#endif
