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

## The refactoring shape: system hook → object of our own → thin adapter

The load-bearing principle above is about *what* we reuse; this is about *how the reuse is structured*
so it stays legible. The recurring, desirable shape: **find the system hook, wrap it in an object of our
own design (a pure state machine, pure policy, or declarative plan — no OS types, unit-testable
off-device), and keep the adapter that binds it to the OS thin** (translate events in, perform actions
out; no decision logic). This is what keeps a sharp outside reader from misreading the code, and it's the
structural half of the "97% interface over driver" mission — each RE finding becomes a *dimension of one
of our objects*, not another imperative special-case.

**Realized exemplars (copy these):**
- **`src/mt2_connect_sm.{h,c}`** — pure BT connect/teardown state machine (`csm_step(state,event) →
  {next,action}`; `csm_teardown_steps()` is teardown as an ordered *declarative list*). The kext is the
  adapter. Tested exhaustively in `tests/test_connect_sm.c`.
- **`src/mt2_presence.{h,c}`** — pure transport-PRESENCE SM (states/events/transport-transition
  actions + reconcile). One of three separate presence-adjacent concerns, deliberately NOT fused:
  presence (`mt2_presence`), arbitration (`mt2_session.active_source` / the `mt2_coordinator` seam),
  and bring-up (`mt2_connect_sm`).
  The osax (`tools/mt2_prefpane_refresh/mt2_prefpane_refresh.c`) is a thin adapter that maps IOKit
  edges + a poll to events and performs the render actions. Delivery (osax / SIMBL bundle / DYLD dylib) is
  *orthogonal* to the logic — three build targets, one payload. Tested in `tests/test_presence.c`.
- **`mt2_should_inject`** in the launch watcher — pure inject-decision + thin AppKit adapter
  (`tests/test_pane_watch.m`).
- **`kext-gesture/gh_default_adapter.cpp`** — half-realized: 7 shared generic callbacks with the provider
  threaded through a seam. The config-engine in miniature (a fuller engine waits on a real 3rd device).
- **`mt2_session_policy_t` (`src/mt2_session.{h,c}`)** — stream conditioning as
  policy DATA, in embryo: the three observed BT/USB deltas (liftoff shape, emit-empties,
  watchdog) started as per-transport config rows, each a one-line flip with a known test.
  Realized 2026-07-07 by the readers engine unification; the convergence then COMPLETED
  2026-07-08 — every flip landed as its own tested change, the two rows converged into a
  single `mt2_policy_default` shared by both transports, and the USB absence pump was
  retired once USB adopted the shared liftoff + watchdog. The fuller per-gate policy
  (`MT1_FIRM_RADIUS`, settle, geometry clamp) remains latent.
- **`src/mt2_splice.{h,c}` + `kext-gesture/mt2_splice_kext.cpp`** — the interpose/splice as a
  declarative install plan: a seam is a `const` row (kind MEM_SLOT | VTABLE_CLONE, gate NONE |
  SLOT_POPULATED | CLASS_NAME, slots, shims); the engine owns save-before-write /
  save-two-write-one / capture-from-clone / gate / restore-only-if-still-ours / idempotence /
  restore-before-free, all host-tested against fake memory (`tests/test_splice.c`); the kext
  supplies IOKit+`vtc_*` ops. All four seams (BT interrupt + control delegates, BNB geometry,
  USB handleReport) are rows; the six `gOrig*`/`gCtrl*`/`g*VtableClone*` globals are retired
  (the two `g*InterposedChannel` trackers stay — the caller's "which channel", not the engine's
  "what"). Realized 2026-07-08. The panic-prone splice is now the most-tested code, not the least.

