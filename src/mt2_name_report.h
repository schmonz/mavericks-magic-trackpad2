#ifndef MT2_NAME_REPORT_H
#define MT2_NAME_REPORT_H
#include <stdint.h>
#include <stddef.h>

/* Build a fixed-size HID *name* Feature report (report 0x55) into `out`: [report id][name bytes][zero pad].
 * The ENTIRE `out_cap`-byte report is zeroed first, so no uninitialized tail can leak past the name -- a
 * short name must read back as name + zeros, never name + stale buffer bytes (observed 2026-07-29: a 2-char
 * name "Bo" read back with heap-looking garbage in bytes 3..64 because getReport wrote only the 1+name-length
 * prefix and left the caller's report buffer tail untouched). `name`/`nl` is the device's variable-length
 * name as captured off the wire; nl is clamped so [id][name] fits in out_cap. Returns the number of bytes
 * the caller should emit = the FULL report width -- the whole report is defined, so callers write all of it,
 * NOT just 1+nl. Pure + side-effect-free (host-testable); the kext's getReport wraps it with the
 * IOMemoryDescriptor write (clamping to the caller's report length). */
size_t mt2_build_name_report(uint8_t report_id, const uint8_t *name, size_t nl,
                             uint8_t *out, size_t out_cap);

#endif
