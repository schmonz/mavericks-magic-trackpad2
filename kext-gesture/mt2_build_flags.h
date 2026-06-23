#ifndef MT2_BUILD_FLAGS_H
#define MT2_BUILD_FLAGS_H
/* Full-BNB mode: genuine BNBTrackpadDevice owns the BT handshake and drives input via its own
 * spawned AppleMultitouchDevice; we win PSM 17 only to hand it over, defer 0xF1 until both channels
 * are OPEN, inject the 0x60/0x02 handler-create trigger, and tee translated MT2->MT1 frames into BNB.
 * No *(BNB+0x1b0) poke. Validated on-device 2026-06-21 (connect/pane/recognition); cursor data-path
 * is being wired by docs/superpowers/plans/2026-06-21-full-bnb-cursor-datapath.md. Validated
 * on-device 2026-06-22: cursor + geometry + scroll/swipe + tap + physical & right click. Default true. */
static const bool kFullBnb = true;

/* Full-BNB geometry: clone OUR transport instance's vtable and override getMultitouchReport
 * (slot 0xcc8) with a geometry answerer BEFORE triggering createMultitouchHandler, so BNB's AMD
 * publishes correct sensor geometry and the cursor is smooth (instead of corner-pinned/janky from
 * the recognizer's degenerate fallback dims). Overriding 0xcc8 short-circuits the AMD's geometry
 * query before it reaches BNBDevice::_getMultitouchReport (which queries the real MT2 over the wire
 * and gets no MT1 D-report answer). Instance-scoped: a co-connected genuine MT1 keeps Apple's shared
 * vtable. Requires kFullBnb. Override BOTH 0xcd8 (getMultitouchReportInfo, the length probe that
 * runs first) and 0xcc8 (getMultitouchReport, data). Validated on-device 2026-06-22. Default true. */
static const bool kBnbGeometry = true;
#endif