**Realized: transport presence as a named composable piece, not one unified object (2026-07-09).**
`src/mt2_presence_observer.{h,c}` is one IOKit adapter over the pure `mt2_presence` SM (notification
arming + removal-window HOLD timer + supersession), consumed by BOTH userspace observers — the prefpane
osax (uses the SM's rendered action) and the USB→BT handoff daemon (keys on the raw event
`PRESENCE_EV_USB_REMOVE`, since a HOLD action can't tell a USB drop from a BT drop). Canonical service
names `AppleUSBMultitouchDriver` (USB) + `BNBTrackpadDevice` (BT); the observer replaced the handoff's
ad-hoc `MT2USBReader`-terminate edge (measured coincident within ~0.038 ms on-device
2026-07-09, so no perceptible handoff delay). The kext's single-transport ARBITRATION stayed separate
(the device self-arbitrates — cabling USB drops BT — so `mt2_coordinator` is a deliberately EMPTY
composition seam); a transition-heavy future device plugs into that seam or swaps a richer
presence/arbitration piece rather than folding everything into one state object. Validated on-device
over a full BT→USB→BT cycle (HOLD coalescing prevents the switch-gap flash; handoff wakes BT on unplug
with no tap); the pane shrank ~116 lines onto a one-line render callback (`46e8f09`).

**Latent targets (apply the shape here next — ranked by payoff):**
Smaller: `src/vhid_mt1.c`'s feature-report acks → a pure report-id→response table + thin HID adapter.
(The former shape-done-small example here, the raw-byte `mt2_usb_button_edge` in the former
`mt2_usb_reframe.c` — now `mt2_usb_bytes.c` — was
deleted by the 2026-07-07 unification: the click edge now lives in the session — `mt2_click_changed`
in `mt2_session_frame` dispatches `post_click` through the registered transport sink.) Standing
direction + running debt list: memory `mt2-refactor-to-explainability`.

> **STATUS: BUILT + ON-DEVICE VALIDATED (2026-07-13).** This is no longer aspirational. The inbound
> interface (mux + verbatim-vendored wire ABI at `third_party/VoodooInput/`), the synthetic terminal
> (fabricated AMD the mux drives — on-device validated in sub-project 2), and a real sample satellite
> (`examples/VoodooInputSample/`) are all merged. On-device end-to-end run: loading the sample kext →
> the mux binds it (`VoodooInputSupported` match) → the fabricated AMD is adopted by hidd
> (`ENABLE-MT`) → `debug.vinput_demo=1` → `messageClient(kIOMessageVoodooInputMessage,…)` circles the
> cursor (proven by cursor-position sampling); clean teardown, no panic. **A driver authored for
> VoodooInput compiles for 10.9 and Just Works.** The paragraphs below are the original decision record.

**The public interface will be modeled after [VoodooInput](https://github.com/acidanthera/VoodooInput),
possibly verbatim** (decided — see `decisions.md` → "Run VoodooInput on 10.9 / become a VoodooInput
plugin"). VoodooInput (acidanthera) is the de-facto community multitouch-input interface — there is no
Apple-published equivalent to model against — and it was this project's original reference (CREDITS.md).
So rather than invent our own "97% API" (`mt2-mission-interface-over-driver`), we **speak VoodooInput's
contact interface** and become *a driver that speaks a known interface* — the same shape, so one device
can target both eras.

The concrete contract (already RE'd): `VoodooInputEvent{contact_count, timestamp, transducers[]}` +
`VoodooInputTransducer{type, id, fingerType, …}`, delivered via the IOKit message
`kIOMessageVoodooInputMessage = 12345`. The "object of our own design" the input-side adapters target is
(or wraps) that transducer model; our RE'd conditioning + geometry become how we *populate* it.

**Nuance (don't lose it):** VoodooInput is the mechanical *inverse* of us — it *fabricates* a fake MT2 so
IOKit matching binds Apple's native MT-HID driver to a virtual nub, and clients translate *non-Apple*
devices into its contact format; we drive a *real* MT2 into Apple's *older* stack. So we adopt its
**interface**, not its plumbing — becoming a VoodooInput *plugin* was evaluated and ruled wrong-direction
(it doesn't wake the real device). Not built yet; credit the interface in CREDITS.md when we implement it.

**Intentional convergences — COMPLETED + on-device validated 2026-07-08:**
1-3. USB `mt2_policy_usb` walked to BT's shape (`liftoff_shape` → `MT2_LIFTOFF_ABSENCE_PAIR`,
   `emit_empty_frames` → 0, `arm_watchdog` → 1), landed as one tested flip each. USB now
   conditions its stream identically to BT.
4. The USB absence pump was removed — the session's ABSENCE_PAIR liftoff + `MT2_IDLE_MS`
   silence watchdog cover post-liftoff framing on both transports. Validated on-device:
   two-finger tap-secondary (the deferred-commit gesture the pump protected) still works.
5. The two now-identical rows collapsed into one `mt2_policy_default` (both readers register
   it); `mt2_policy_bt` / `mt2_policy_usb` are gone.
6. `mt2_usb_reframe.{h,c}` renamed `mt2_usb_bytes.{h,c}` (checksum + click-report byte
   helpers only — it no longer reframes; test renamed `test_usb_bytes.c`).
Note: tap-drag was inconclusive on-device (macOS "Enable dragging" is off by default; the
behaviour matches BT — not a regression).

**Splice engine — considered-and-declined + queued (2026-07-08):**
- DECLINED: a single reverse-order global teardown walk. The four seams restore at genuinely
  different lifecycle moments (BT delegates in-gate at `stop()`; geometry + USB inside `gh_stop`),
  on different channels/threads; one ordered walk would FABRICATE a teardown sequence Apple's
  object lifecycles don't want. Per-row restore-at-existing-site is correct here. Revisit only
  if a real cross-seam ordering need appears (the row model makes it a small addition).
- DECLINED (2026-07-08, after inspection): uniform guards across rows. Each row reproduces its
  seam's exact current guards per KIND (the MEM_SLOT delegate rows check only-if-still-ours on
  restore; the class gate is CLONE-only). Adding a still-ours check to the CLONE restore to
  match MEM_SLOT is RISKIER than the current unconditional `vtc_restore`: a wrong check would
  SKIP a needed restore and leave a dangling shim (panic), whereas always-restore is the safe
  pattern. Uniformity for its own sake trades safe for risky.
- DECLINED (2026-07-08): fold the `gOrigUsbHandleReport`/`captured_orig` read strictly under the
  write's guard (a supposed install-time TOCTOU). `captured_orig` is already read inside the
  gated install path; the actual concern from review was a RUNTIME shim-vs-restore race (the
  unload-while-streaming class), which this framing doesn't address. No clean install-time
  TOCTOU exists.

## The cast

- **`BNBTrackpadDevice : BNBDevice : BluetoothMultitouchTransport`** — Apple's genuine BT trackpad
  transport. It owns the prefpane (the pane matches this class) and spawns the AMD. We manually start
  a genuine instance so the pane lights up and Apple drives input.
- **`AppleMultitouchDevice` (AMD)** — Apple's multitouch device; its recognizer plugin (kernel
  `MultitouchHID`) turns contact frames into pointer/gesture/click events. Userspace
  `MultitouchSupport` caches geometry at first attach.
- **Our `MT2BTReader`** — wins the BT L2CAP channels, manually starts the genuine BNB,
  interposes a translation shim, and injects geometry + the button gate.
- **Our `MavericksVoodooInputHost`** — the session/conditioning nub (shared MT2→MT1 pipeline) and the
  click sink; also owns the `debug.mt2_log` sysctl.

## End-to-end data flow (full-BNB — RETIRED genuine-BNB era, NOT current)

> **STALE for BT as of `89cad00` (2026-07-15): this section describes the genuine-BNB manual-start path,
> which was DELETED. Current BT is owned/synthetic** — `MT2BTReader` matches the `IOBluetoothL2CAPChannel`
> nub as its provider, `listenAt(incomingData)` on PSM 19, sends the deferred `0xF1` on PSM 17, and drives
> a *fabricated* AMD (no `BNBTrackpadDevice` manual-start, no delegate interpose, no geometry vtable-clone).
> The Apple-stack RE below (PSM ordering, `0xF1` framing, transport vtable slots `0xcc8`/`0xcd8`, the 5 s
> watchdog, geometry values) remains VALID as facts about Apple's stack; only the "our reader manual-starts
> BNB / interposes" description is retired. Ground truth + the reconnection-ladder RE:
> `open-questions.md` "Reconnection mechanism — RE'd 2026-07-16"; the decision: `bt-decisions.md`.

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

## Historical bug: cursor edge dead zones — FIXED 2026-06-25 (`eea90c7`)

**Symptom (both transports).** A ~1-inch band near each L/R pad edge froze the cursor's X (couldn't
move left/right within the band) while Y still moved; a larger top band froze Y while L/R still worked;
no bottom band. Net effect: the active area looked reduced and offset toward the bottom. Each dead zone
froze **only the axis perpendicular to its edge** — the exact signature of position-normalization
saturating against a rectangle smaller than the pad (`norm.x = clamp((x-xmin)/(xmax-xmin),0,1)` pins to
0/1 in the L/R band; `norm.y` pins to 1 in the top band). On-device `tools/mt_contacts` norm/abs traces
confirmed a CLAMP (values pinned at exactly 0.000/1.000, not held-at-entry): the recognizer's active
position rectangle measured ≈ **80×64 mm, Y-center +31 mm**, versus the physical **160×115 mm** — about
half-scale and offset up.

**Root cause: we seeded a HALF-RESOLUTION sensor grid + a ZEROED region.** The recognizer builds its
position-normalization rectangle from the sensor grid/region, not from the Surface dimensions. We seeded
**Cols 16 / Rows 13 + a zeroed Sensor Region + no Surface Descriptor**; a genuine MT2 is **Cols 30 /
Rows 22** with a populated Region/Surface descriptor (ground truth in `reference.md` → genuine geometry).
`16/30 = 0.53`, `13/22 = 0.59` — matching the measured active-rect fractions (X ≈ 0.53, Y ≈ 0.56). So
our half grid produced a half-pad normalization rectangle → the perpendicular-axis edge dead zones.

**The fix.** Seed the **genuine geometry** (Family 129, Rows 22, Cols 30, Surface 15600×11040 + the real
Region Descriptor / Region Param / Surface Descriptor) from **one source of truth** (`src/mt2_geometry.h`)
into BOTH the BT report (`mt2_geometry.c`) and the genuine-USB init dict — verified on both transports
`eea90c7`. Exact bytes in `reference.md` → genuine geometry.

**Four theories were FALSIFIED first** (recorded so we don't re-chase them):
1. **Coordinate-range clamp** (`mt1_encode` MT2_MIN/MAX_X too narrow → `scale()` clamps): refuted —
   measured decoded extent ≈ the assumed range; the ±2500 `MT1_MAX_X` experiment changed nothing.
2. **Device-side sensor saturation** at the border: refuted — under full-BNB an in-band finger wiggle
   made decoded `x` VARY (3440→3573) while the cursor X stayed frozen ⇒ decoded x is faithful and
   changing ⇒ the pin is DOWNSTREAM, not the device.
3. **`MTSlideGesture::isBlocked` gated on `transport==4` (Bluetooth):** falsified on-device — the
   `kEdgeNoBtTransport` override made `tools/mt_transport` report `transportMethod=1` (not 4), but the
   dead zone PERSISTED and from-edge swipes STILL worked (both should have flipped). The reserve is NOT
   transport-gated; `isBlocked` decides the edge-*swipe* block, not pointer suppression.
4. **Surface-width seed too narrow** (the meticulous-but-wrong one): widened `MT2_SURFACE_WIDTH`
   13000→16000 (single knob in `src/mt2_geometry.h`, propagation verified end-to-end via `ioreg` +
   `tools/mt_transport` cached `surfaceW=16000`) — dead zones were IDENTICAL. A confident static-analysis
   chain (`detectSustainedHoverAtEdge` band = `threshold_mm ÷ surfaceWidth`) did not describe the operative
   mechanism. 16000 was KEPT (the pad provably IS 160 mm; 13000 was a real under-model) but it was NOT the
   fix. The lesson: pure static RE reached a confident-but-wrong conclusion; the on-device norm/abs trace
   plus a genuine-device `ioreg` sample cracked it.

## Cursor actuation — how the pointer actually gets driven

Geometry makes the cursor *smooth*; a separate mechanism makes it *move at all*. Apple wires a
cursor only if a matched **`AppleMultitouchHIDEventDriver`** lives in the AMD's provider subtree: the
AMD's `hidEventDriverPublished` ancestor-walks the published event driver's provider chain against the
AMD's own provider, and on a match builds an `AppleMultitouchEventDriverWrapper` → `IOHIDPointing` →
cursor.

The catch in full-BNB: BNB's `IOHIDInterface` carries the MT2's **real BT identity (VID 76 / PID 613
/ source 1)**, but Apple's stock `BNBTrackpadEventDriver` personality matches **VID 1452 / source 2**
— so no event driver binds BNB's AMD → no `IOHIDPointing` → no cursor. (Manual-start bypasses
*device* matching but **not** event-driver matching.) We fix it with a static Info.plist personality
**`MT2HIDEventDriverBNB`**: Apple's `AppleMultitouchHIDEventDriver` `IOClass`, keyed to VID 76 / PID
613 / source 1 with a high probe score, so Apple's own event driver binds BNB's interface and drives
the cursor. It's inert in fDevice mode (no VID-76 interface exists there). The fDevice path uses the
twin `MT2HIDEventDriver` personality (VID 1452 / source 2) plus the MT1 HID shell. See `reference.md`
→ cursor actuation; oracle: `tools/re amd-actuation`.

### Genuine-USB cursor path (different shape — recognition is in `hidd`, not in-kernel)

For the genuine-USB target (`AppleUSBMultitouchDriver`, manual-started + `handleReport` interposed —
see the reframe seam), the cursor is driven by a **userspace** loop, RE-confirmed 2026-06-24 from the
carved kernelcache (`captures/kc/AppleUSBMultitouch.rebased`):

```
device → handleReport → validateChecksum → enqueueData
   → AppleUSBMultitouchUserClient (frames client, opened by hidd)
   → hidd runs the MultitouchHID.plugin recognizer (loaded via IOCFPlugInTypes)
   → UC::postRelativeMouseEvent / postScrollWheelEvent  (user-client external methods)
   → driver postRelativeMouseEvent (vtable byte 0xb30)  [gated on +0x178 / +0x180]
   → the wired AppleUSBMultitouchHIDEventDriver  → IOHIDPointing → cursor
```

Two independent gates, **both** required, and the first is the master:
1. **Frames client open by `hidd`.** It delivers frames to the recognizer AND is the channel the
   recognizer posts motion back through. `allocClassWithName` manual-start skips the IOKit personality
   merge, so the instance lacks the properties `hidd` engages on — seed them in the **init dict**
   (`setProperty` is dropped): `HIDServiceSupport`, `IOCFPlugInTypes`→MultitouchHID.plugin, plus
   `IOUserClientClass` + geometry. With these, `hidd` (pid ~89) opens `AppleUSBMultitouchUserClient`
   (confirmed on-device 2026-06-24). The working BT AMD carries the same set and is likewise opened by
   `hidd` (`captures/cursor-seam/A-usb-working-amd.txt`).
2. **Event driver wired.** `AppleUSBMultitouchDriver::hidEventDriverPublished` (a notifier registered
   in `StartFinalProcessing` for `AppleUSBMultitouchHIDEventDriver`) gates on a **LocationID** match
   (published driver's provider `LocationID` vs the driver's own `+0x5ac`); on match it caches the event
   driver at `+0x180` / sets `+0x178`. `postRelativeMouseEvent` only actuates when those are set. (USB
   gates on LocationID equality; the BT AMD path gates on provider-subtree ancestry — different test,
   same role.)

Contrast with BT: there recognition produces the pointer the same userspace way (hidd opens the AMD's
user client), but the BT AMD is a normally-matched service so hidd finds it without the property-seeding
dance. **Open blocker (genuine-USB):** even with gate 1 solved, the contacts the recognizer receives are
malformed (phantom count, state-0 lead contact, X pinned to 0) → no pointer. See `open-questions.md` →
"Genuine-USB contact frames malformed". Oracle for this layer = `tools/mt_frames_probe`.

## Connect lifecycle (and the flap)

Healthy sequence (observable via CONNTRACE): `CONTROL_UP → INTERRUPT_BOUND → BNB_FORMED → INTERPOSED
→ HANDLER_UP → MT_MODE → STEADY`. The historical flap = the link going inactive before PSM 19 opens
(early `0xF1`, no `waitForChannelState(OPEN)`). Defer-`0xF1` fixed the warm-reconnect case (measured
0 flaps, 2026-06-22). Cold-boot/sleep-wake remain unmeasured — see `open-questions.md`. The
`tools/re conn-trace` oracle gives a STEADY/FAIL verdict per connection.

## Two architectures

- **Full-BNB (shipped):** genuine BNB owns connect + input + pane; we translate/condition/inject.
  Maximal Apple reuse; needs the geometry + button-gate injections above.
- **Hybrid (earlier, `1f4bf79`):** our own fDevice drives input; a `+0x1b0` poke routed prefs.
  Simpler in some ways (geometry was free via our `getReportStub`) but the poke was a panic path.
  See `decisions.md` for why we moved past it (and the live-vs-fragile trade).

## Retired synthetic approach (pre-2026-06-24) — REVIVED on-demand 2026-07-12 for VoodooInput

> **UPDATE 2026-07-12: no longer fully retired.** The fabricated-`AppleMultitouchDevice` +
> `MT2HIDShell` machinery below was **recovered from git history and revived** as the *terminal
> consumer* for **VoodooInput satellites** (sub-project 2 of the VoodooInput interface work). A
> satellite has no Apple MT hardware to reuse, so a fabricated AMD is the only way its frames reach
> the recognizer. It is stood up **on-demand** (`MavericksVoodooInputHost::beginSyntheticTerminal` → `mt2_synth_amd.*`),
> driven by the VoodooInput mux (production) or the test user client (`tools/mt2_synth_inject`), and
> fed via `kSynthSink` (`mt1_encode` → `handleTouchFrame`) — never at load, so it does NOT collide
> with the genuine MT2 BT/USB paths. The RE below (IsFake-strict cast, `MT2HIDShell` identity
> accessors, `IOCFPlugInTypes`/`MultitouchPreferences`/getReport-geometry) is what the revival
> restored verbatim. The **MT2 itself still uses the genuine paths**; routing the MT2 through this
> revived terminal is a recorded follow-on (`decisions.md` → "MT2 → synthetic terminal").

Before the genuine paths became the default, the kext drove the MT2 a second way: **decode the
device ourselves and feed our own fabricated `AppleMultitouchDevice`.** That code was deleted
2026-06-25 once genuine-BNB (BT) + genuine-USB both shipped, but its hard-won RE is preserved here —
several of these findings are reusable for the "97% API" mission and were not obvious.

**The fabricated AMD nub (the strict-cast story).** We `allocClassWithName("AppleMultitouchDevice")`,
`init`'d it with **`IsFake=false`**, installed an enable stub + geometry handler, then `attach`+`start`
under our `MavericksVoodooInputHost` nub. `AppleMultitouchDevice::start` reads `getProperty("IsFake")`:
- **`IsFake=true`** → LENIENT: best-effort event-service lookup, always continues `start()`.
- **`IsFake=false`** → STRICT: walks the IOService plane to the parent provider and **requires it cast
  to `AppleMultitouchHIDEventDriverV2`/`…EventDriver`/`…EventService`**, else logs "Could not cast our
  provider" → "Failing start." → returns false.

We deliberately ran the **strict** path (`IsFake=false`) — not the easy bypass — because strict is what
wires in-kernel cursor actuation. To satisfy the cast we published **`MT2HIDShell`** (see below) under
the nub so a real event driver lived in the AMD's provider subtree. (The file's top-of-source comment
described an `IsFake=true` bypass from an earlier milestone; the shipped synthetic code used `false`.)
After start we set `MT Built-In=true` + `Driver is Ready=true` so `MultitouchSupport`/`hidd` would adopt
the device's `AppleMultitouchDeviceUserClient`.

**`MT2HIDShell` — an in-kernel `IOHIDDevice` published under the nub.** Its only job: be a real
`IOHIDInterface` provider that Apple's `AppleMultitouchHIDEventDriver` matches and binds, so a started
`IOHIDEventService` (→ `IOHIDPointing` → cursor) comes into existence **as a descendant of our nub** —
which is exactly where AMD's `hidEventDriverPublished` ancestor-walk looks (a standalone userspace
`IOHIDUserDevice` lives under `IOHIDResource`, not our nub, and is rejected). Load-bearing detail:
`IOHIDDevice::publishProperties` sources the interface match keys (VendorID/VendorIDSource/Transport)
from the **virtual `new*String`/`new*Number` accessors, NOT the property table** — so the shell had to
override `newVendorIDNumber`/`newVendorIDSourceNumber`/`newTransportString`/… or the interface carried
no identity and Apple's event driver never matched. The shell sent no HID input reports; real touch data
flowed feeder → `submitFrame` → `handleTouchFrame`. It matched the **`MT2HIDEventDriver`** personality
(VID 1452 / PID 782 / source 2).

