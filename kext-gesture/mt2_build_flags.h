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

/* Edge-clamp fix EXPERIMENT (2026-06-23): override the transport's newTransportString (vtable slot
 * 0x868, on our existing geometry clone) so BNB's AMD reports a non-BT Transport. The 10.9
 * MTSlideGesture::isBlocked buggy edge reserve (the L/R "dead zone") is gated on transport==4
 * (Bluetooth); a non-BT transport skips it. TRADEOFF (RE-confirmed): also disables from-edge swipe
 * gestures (isActiveEdgeSlide, same transport==4 gate); core gestures (tap/scroll/swipe/pinch)
 * unaffected. Requires kBnbGeometry (rides its vtable clone). Experiment ON to validate; set false +
 * decide ship default after the on-device verdict. */
/* FALSIFIED 2026-06-24: this override makes MTDeviceGetTransportMethod report 1 (not 4 =
 * Bluetooth) — confirmed live via tools/mt_transport — but the L/R dead-zone PERSISTS and
 * from-edge swipes STILL work. So the edge reserve is NOT gated on transport==4 (the prior
 * RE was wrong; the coordinate-range theory was also falsified earlier). Left false: this
 * is a no-op for the bug. Edge-clamp needs fresh from-evidence RE (candidate gates seen in
 * mt_transport: builtIn=0, familyID=128, driverType=4, parserOptions=47). */
static const bool kEdgeNoBtTransport = false;

/* Genuine-USB translate-and-feed (2026-06-24): instead of our synthetic-USB reader (MT2USBReader
 * feeding our nub), let Apple's genuine AppleUSBMultitouchDriver own the cabled MT2 (genuine USB
 * prefpane). manual-start it on our interface, seed IOUserClientClass + sensor geometry via its init
 * dict (its setProperty override drops keys; manual-start skips personality merge), and interpose
 * handleReport (vtable slot 0x117) to reframe each MT2 0x02 report into a CompactV4 PATH frame
 * (mt2_usb_to_compactv4: type byte 0x28 + 4-byte hdr + 9-byte contacts + checksum). DATA PATH PROVEN
 * end-to-end 2026-06-24 (frames reach MultitouchSupport, coords track the finger). NOT yet
 * cursor/pane-complete: still needs WindowServer to open+drive the instance (auto-discovery TBD), so
 * synthetic-USB remains the working default. Default false; flip true to continue genuine-USB work. */
static const bool kGenuineUsb = false;
#endif
