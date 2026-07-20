#ifndef MAVERICKS_DIAG_H
#define MAVERICKS_DIAG_H
/* Shared per-transport stream diagnostics (debug.mavericks_log). ONE module for what BT and USB previously
 * each carried a private, drifting copy of: the distinct-report-id-once log, the per-frame edge
 * (level 2), and the activity resume-after-idle-gap marker. Keyed by transport so a single vantage
 * observes BOTH readers in the same format — if the two paths ever diverge (e.g. one keeps streaming
 * across a screensaver and the other goes silent), that divergence pops out here instead of hiding in
 * two separately-worded logs. Kext-only (MAVERICKS_DLOG -> IOLog + the kernel clock), like mavericks_log.cpp.
 *
 * NOT a replacement for BT's CSM connect timeline: CSM_STEADY is FUNCTIONAL — recording it stops the
 * enable-retry loop (teardown safety) — so it stays in MT2BTReader. This module is purely observational.
 *
 * Concurrency: the MT2 drives ONE transport at a time (cabling USB drops BT — docs/mt-stack
 * "single transport at a time"), and the two transports key DISTINCT state slots, so no arbitration is
 * needed; each reader's shim is already serialized on its own workloop. */
#include "mavericks_frame.h"
#include <stdint.h>

typedef enum { MT2_DIAG_BT = 0, MT2_DIAG_USB = 1, MT2_DIAG_NXPORT } mavericks_diag_xport_t;

/* Fresh stream for this transport: re-observe report ids, re-arm the first-frame + gap markers. Call on
 * a new connection (BT CONTROL_UP) / re-enumeration (USB startGenuine). */
void mavericks_diag_reset(mavericks_diag_xport_t x);

/* Log each distinct report id once (level 1) — reveals the id mix, e.g. a wake that came back in
 * basic-HID mouse mode instead of the touch stream, or confirming the battery 0x90 arrives on a given
 * channel. Id-only: does NOT touch the resume-gap clock, so a side channel (BT control-plane battery
 * polls) can share the transport's id set without masking the touch stream's idle gap. */
void mavericks_diag_saw_id(mavericks_diag_xport_t x, uint8_t report_id);

/* At the TOUCH-stream shim entry, on the RAW report id (pre-decode): mavericks_diag_saw_id + the resume edge
 * (level 1) when activity resumes after a >=60s lull. Drives the transport's idle-gap clock. */
void mavericks_diag_raw(mavericks_diag_xport_t x, uint8_t report_id);

/* After a frame decodes: the once-per-stream "first frame streamed end-to-end" marker (level 1; pass
 * want_first=false where the reader already has a functional first-frame signal — BT's CSM_STEADY),
 * and always the per-frame edge coords (level 2 — the frame-by-frame capture for a short-window bug). */
void mavericks_diag_frame(mavericks_diag_xport_t x, const MavericksTouchFrame *frame, bool want_first);

#endif /* MAVERICKS_DIAG_H */