**`IOCFPlugInTypes` adoption fix (the "M5 fix").** Diffed against a real hidd-adopted device: the
fabricated AMD must advertise **`IOCFPlugInTypes` → `MultitouchHID.plugin`** (UUID
`0516B563-B15B-11DA-96EB-0014519758EF` → `AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin`)
or `hidd`/MultitouchSupport never instantiates the plugin, never opens a user client, and no frames flow.
This is the same key the genuine `DefaultMultitouchProperties` carries — see `reference.md`.

**The `MultitouchPreferences` vs `TrackpadUserPreferences` shadowing finding (reusable, subtle).**
`MTTrackpadHIDManager::determineHIDManagerSettings` (in `MultitouchHID.plugin`) builds its trackpad
settings + chord-gesture-set by reading a prefs dict from the device's IORegistry entry. It reads
**`TrackpadUserPreferences` FIRST** and falls back to **`MultitouchPreferences`** only if the first key
is **absent** (`0x1c3ae`/`0x1c3c4`: the `testq/jne` skips the fallback whenever the first key is present).
With **neither** present it runs a bare-defaults path that leaves the chord set empty → no chord ever
commits → no cursor/tap/scroll/gesture output. We seeded **`MultitouchPreferences`** (NOT
`TrackpadUserPreferences`) on purpose: the genuine settings-push pipeline
(prefpane → BNB `setProperties` → `_setMultitouchPreferences` → `AppleMultitouchDevice::setPreferences`)
writes the user's live prefs into the `MultitouchPreferences` key. Seeding `TrackpadUserPreferences`
instead would **permanently shadow** that push (recognizer reads our stale defaults, never sees the
user's real `Clicking=No`) → the classic "tapping always clicks regardless of the checkbox" bug. Seed
the same key the push targets, and our defaults activate gestures pre-push, then get overwritten/merged
by the user's real settings. (Genuine BNB gets its prefs via the genuine pipeline, so this seeding is
unnecessary there — but the read-order finding is the reusable part.)

