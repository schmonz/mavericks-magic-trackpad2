# Explanation — how we drive the 10.9 multitouch stack

This is the mental model. For exact numbers see `reference.md` (+ `src/mt2_stack.h`); for workflows
see `how-to.md`; for paths we rejected see `decisions.md`; for what we don't yet understand see
`open-questions.md`.

## The load-bearing principle

> **Translate faithfully, delegate recognition to Apple, condition the stream, and inject only what
> Apple's path can't obtain over the wire.**

The Magic Trackpad 2 speaks a multitouch report (`0x31`) that 10.9's stack doesn't natively parse.
Rather than reimplement gesture recognition, we **reuse Apple's**: we translate MT2 → the MT1 report
(`0x28`) Apple understands, let Apple's `AppleMultitouchDevice` (AMD) + the MultitouchHID recognizer
do all the gesture/tap/click work, and only **condition** the stream (lifecycle states, liftoff) and
**inject** the two things Apple's path can't get for this device — sensor **geometry** and the
**device-button gate**. Everything else is Apple's code.

## The cast

- **`BNBTrackpadDevice : BNBDevice : BluetoothMultitouchTransport`** — Apple's genuine BT trackpad
  transport. It owns the prefpane (the pane matches this class) and spawns the AMD. We manually start
  a genuine instance so the pane lights up and Apple drives input.
- **`AppleMultitouchDevice` (AMD)** — Apple's multitouch device; its recognizer plugin (kernel
  `MultitouchHID`) turns contact frames into pointer/gesture/click events. Userspace
  `MultitouchSupport` caches geometry at first attach.
- **Our `com_schmonz_MT2BTReader`** — wins the BT L2CAP channels, manually starts the genuine BNB,
  interposes a translation shim, and injects geometry + the button gate.
- **Our `com_schmonz_MT2Gesture`** — the session/conditioning nub (shared MT2→MT1 pipeline) and the
  click sink; also owns the `debug.mt2_log` sysctl.

## End-to-end data flow (full-BNB, the shipped architecture)

1. The MT2 connects over Bluetooth: control channel **PSM 17**, interrupt channel **PSM 19**.
2. Our reader **wins PSM 17** (high `IOProbeScore` + VID/PID), excluding the generic
   `IOBluetoothHIDDriver`, and **manually starts a genuine `BNBTrackpadDevice`** on the real channel
   (so the stock prefpane attaches, panic-safely — see `decisions.md`).
3. We **defer the `0xF1` enable** (sending it before the channel reaches OPEN blocks ~14s and flaps
   the link — that was the connect-flap root cause; deferring it fixed the warm case).
4. When BNB's interrupt channel appears, we **interpose** our shim onto its delegate-callback slot
   (`channel+0x110`), keeping BNB's own target. We **inject a `0x60/0x02` trigger** so BNB spawns its
   own AMD, then re-send `0xF1` a few times to force multitouch mode.
5. Each MT2 `0x31` report → our shim → `mt2_decode` → the **shared session** (`mt2_session`:
   MakeTouch/Touching/BreakTouch lifecycle + liftoff conditioning) → `mt1_encode` (`0x28`) →
   **BNB's AMD `handleTouchFrame`** → Apple's recognizer → cursor + gestures.

## The two injected side-channels (the hard-won bits)

**Geometry.** Apple's AMD asks for sensor geometry during bring-up via D-reports. The query is
`_cacheDeviceProperties → getReport → _deviceGetReportWithLookUp → _getFeatureReportInfo`, which
calls the handler object at AMD `+0xa8` — which routes to **`transport->vtable[0xcd8]`
(getMultitouchReportInfo, the LENGTH probe) FIRST, then `0xcc8` (getMultitouchReport, the DATA)**.
Both are stubs on the stock transport, so geometry comes back empty → degenerate scaling → janky
cursor. We **clone our transport instance's vtable and override BOTH slots** (the info answerer
returns the payload length, the data answerer returns the bytes from `mt2_geometry`), installed
**before `bnb->start()`** so the AMD is born with correct dims (13000×11300, 13×16, family 128).
Overriding only `0xcc8` does nothing — the `0xcd8` probe fails first and short-circuits. The clone is
instance-scoped, so a co-connected genuine MT1 keeps Apple's shared vtable.

**Device-button gate.** Physical click + two-finger right-click are dispatched via
`handlePointerEventFromDevice` — but the AMD only acts on it if its **S+9 gate** is set, which
`AMD::start` does **iff** it reads `getProperty("ExtractAndPostDeviceButtonState") == true`. BNB
spawns its AMD without that property, so the gate is closed and those clicks are dropped (tap-to-click
still works — it's the recognizer's gesture path, gate-independent). We put
`ExtractAndPostDeviceButtonState` in `DefaultMultitouchProperties`; `createMultitouchHandler` copies
it onto the AMD before `start`, so the gate opens. We also route the click sink to BNB's AMD.

## Connect lifecycle (and the flap)

Healthy sequence (observable via CONNTRACE): `CONTROL_UP → INTERRUPT_BOUND → BNB_FORMED → INTERPOSED
→ HANDLER_UP → MT_MODE → STEADY`. The historical flap = the link going inactive before PSM 19 opens
(early `0xF1`, no `waitForChannelState(OPEN)`). Defer-`0xF1` fixed the warm-reconnect case (measured
0 flaps, 2026-06-22). Cold-boot/sleep-wake remain unmeasured — see `open-questions.md`. The
`re/conn-trace` oracle gives a STEADY/FAIL verdict per connection.

## Two architectures

- **Full-BNB (shipped):** genuine BNB owns connect + input + pane; we translate/condition/inject.
  Maximal Apple reuse; needs the geometry + button-gate injections above.
- **Hybrid (earlier, `1f4bf79`):** our own fDevice drives input; a `+0x1b0` poke routed prefs.
  Simpler in some ways (geometry was free via our `getReportStub`) but the poke was a panic path.
  See `decisions.md` for why we moved past it (and the live-vs-fragile trade).