**Synthetic geometry via `getReport` (vs genuine's vtable override).** The fabricated path got geometry
"for free": we installed a `getReportStub` on the AMD (`setGetReportHandler`) that answered the D-report
queries (`0xd1/0xd3/0xd9/0xd0/0xa1/0x7f`, skip `0xdb`) straight from `mt2_geometry`. A paired
`setReportStub` remembered the 1-byte value `hidd` SET per reportID in a `g_reg[256]` table so a later
GET echoed it back — because `hidd` SETs mode registers (`0xC8/0xDC/0xDD`) then GETs them and disables
gestures if the GET fails. The genuine path can't install a `getReport` handler on BNB's AMD, so it
instead clones the transport vtable and overrides slots `0xcd8`/`0xcc8` (see "The two injected
side-channels").

**Synthetic transport feeds (BT + USB).** Both retired feeds owned the wire themselves instead of
interposing Apple's driver:
- **BT:** our reader listened on the interrupt channel (`incomingData`), decoded raw `0x31` →
  `submitFrame`, sent the `0xF1` multitouch-enable on PSM 17 itself (it owned the channel), and tee'd an
  MT1-encoded frame to BNB's original delegate (`gOrigCb`) to keep BNB's 5s restart watchdog quiet.
  **Timing lesson (kept even though the code went):** firing `0xF1` on PSM 17 *before* the channel
  reaches OPEN makes `sendTo` block ~14s and the device tears the link down → flap. The genuine path
  avoids this by letting BNB run its own handshake (see "Connect lifecycle").
- **USB:** our reader opened the interface, found the interrupt pipe, sent the SET_REPORT enable, and ran
  an async `armRead`/`readComplete` loop that decoded MT2 USB `0x02` reports → `submitFrame`. The genuine
  USB path instead manual-starts `AppleUSBMultitouchDriver` and interposes its `handleReport` (the
  CompactV4 reframe seam).

## The reframe seam generalizes — a config dimension for the "97% API" mission

The load-bearing principle has a reusable shape worth naming, because it is the clearest concrete
form of the mission (a reusable engine that makes writing such a driver easy; a new device should be
mostly a *config*, not new code — see the `mt2-mission-interface-over-driver` note):

> **Engine capability:** *translate a device's native report into the format some genuine consumer
> already understands, and feed it at that consumer's input seam.*
>
> **Config dimensions** (per device × per target): the device's native decode; the target's expected
> packet format (framing, header, checksum); and **where the seam is** — which method/slot we
> interpose to inject the translated frame.

We already have **three instances of this one capability**, differing only in their config:

| Path | translate | target consumer | the seam (where we feed) |
|---|---|---|---|
| BT (shipped) | MT2 `0x31` → MT1 `0x28` | genuine BNB's spawned AMD | L2CAP delegate-callback slot `channel+0x110` |
| Synthetic-USB (retired) | MT2 USB `0x02` → MT1 | our own `MavericksVoodooInputHost` nub | `submitFrame` (no interpose; we own the nub) |
| Genuine-USB (new) | MT2 USB `0x02` → Apple CompactV4 path-binary | genuine `AppleUSBMultitouchDriver` | its `handleReport` (vtable slot `0x117`), instance-clone interpose |

**The point for the future:** *other* multitouch devices that want to ride a genuine Apple driver
will need their own seam at exactly this kind of point. A different trackpad/mouse over USB would
reuse the genuine-USB seam (`AppleUSBMultitouchDriver::handleReport` + the CompactV4 reframe) with a
different native decode; one over BT would reuse the BNB delegate seam. So "interpose `handleReport`
and reframe" is **not an MT2 special case** — it's a slot in the eventual engine's device table
(`{ native-decode, target-format+checksum, seam-locator }`). When we generalize, each genuine
consumer contributes one row of "here is its input seam and the format it expects"; each device
contributes one row of "here is how I decode and which consumer/seam I target." See
`reference.md` (vtable slots, report formats) and `open-questions.md` (the genuine-USB packet
layout + interpose seam, fully pinned 2026-06-24) for the concrete values that would populate it.

## Bluetooth prefpane device identity: name + picture (RE'd 2026-06-25)

How the **Bluetooth** System-Prefs pane labels and pictures our MT2, and what it would take to make
it look like a genuine Magic Trackpad. These are SEPARATE from the *Trackpad* pane (that one is the
recognizer/`BNBTrackpadDevice` story above); this is the **`Bluetooth` pane's device row**.

### Name — cleanly fixable via `displayName`
The pane shows the device's Bluetooth friendly name from `blued`'s cache
(`/Library/Preferences/com.apple.Bluetooth` → `DeviceCache[<addr>]`). `blued` fetches it from the
device (HCI Remote Name Request) on every connect and stores it in **`Name`**, stamping
`LastNameUpdate`. On 10.9 the MT2's fetch yields **garbage — the two bytes `0x02 0x01`** (not text);
this is a **stock-10.9 limitation, NOT our driver** — proven by A/B: it re-corrupts on a clean
power-cycle with our kext fully unloaded (the `0x02 0x01` matching our enable payload was coincidence).
BUT the pane prefers a separate user-rename key, **`displayName`**, which `blued` does NOT clobber on
re-fetch. Setting `displayName` once (e.g. "Magic Trackpad 2") sticks across power-cycles/transports —
**proven on-device** (a manual Rename persisted). That's the (a) fix; it must be set through the proper
path (a direct PlistBuddy edit of the cache is overwritten by `cfprefsd`/`blued`).

**The `0x02 0x01` is the DEVICE's own advertised name — and it's WRITABLE (we appear to have written it).**
The MT2's cache entry (`com.apple.Bluetooth.plist`, `ClassOfDevice=1428`, `displayName="Magic Trackpad 2"`,
`Name=""`) carries an **`EIRData`** blob whose **AD type `0x09` (Complete Local Name) = the bytes `02 01`**
— the device is *broadcasting* its name as `0x02 0x01` (non-printable → shows empty/garbage). This is
persisted ON THE DEVICE: the same unit reports these exact bytes on **both Mavericks and Tahoe**, and OTHER
MT2s report normal names — so it's this unit's stored name, not an OS parse bug. And `0x02 0x01` = our
**multitouch-enable payload** (USB `02,01` / BT `F1,02,01`). Conclusion: **the MT2 HAS a writable,
device-persisted name field, and our enable path apparently mis-wrote our payload into it.** (Contrast the
Magic Mouse entry two rows up: `Name="Magic Mouse"` — Apple input devices carry a settable name.)
**Web-confirmed** ([Magic Utilities device-config docs](https://magicutilities.net/magic-mouse/help/device-config)):
Apple Magic devices store the BT name *on the device*, and it's host-writable — Magic Utilities (Windows)
renames by storing the new name in the device over an active BT connection + restarting it; a **factory
reset sets the BT name back to default** (our recovery path for this corrupted unit); it even detects an
internal-vs-device name mismatch (exactly our `02 01` vs `displayName` state). ⇒ writing a proper on-device
name is a KNOWN, shipping operation. The Apple-quality fix — a real on-device name that follows the unit
across Macs — **is achievable**; the one un-published piece is the exact name-write mechanism. **Both open
drivers were checked and NEITHER writes the name** (Linux `hid-magicmouse.c` = enable + battery only;
imbushuo Windows = `AmtPtpSetWellspringMode`/enable only) — but the Linux driver comments that *"connected to
a Mac, the name is automatically personalized"*, so **macOS itself writes the name onto the device.** ⇒ best
lead is our OWN platform. **FOUND (disasm of `IOBluetooth` on the 10.9 box):** the writer is
**`-[BluetoothHIDDevice setDeviceName:]`** (@0x43180) — a HID **Feature-report** write: it looks up the report
id via `reportIDForReportKey:` in one of two device-declared schemes — a single **`"LongDeviceName"`** report
(bounded by `getMaxDeviceNameLength`), or **`"DeviceName1".."DeviceName4"`** (4×8-byte chunks) + a
**`"DeviceNameChange"`** commit report — sends them via the IOHIDDeviceInterface `setReport` (type 2 = Feature,
1000 ms), then does `remoteNameRequest:` (refresh) + `setDisplayName:` (alias sync). `+[IOBluetoothDevicePair
setAppleDeviceName:]` is the pairing-time wrapper. (`blued` only *reads* device names + writes the *computer's*
local name.) ⇒ **it's a callable ObjC method: `[bluetoothHIDDevice setDeviceName:@"Mavericks Trackpad 2"]`
does the whole write** — no byte-level RE — which both un-corrupts our `02 01` unit and delivers the feature,
via Apple's own vetted path. The host `displayName` alias (+ auto-re-apply on pair) is only a *fallback*.
See `mt2-device-writable-name` (mechanism RE'd).
[Earlier same-day note claiming
"no writable field" was WRONG — it missed this EIR name field.]

**✅ SOLVED + PROVEN ON-DEVICE 2026-07-05 — the `setDeviceName:` lead above was the WRONG path for THIS
device; the real writer is a direct SET_REPORT to Feature report `0x55`.** `-[BluetoothHIDDevice
setDeviceName:]` builds its report-id map from `ExtendedFeatures`, which is EMPTY on the MT2 under 10.9
(predates the MT2), so it silently no-ops here — the MT2 declares no `LongDeviceName`/`DeviceName1..4`
report. What it DOES declare (135-byte BT HID descriptor) is exactly one Feature report: the 64-byte
**vendor report `0x55`** (usage page `0xff02`). That IS the name store. Proof chain (`tools/re mt2-name`
read, `tools/re mt2-name-write` write):
- `GET_REPORT(Feature,0x55)` returned `02 01` — the SAME two bytes showing as the device's corrupted
  name on macOS Tahoe. That match identified `0x55`.
- `SET_REPORT(Feature,0x55,[id][raw name bytes])` (no header/terminator; payload stored verbatim) wrote
  a canary → read back → **survived a full power cycle** (NVRAM) → **Tahoe displayed the canary** as the
  device name and it followed the unit across hosts. Then wrote the real name `"Mavericks Trackpad 2"`.
- The enable report is a DIFFERENT report (`0xF1`), so name writes never touch multitouch-enable; and no
  kext code interposes `0x55`, so the userland SET/GET reach the device directly.
- The `02 01` was an accidental early-dev write, not the current enable path (which targets `0xF1`).

**Host-cache / live-refresh behavior (validated 2026-07-05):** 10.9 is NOT blind to the on-device name —
it lands in the BT plist `Name` cache (`/Library/Preferences/com.apple.Bluetooth.plist`, per device) and
the pane shows it; the pane just prefers the `displayName` override when set (the old interim alias).
`-[IOBluetoothDevice remoteNameRequest:]` (what blued does after a name write) refreshes the cache LIVE —
**no power cycle** (proven: wrote a test name → remoteNameRequest → `[d name]` + the plist both updated
instantly). Clean update sequence: **SET_REPORT(0x55,name) → remoteNameRequest: → setDisplayName:nil**
(clear the alias so the on-device name shows through). Tools now carry `--clear`/`--refresh`
(`tools/mt2_set_btname`).

**Rename routing + the mirror (RE'd 2026-07-03; corrected + de-risked 2026-07-05).** The pane's device-list
right-click **Rename** writes ONLY the host `displayName` alias — the Bluetooth pane binary calls NEITHER
`setDisplayName:` NOR `setDeviceName:` (it only reads `getDisplayName`); the write lands in `blued`/the
prefs cache, and the device menu is built by `DeviceMenuCreator` (`preferenceItemsForDevice:` = Option-gated
debug rows only). So there is **no pane selector to swizzle**. **To make Rename follow the device onboard we
MIRROR**, folded into the osax already injected into System Prefs (`mt2_prefpane_refresh.c`, aux tick
~1.5 s): each tick read the MT2's `displayName`; if it CHANGED (user renamed) →
`SET_REPORT(Feature,0x55,newName)` → `remoteNameRequest:` → `setDisplayName:nil` (auto-clear, user's
choice). **GATE** the write to a present BT-transport MT2 (a pane-independent BT-presence signal — NOT `gPresence==PRESENCE_BT`, since renames happen on the Bluetooth pane where the Trackpad-pane SM may never have armed) + skip any in-flight transition (`gPresence==PRESENCE_HOLD`), keeping the
in-System-Prefs SET_REPORT out of the getReport-panic scenario (`docs/mt-stack/open-questions.md` /
`mt2-usb-bringup-getreport-panic`). All four primitives are validated on-device; only the ~40-line osax
integration + a gated System-Prefs reload/test remain. This SUPERSEDES the standalone
`com.schmonz.mt2namemirror` LaunchAgent (`c7de7be`), which used the dead `setDeviceName:` path.

### Picture — ✅ FIXED 2026-07-02 (pane row = a VIEW, NOT the vault); vault RE below was the WRONG premise
**REALITY CORRECTION (2026-07-02, mt2-prefpanery session — shipped, on-device verified).** The entire
vault story below is real for the vault API, but it is **NOT how the Bluetooth PANE's device-row icon is
drawn**, so the "only fixable by binary-patching the vault / swizzling `imageForDevice:`" conclusion was
wrong. What we actually found by RE + on-device:
- The pane's **"Devices" list is a VIEW-BASED `NSTableView`**: each row is an `NSTableCellView` whose
  **direct-child `NSImageView`** holds the device icon (Magic Mouse gets a mouse image; the MT2 got the
  generic BT logo). Proven by a runtime view-tree dump (`/tmp/mt2_pane_dump` → the osax logs the tree).
- **The vault is never consulted for that row.** `tools/re calls` shows `imageForMajorDeviceClass:...` has
  **no callers inside IOBluetoothUI** for the list; `stockImageForDevice:` has none; the **pane binary
  doesn't reference the vault at all** (`re str-xref` on Bluetooth.prefPane). And empirically: a swizzle of
  `imageForDevice:forMacTarget:` AND of the funnel `imageForMajorDeviceClass:minorDeviceClass:forMacTarget:`
  **never fired** when the row drew. (The vault path is used by *other* surfaces — pairing wizard / info —
  not the pane row; that part of the RE stands, just mis-attributed.)
- **THE FIX WE SHIPPED (osax, `tools/mt2_prefpane_refresh/mt2_prefpane_refresh.c`, `paint_device_icon`):**
  walk the pane window's view tree each 2s tick, find the MT2's `NSTableCellView`, and set its direct-child
  `NSImageView.image` directly to Apple's `Trackpad.prefPane/Contents/Resources/TrackpadPicture.png` — the
  same "own the real view" approach as the battery row / Change-Batteries button. **No vault swizzle, no
  binary patch.** Re-asserted each tick (the list repopulates on device edges); Magic Mouse untouched.
- **Row-identification finding:** the cell's `objectValue` is NOT the `IOBluetoothDevice` (doesn't answer
  `getDeviceClassMinor`), and the paired MT2's **`name` AND `displayName` are EMPTY in-process** — the pane
  shows Apple's product-database name. So we match the MT2 row by the CoD-resolved paired-device label
  (`refresh_mt2_label`, from `[IOBluetoothDevice pairedDevices]` filtered to major 5 / minor 0x25) OR the
  product name "Magic Trackpad 2" the row shows. Residual: breaks only if a user renames the device to
  something that is neither its resolved label nor contains "Magic Trackpad 2".
- **Scope:** covers the **pane** (the deliverable). The BT **menu extra** / `BluetoothUIServer` were NOT
  touched; if they ever want the icon they need their own delivery (and may use a different path than both
  the vault and this view — re-RE before assuming).

**(historical vault RE — accurate about the vault API, but it is NOT the pane-row path; kept for the other
surfaces that do use the vault):**
The device picture resolves in **`IOBluetoothUI.framework`**, class **`IOBluetoothDeviceImageVault`**:
`imageForDevice:forMacTarget:` reads the device's **Class-of-Device** (`deviceClassMajor` /
`deviceClassMinor`) → `imageForMajorDeviceClass:minorDeviceClass:forMacTarget:` → a **nested dict
lookup `vault[NSNumber(major)][NSNumber(minor)]`** (with a per-major `minor="none"` fallback). On a
miss it returns the generic **`UIBluetoothLogo.icns`**. The vault is populated in
`-[IOBluetoothDeviceImageVault initImageDictionaries]`; there is also an `imageForModelString:`
by-model path. Each entry is a dict resolved by **`loadResourcesForDict:`**, which supplies the image
via ANY of: **`ImageObject`** (a ready `NSImage`), **`SystemIconID`** (→ `imageForSystemIconType:ofSize:`),
**`BundleID`** (→ `_LSCopyBundleURLWithIdentifier`), or **`BundlePath` + `ResourceName`** (an `NSBundle`
resource file). → **An arbitrary external image file is natively supported by the entry format**
(`BundlePath`+`ResourceName`, or a constructed `ImageObject`).

- The **MT2's CoD = `0x594`** → major 5 (Peripheral), minor `0x25` → not a vault key → generic logo
  (user-confirmed icon = the Bluetooth logo).
- The **Magic Trackpad 1 asset is `Trackpad.prefPane/TrackpadPicture.png`** (the picture we'd want).
- **RE COMPLETE (2026-06-30, exact flow disasm'd** — after extending `tools/re objc-methods` to emit IMP
  addresses for symbolicated frameworks; IMPs: `+initImageDictionaries`@`0x17d1f`, `+imageForDevice:
  forMacTarget:`@`0x18867`, `+imageForMajorDeviceClass:minorDeviceClass:forMacTarget:`@`0x188e2`,
  `+loadResourcesForDict:`@`0x18715`**):**
  - **The pane's row icon comes from `imageForDevice:` → `imageForMajorDeviceClass:minorDeviceClass:`**, a
    two-level dict lookup: `vault[NSNumber(major)][NSNumber(minor)]`, with a **per-level `"none"` fallback**
    keyed by `NSNumber(0x6e6f6e65)` (ASCII "none" packed as a uint32) — i.e. `vault[major]["none"]` and
    `dict[minor]` → `dict["none"]`. `forMacTarget` picks `mMacImageMajorDict` first, else `mImageMajorDict`.
    The resolved entry is a dict → `ImageObject` (an `NSImage`) or `BundlePath`+`ResourceName` (→ `NSBundle
    pathForResource:ofType:` → `[[NSImage alloc] initWithContentsOfFile:]`, `setScalesWhenResized:YES`,
    `setSize:`). So the asset is a scaled `NSImage` — **exact pixel size is not critical** (it rescales).
  - **The vault has a MOUSE entry (major 5 → `Mouse.prefPane`/`Mouse.icns`) but NO trackpad entry.** MT2
    `(5, 0x25)` misses every level → generic logo. (Correcting an earlier note: the framework DOES contain
    the trackpad art + a `loadImageFromBundle:"Trackpad.prefPane" withResourceNamed:"TrackpadPicture.png"`
    call, but that lives on a SEPARATE path (~`0x42d0`) gated on `-[device isPointingDevice] && (major&0xf)
    ==5`, used elsewhere — NOT the pane's `imageForDevice:` vault. Likely MT2's non-standard CoD minor
    `0x25` also fails `isPointingDevice`, so even that path skips it. The `kVaultTrackpad*` strings are
    TYPE-STRING (text label) keys, not image-vault keys.)
  - **THE FIX** (pinned): insert `vault[5][0x25]` (or the major-5 `"none"` fallback) →
    `{ BundlePath = "/System/Library/PreferencePanes/Trackpad.prefPane"; ResourceName = "TrackpadPicture.png" }`
    (Apple's own MT1 art, already on disk) — by swizzling `imageForDevice:` / `imageForMajorDeviceClass:` to
    return our image for CoD `(5,0x25)`, OR binary-patching `+initImageDictionaries` to add the entry. Since
    the vault is code (no source plist), a **binary patch of the shared `IOBluetoothUI` covers all render
    processes uniformly** (per the lazy-multi-process finding below).
- There is **no per-device image override** like `displayName`, and the **CoD is re-fetched from the
  live device every connect** (a cache override of `ClassOfDevice` does NOT stick — proven: set
  `1428`→`9600`, restarted `blued`, it re-fetched and overwrote back to `1428`). So the picture cannot
  be fixed via the cache.

**The entry we need to make resolve for CoD `(5,0x25)`** (either as a real `vault[5][0x25]` entry, or as the
value a swizzled `imageForDevice:`/`imageForMajorDeviceClass:` returns):
- **MT1 art (reuse Apple's):** `{ BundlePath = "/System/Library/PreferencePanes/Trackpad.prefPane"; ResourceName = "TrackpadPicture.png"; }`
- **Custom MT2 art:** `{ ImageObject = <our NSImage> }` (easiest via a runtime swizzle — build `[[NSImage alloc]
  initWithContentsOfFile:…]` from any file/embedded data we control and return it), OR
  `{ BundlePath = "<a bundle we install>"; ResourceName = "MagicTrackpad2.icns"; }` for the binary-patch route
  (`BundlePath` must be an `NSBundle` dir — resolver does `bundleWithPath:` → `pathForResource:ofType:` →
  `initWithContentsOfFile:`). **Asset spec:** the row image is drawn at **32×32 pt square** (`setSize:` double
  `0x24750` = `32.0`, width=height; = 64px @2x) with `setScalesWhenResized:YES`, so author a high-res source
  (multi-rep `.icns` / ≥256px, transparent bg, product-render style like Apple's Magic Mouse row icon) and it
  rescales cleanly. Exact px isn't load-bearing; the *presentation style* is.

**Delivery — the render surface is LAZY + MULTI-PROCESS** (RE 2026-06-30: at rest only `coreaudiod` maps
`IOBluetoothUI`; System Prefs maps it when the BT pane opens, `BluetoothUIServer` spawns on demand,
`SystemUIServer` loads it only when the BT menu draws). Options, by where we intervene:
1. **Binary-patch the on-disk `IOBluetoothUI`** (no SIP on 10.9): add a `vault[5][0x25]` entry in
   `+initImageDictionaries` (the vault is code — no source plist). ONE change, every consumer picks it up
   (uniform coverage). Tradeoff: modifies an Apple shared binary; reverts on OS update → installer re-applies.
2. **In-process injection** (no Apple-binary change): a tiny dylib per rendering process that inserts the
   entry or swizzles `imageForDevice:`. Coverage plumbing: System Prefs = our osax already; `SystemUIServer`
   (BT menu) + `BluetoothUIServer` (pairing) via `DYLD_INSERT_LIBRARIES` in their launchd plists, or once in
   `/etc/launchd.conf` (`setenv`, covers every launchd child). Tradeoff: our code loads into many processes
   (must no-op unless `IOBluetoothUI` is present); reversible via config. For first-run UX the surfaces that
   matter are the **pane** (osax) + **menu extra** (SystemUIServer) — so a pragmatic subset, not all four.
3. **Replace the generic fallback asset** `UIBluetoothLogo.icns` — trivial data swap, but COLLATERAL (every
   *unmatched* BT device then shows a trackpad). Only defensible if MT2 is the sole unmatched device. Not a
   real targeted fix.
4. **Device/CoD level** — blocked: the CoD is re-fetched every connect and a cache override doesn't stick;
   making the device advertise a vault-matching CoD, or patching `IOBluetooth.framework`'s
   `isPointingDevice`/CoD read, is just a *different* binary patch, no better than (1).
No purely-data option exists for a *targeted* fix (the vault is code-built). Net: **(1) patch once, uniform,
modifies Apple's file** vs **(2) inject per consumer, no Apple-file change, our code spreads.**

**This is a WANTED public-UX goal, not just-cosmetic** (user 2026-06-30, `mt2-bt-pane-icon-wanted`): a
new user seeing a proper Magic Trackpad icon vs a generic Bluetooth blob is exactly the first-run polish
public release aims for (`mt2-mission`/`mt2-project-goal-public-apple-ux`) — name + icon together give the
genuine-device Bluetooth-pane identity. The RE above is settled; the open work is the **least-invasive
multi-process delivery**, reusing our existing loader infra (`mt2-prefpane-osax-injection-mechanism`)
rather than inventing a new one. (The `displayName` NAME fix is the already-clean half; the icon is the
harder half worth doing.)

## MT2BTReader bring-up: the load-bearing whys (RE detail)

The reader keeps one-line pointers to this section next to the code; the full reasoning
lives here so the `.cpp` stays legible.

### Deferred 0xF1 multitouch enable (`setupInGate` / `reEnableInGate`)

Firing the 0xF1 SET_REPORT(feature) enable on the control channel (PSM 17) *before* the
channel reaches OPEN makes `sendTo` block ~14 s; the device tears the link down meanwhile
(it never got the genuine `listenAt` + `waitForChannelState(OPEN)` acceptance) → channel
inactive → BNB attach fails → connection flap (root-caused 2026-06-21, RE §6). So we let
BNB's `handleStart` accept PSM 17 first and DEFER the enable: the phase-2 timer
(`interposeTimerFired` → `reEnableInGate`) sends 0xF1 after both channels are OPEN. The MT2
uses the 0xF1 command; Apple's stock BNB sends the MT1 0xD7 and so never completes the
enable, which is why the pad is unreachable over stock BT.

### Reconnect re-enable: retry-until-first-frame, teardown-safe (`gSteadyConn`)

BNB's `handleStart` leaves the device in basic-HID mouse mode (report 0x02) after our
initial enable, so phase-2a re-sends 0xF1 to force multitouch mode (report 0x31). On a
rapid/unstable reconnect the enable setReport can fail (channel-not-ready, 0xe00002bc) for
the first few seconds; the old fixed "8 sends then give up" window missed it and left the
device stuck in mouse mode = no cursor until a manual tap (`bt-reconnect-enable-fails`). Fix:
KEEP re-sending until this connection's first real multitouch frame flows. Cadence: fast
(250 ms) for the initial push, then gentle (1 s) so a genuinely-stuck-but-connected device
isn't spammed.

The teardown-safety is the whole point of `gSteadyConn`: `bt_interpose_shim` records
`gSteadyConn = gConnId` on the first decoded frame, and phase-2a re-enables ONLY while
`gSteadyConn != gConnId` (pre-first-frame bring-up). So a device that already reached STEADY
and is now powering off never re-enters the re-enable branch — re-enabling on a working
device's power-off was exactly the v1 regression (v1 re-enabled on any mouse-mode report).
`gSteadyConn` is keyed on `gConnId` (bumped on each control-channel open), so a fresh
connection is automatically `< STEADY` until it produces a frame — no reset needed.

### Battery: publish on the BNB node + the ExtendedFeatures presence gate

The MT2 reports battery as the standard Apple Power-Device INPUT report id 0x90 =
`[0x90][status flags][capacity 0-100]` (capacity = Usage 0x65; byte[1] bit = charging;
verified live both transports, e.g. `90 05 64` = 100%). It never streams 0x90 — only answers
a GET_REPORT — so the control reader polls it (`pollBatteryInGate`, 30 s cadence) and
`bt_control_shim` catches the response. We publish the capacity as the `BatteryPercent`
OSNumber on the genuine BNB node — exactly the property the Trackpad pane reads
(`-[AppleBluetoothHIDDevice batteryPercent]` → `IORegistryEntryCreateCFProperty(node,
"BatteryPercent")`). This bypasses BNB's MT1-shaped voltage/chemistry model (getExtendedReport,
which the MT2 can't answer).

Publish dedup is keyed on BOTH the value AND the node instance: each BT reconnect
manual-starts a FRESH BNBTrackpadDevice (a brand-new registry node with no BatteryPercent);
keying on value alone meant a same-value reconnect (100 → 100) skipped the setProperty on the
fresh node, so the pane/menu read −1.0 after every reconnect. `gLastBattBnb` is reset in
`start()`/`stop()` so a malloc-reused address can't false-match.

The display GATE (seeded in `start()`): the pane reads battery via
`-[AppleBluetoothHIDDevice withBluetoothDevice:]`, whose `initWithHIDDevice:` (IOBluetooth
@0x114d8) does `if (IORegistryEntryCreateCFProperty(node,"ExtendedFeatures")==nil){dealloc;
return nil;}` — so with no ExtendedFeatures the wrapper is nil and batteryPercent returns 0
REGARDLESS of our published BatteryPercent (RE'd 2026-07-01, `tools/mt2_panebattery_probe`).
Genuine MT1 sets it from real extended feature reports; the MT2 has none. We publish a
present-but-empty dict purely to pass that presence gate — batteryPercent then reads our
`BatteryPercent` off the same node (it does not consult the dict's contents).

### Product re-seed after IOHIDDevice::start (`start()`)

Do NOT seed "Product" in the BNB init dict: IOHIDDevice::start (BNB's superclass) overwrites
it from the empty HID product string AFTER the dict is applied (verified live: node
Product="" despite the seed). So we set Product post-start, once `fManualBnb` exists. This is
distinct from the device's persistent BT name (HID Feature report 0x55). USB needs no
equivalent — there the genuine AMD copies the device's real USB iProduct descriptor.

### stop(): plain terminate + release, NO waitQuiet

The manually-started BNB sits at busyState=1 for its ENTIRE life — its genuine connect
lifecycle never completes (deviceReady is never reached in our hybrid flow; the 5 s "Forcing
MT restart" watchdog cycles), so the start-time busy is never balanced. terminate() can't
drop it: probe showed busy=1 before terminate and still busy=1 after a full 8 s waitQuiet
(AMD child busy=0, so it's BNB's own). A waitQuiet here can therefore NEVER succeed — it only
stalls every disconnect for the full bound (RE'd 2026-06-23, see open-questions.md). Unload
safety rests on the in-gate delegate + vtable restores that run BEFORE the teardown (not on
quiescence); the async termination completes after release(). So: plain terminate() +
release(), no wait.

## MT2USBReader bring-up: the load-bearing whys (RE detail)

Sibling to the BT section above. The reader keeps one-line pointers here; the full reasoning
lives below so `MT2USBReader.cpp` stays legible.

NB (RE'd 2026-07-07, 10.9 binary): Apple's `handleReport` itself dispatches `handleButton`
(vtable slot 0xb28, call at handleReport+0x1fd) and contains a `semaphore_timedwait` on its
user-client enqueue path — so the chained call under the engine's session lock can block for a
bounded time; acceptable because contention on that lock is only the BT-only idle timer and
transport-switch registration.

### Enable → settle → start ordering (panic hardening) (`sendEnable` / `settle`)

`startGenuine` sends the MT2's USB multitouch-enable (a SET_REPORT control transfer, payload
`02 01`) and then sleeps `MT2_USB_ENABLE_SETTLE_MS` (50 ms) BEFORE starting Apple's
AppleUSBMultitouchDriver. Both the reorder AND the settle are load-bearing (root-caused
2026-07-04, `mt2-usb-bringup-getreport-panic`): if the AMD starts on a still-mouse-mode device
its `configureDataMode` floods the not-ready device with feature-report probes that all stall
(0xe000404f) — the storm behind an `!pageList phys_addr` panic in IOHIDFamily on the BT→USB
switch. The enable ACKs immediately but the device's mode switch is ASYNC and `gh_start` is
fast, so enable-before-start ALONE is not enough — with no settle the mouse-mode storm returns
(0x28 packets + a 0xc8 configureDataMode retry loop, 24 vs 17 stalls). 50 ms measured
sufficient (reorder-only = storm; reorder + 50 ms = clean). The enable is a control transfer on
`fIntf` (valid since `start()`, independent of the AMD), so it is safe to send here, before
`gh_start`. Don't delete the settle.

### Post-liftoff commit-window framing (shared session, both transports)

DURABLE RE insight (why the starvation happens): the genuine recognizer's deferred tap commits
(e.g. `MTTapDragManager::sendPendingSecondaryTap`, the 2-finger TAP secondary click) run once PER
FRAME and key off the frame timestamp; our device goes silent at liftoff, so an isolated tap's
commit window starves — nothing advances the recognizer's clock to reach the commit. (RE'd
2026-06-24.)

Historically USB solved this with its own reader-side absence pump (`armAbsencePump` /
`mt2_usb_pump_action`): after silence it pumped zero-contact frames on advancing wall-clock ts for
a window long enough to cover the double-tap commit (~30 × 15 ms ≈ 450 ms), re-arming
(`gPumpBudget` / `USB_PUMP_SILENCE_MS`) on any new real report so it never pumped mid-gesture. BT
instead solved it inside the shared session's liftoff + silence watchdog.

Since the 2026-07-08 convergence BOTH transports solve it the same way, inside the shared
`mt2_session`: `emit_with_liftoff` emits the ABSENCE_PAIR at liftoff, and the silence watchdog
flushes on silence-with-contact — together these advance the recognizer's clock across the commit
window. The USB pump (`gPumpBudget` / `USB_PUMP_*` / `mt2_usb_pump_action`) was retired 2026-07-08
as redundant, validated by the two-finger tap-secondary gesture still committing on-device.

### Seed via the init dictionary, not setProperty (`usb_build_init_props`)

Seed the manually-started AMD via the INIT dict, NOT `setProperty`: the driver overrides every
`setProperty` variant and drops unknown keys, but `init()` forwards the dict to `super::init`
which populates the property table directly. Manual-start does no personality merge AND the device
NAKs Apple's feature reports, so without these the instance lacks its user-client class (→ 0
frames to clients), sensor geometry, and the hidd-engagement props (`HIDServiceSupport` +
`IOCFPlugInTypes` → MultitouchHID.plugin). `HIDDefaultBehavior=Trackpad` (NOT the personality's
Mouse) because MT2 multitouch-streaming emits no mouse reports. `parser-options` 39 = 0x27 (bit
0x2 = clicky-hardware gate); prefs seeded under both `MultitouchPreferences` and
`TrackpadUserPreferences` so click/right-click enable. Geometry values are the one source of
truth in `src/mt2_geometry.*` (the half-resolution 16×13 grid + zeroed region we used before was
the edge-dead-zone root cause). No `Product` seed: the genuine AMD's `start` copies the device's
real USB iProduct descriptor ("Magic Trackpad") onto the node, overriding any seed (verified
live) — the device-accurate value, so we let it stand (BT differs — it re-seeds post-start).

### releaseInterface during willTerminate (unplug deadlock) (`releaseInterface`)

`releaseInterface` MUST run during the `willTerminate` handshake (device unplug / re-enumerate),
not just in `stop()`: IOKit does not call `stop()` until the interface is released, so deferring
teardown to `stop()` deadlocks — the reader stays inactive/busy and pins the whole dead device
subtree (leaked, never freed; one per unplug). It is idempotent (willTerminate then stop). It
reverses `startGenuine`: the genuine driver owns the interface + interrupt pipe, so we never
opened `fIntf` ourselves and must NOT close it (an unmatched close would unbalance the open
count) — we only null it. The pump is stopped FIRST (cancel + remove from the workloop) so no
pumped frame can race the `gh_stop` teardown (which restores the vtable, avoiding the in-flight
handleReport use-after-free, then terminates + releases the genuine driver).
