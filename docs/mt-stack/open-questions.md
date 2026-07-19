# Open questions — things we need to understand but don't yet

Genuine known-unknowns about driving the 10.9 multitouch stack: behaviours we've *observed* but
can't yet *explain*, and things we haven't measured. Distinct from `decisions.md` (which records
choices we've already resolved). When one of these gets understood, fold the finding into
`explanation.md`/`reference.md` and remove it here (or move a closed one to `decisions.md` if it
settled a choice).

---

## Edge-clamp: frozen edge bands — STILL OPEN; leading lead = X surface-dim seed too narrow

> **UPDATE 2026-06-24 (off-device `tools/re`) — GEOMETRIC ROOT-CAUSE CANDIDATE found; redirects H3
> from the (zeroed) Sensor *Region* to the (seeded) Sensor *Surface* dimensions.** Traced the recognizer's
> actual edge-zone classifier end to end, all inside MultitouchHID.plugin:
>
> - `MTParser::updateSurfaceDimensions` (@0x7d4e) builds the mm model from `_MTDeviceGetSensorSurfaceDimensions`
>   (= the `Sensor Surface Width/Height` **we seed**), `×0x51EB851F >>0x25` = **÷100 → mm**. (It also calls
>   `MTSurfaceDimensions::updateScreenBounds_mm` with a **zeroed** MTRect — screen bounds unset; a secondary lead.)
> - `MTParserPath::updateZonesAndEdges` (@0x903a) → `MTParserPath::computeZonesAndEdgesMask` (@0x9176) classify
>   each contact into edge zones by comparing its position-in-mm against the surface extent-in-mm. The four edge
>   distances fall straight out: **left=`contactX`, right=`extentX−contactX`, bottom=`contactY`, top=`extentY−contactY`**.
> - The thresholds come from `MTParameterFactory::initTouchZoneParams` (@0x21182), decoded from its immediate
>   moves (IEEE-754 verified): `[0x4]`=**31.5mm** (corner radius), `[0x8]`=**22.8mm** (Y/top-bottom edge),
>   `[0xc]`=**31.5mm** (X/left-right edge), `[0x10]`=1.0mm, `[0x18]`=12.0mm; transport-dependent `[0x14]`=3.0(BT)/0.5(USB),
>   `[0x1c]`=18.0(BT)/6.0(USB). **`parserType−1000 ≤ 999` gate ⇒ our `parserType=1000` gets the REAL thresholds**
>   (not the zeroed fallback). Crucially, `initTouchZoneParams` takes `MTSurfaceDimensions const&` but **never reads
>   it** — the thresholds are **fixed absolute mm**, surface-size-independent.
>
> Therefore **band-as-fraction-of-pad = `threshold_mm ÷ surface_extent_mm`**, and the surface extent is entirely our
> seed. Our seed is **X-asymmetrically wrong**:
> - Y: `11300 ÷100 = 113mm` vs physical **114.9mm** → 98.3% ≈ correct ⇒ Y-band = `22.8/113` = **20.2%/side** (≈right).
> - X: `13000 ÷100 = 130mm` vs physical **160mm** → 81% (~19% too narrow) ⇒ X-band = `31.5/130` = **24.2%/side**
>   (should be `31.5/160` = 19.7%). X reserve is inflated ~23% **by the too-narrow X seed AND the inherently larger
>   X threshold (31.5 > 22.8mm)**.
>
> Every symptom falls out of this: **X frozen / Y fine** (X seed wrong, Y seed right); **transport-independent**
> (the X/Y edge thresholds `[0x8]`/`[0xc]` don't depend on transport — which is exactly why the transport-flip test
> below couldn't move the band; it was the wrong knob, not an exoneration of the edge mechanism).
>
> **DECISIVE CORROBORATION (the device really IS 160mm wide).** The decoded contact-coordinate spans confirm
> both physical dims to <0.1%, independent of any seed: X `-3678..3934` = **7612 units** = `160mm × 47.6 u/mm`
> (=7616 ✓); Y `-2478..2587` = **5065 units** = `114.9mm × 44.1 u/mm` (=5067 ✓). So the pad's X axis is *provably*
> 160mm. The recognizer consumes `SurfaceWidth÷100` as physical mm for edge distances, and Y was seeded to
> ~physical (113≈114.9mm) — so X should be 160mm (`16000`), and `13000→130mm` is an under-model, not a real
> narrow active area. This removes the "maybe genuine reports 130mm too" worry for the *physical* width.
>
> **LEADING FIX HYPOTHESIS (single constant):** `MT2_SURFACE_WIDTH` 13000 → **~16000** (160mm×100, matching how
> `MT2_SURFACE_HEIGHT=11300` already matches 114.9mm). **Prepared on-device experiment** (cheap/reversible/one
> variable): bump the constant, rebuild, load, device on, sweep L/R edges. *Predict:* X-band shrinks ~24%→~20%/side.
> **Caveats to watch:** (a) blast radius — `convertPixelsToMillimeters` uses *separate* fields `[0x20]/[0x24]` (a
> distinct pixel→mm resolution), so cursor *speed* is **likely** uncoupled from Surface Width, but confirm no
> speed/accel change; (b) verify no prefpane/geometry/scroll/gesture regression (13000 is part of the currently-working
> geometry); (c) verify on **both** transports. **Unresolved off-device:** whether a genuine MT2 reports 13000
> (→ same zone on genuine ⇒ freeze is a separate bug) or ~16000 (→ our value IS the bug). The Y-matches-physical /
> X-doesn't pattern strongly implies 13000 is **our** error, but only the widen-and-observe test settles it.
> The exact zone-mask bit that holds X (mask stored at `MTParserPath +0x108/+0x10c/+0x110`) is offset-aliased in
> static xref (collides with MTChordIntegrating/MTHandMotion) ⇒ needs on-device instrumentation, not more static RE.
>
> **FREEZE MECHANISM IDENTIFIED — `MTParserPath::filterContactForScreenUI` (@0xa8a0).** Despite the name, this is
> the per-contact, per-frame **position hysteresis / edge-hold filter** that writes the filtered contact position
> (`0x28`/`0x2c`) and the **motion deltas `0x148`/`0x14c`** that downstream cursor motion consumes. It computes an
> **edge-hold flag `r14b`** = `(0x1c>4 && 0x7c≠0 && 0x118≠0)` **OR `MTParserPath::detectSustainedHoverAtEdge`
> (@0x97fc, geometry-driven)** OR `0x180`. When `r14b` is set it **snaps the position anchor (`0x130`/`0x138`) to
> the current pixel position and zeroes the delta/velocity accumulators (`0x148`/`0x150`)** — i.e. the contact's
> *position keeps tracking* (matching the on-device observation that decoded `x` VARIES `3440→3573`) while the
> *motion delta driving the cursor goes to 0* (the freeze). This is an exact symptom match and supersedes
> `computeZonesAndEdgesMask` as the *executing* mechanism (zones only classify; this holds). It's heavily
> `MTSurfaceDimensions`-coupled (convertMillimetersToPixels / convertSurfaceFractionToPixels / pixel hysteresis),
> so the too-narrow-X surface seed feeds it too.
>
> **CHAIN CLOSED — `detectSustainedHoverAtEdge` (@0x97fc) is X-only by construction.** Decoded instruction-level:
> (1) `convertMillimetersToSurfaceFraction(const)` yields the trigger threshold **as a fraction = `threshold_mm ÷
> surfaceWidth_mm`** (its X component); (2) it computes distance to the **nearest L/R X edge** in fraction
> (`min(|contactX|,|contactX+c|)`, from `0x28` = contact **X only**); (3) sets the hold flag `0x180` iff
> **X-edge-distance < X-threshold-fraction** AND **hover sustained > time threshold** AND a ratio test. There is
> **no Y term in the geometry gate** — only the X fraction vs an X-width-derived threshold. So the sustained-hover
> edge-hold is **X-axis by construction** (freezes X, never Y) and its band width scales as `threshold_mm ÷
> surfaceWidth`. THE COMPLETE CAUSAL CHAIN: *seeded surfaceWidth (130mm, too narrow) → inflated X-edge-fraction
> threshold → sustained-hover hold (`0x180`→`r14b` in filterContactForScreenUI) → motion deltas (`0x148`) zeroed →
> X frozen while position still tracks.* Every symptom (X-only, Y-fine, position-tracks-while-cursor-frozen,
> transport-independent) falls out of this one mechanism scaled by the one wrong constant. **Decisive on-device
> test unchanged: widen `MT2_SURFACE_WIDTH` 13000→~16000** (predict the X hold band shrinks ~24%→~20%/side; watch
> cursor speed + prefpane/gesture regressions; verify both transports). This is the off-device end of the line —
> the investigation is complete pending that single experiment.

**Scope + implementation note (the bug is on ALL transports — it is NOT transport-dependent).** The frozen-X
band is a *shared-geometry* defect: every transport path publishes the same too-narrow `13000`. Confirmed in
code — BT (full-BNB) and synthetic-USB both emit it via `mt2_fill_geometry_report` (the `MT2_SURFACE_WIDTH`
#define in `src/mt2_geometry.c`), and genuine-USB seeds the **same value as a separate hardcoded literal** at
`kext-gesture/MT2USBReader.cpp:185` (`mt2_dict_num(initp, "Sensor Surface Width", 13000)`). So the fix has
**two edit sites**, not one: (1) the `MT2_SURFACE_WIDTH` #define → fixes BT + synthetic-USB; (2) the literal at
MT2USBReader.cpp:185 → fixes genuine-USB. Cleanest is to make site (2) reference the #define so there's a single
knob (honors the "changes apply to both transports" rule). This also retires the old belief that genuine-USB's
`Transport="USB"` dodges the clamp — it does not (the clamp is geometry-driven, transport-independent; the
recognizer's X/Y edge thresholds `[0x8]`/`[0xc]` don't vary by transport, only `[0x14]`/`[0x1c]` do). **Sequencing
(user-directed 2026-06-24): HOLD this edit until genuine-USB is device-tested + merged to `main`**, so the
edge-clamp change lands on top of a known-good base and is verified across both transports at once.

> **UPDATE 2026-06-24 — transport theory FALSIFIED.** The `kEdgeNoBtTransport` build (override
> `newTransportString` → non-BT) was tested on-device. `tools/mt_transport` confirms it worked at the
> recognizer level: `transportMethod=1` (not 4 = Bluetooth). **But the L/R dead-zone PERSISTED and
> from-edge swipes STILL worked** — both should have changed if they were gated on `transport==4`. So the
> edge reserve is NOT gated on transport (the `0xf8a4 cmpl $0x4` reading below is either misread or not the
> operative gate). This is the SECOND falsified theory (coordinate-range was first). Per systematic
> debugging: stop theory-guessing; re-investigate from evidence (instrument what happens to the cursor's
> X delta inside the band). Candidate gates to examine instead (from `mt_transport`): `builtIn=0`,
> `familyID=128`, `driverType=4`, `parserType=1000`, `parserOptions=47`; also whether toggling the NC
> edge-swipe pref changes it. The `kEdgeNoBtTransport` flag is reverted to false (no-op for the bug).
> Everything below is the (now-doubted) prior transport RE, kept for the audit trail.
>
> **UPDATE 2026-06-24 (off-device, `tools/re`) — the `isBlocked` theory targets the WRONG mechanism.**
> Re-RE'd `MTSlideGesture::isBlocked` (@0xf7ae) fresh: it decides whether the edge-**swipe gesture** is
> blocked (returns the slide-block verdict), NOT pointer suppression. Its reserve-zone test
> `testb $0x20, 0xd9(%r15)` (@0xf8a4) is reached only via event-type `0x44` + `chord+0x108==0` +
> `chord+0xd8==1` + a timing gate + **transport==4** + `(al&3)!=0`. Decisively: `MTHandStatistics+0xd9` bit
> `0x20` (the "contact in reserve zone" flag) is **read in exactly ONE place — this `isBlocked` line**
> (`tools/re xref-offset MultitouchHID 0xd9 R`). So it is NOT a pointer-motion gate; the frozen-X *pointer*
> band must come from a SEPARATE suppression (the edge-swipe-vs-pointer arbitration HOLDING X delta while a
> contact sits in the edge reserve). That's why flipping transport (which only affects this swipe-block
> path) left the pointer dead-zone untouched. **NEXT (from evidence): find where relative-pointer X delta is
> held/zeroed near the L/R edges — the motion-accumulation / mickey path, not `isBlocked`.** Candidates:
> `MTSlideGesture::sendSlideMickeys`/`integrateAxisMotion`, or a reserve-hold in the pointer dispatch keyed
> on the contact's edge position vs the **Sensor Region** (ours is seeded all-zero — a live H3 lead).

### (superseded) prior reading: `MTSlideGesture::isBlocked`, gated on BT transport

**Symptom:** cursor freezes in a band near the pad edges (you must clutch/lift more); the cursor still
reaches all screen edges (it's relative). Up/down works in the band.

**Not coordinates (H1 + the encode-range fix are dead).** In-band wiggle: decoded `x` *varies*
(`3440→3573`) while the cursor is frozen → the device reports position fine; nothing in decode/encode
clamps. On-device, narrowing the emitted X range (`MT1_MAX_X` ±2500) did **nothing** — confirming it's
not a coordinate-range problem. (Also overturns the 2026-06-19 "device-side saturation" reading.)

**CONFIRMED CAUSE (RE, off-device):** the 10.9 `MTSlideGesture::isBlocked` edge-swipe reserve
(MultitouchHID.plugin) — a known bug where the ~2cm edge strip stays reserved (blocking 1-finger
pointer) even with the NC gesture off. Instruction-verified at the `0xf8a4` conditional:
```
0xf89a  cmpl $0x4, %r8d         ; transport == 4 (Bluetooth)?
0xf89e  jne  0xf902             ;   not BT -> skip the buggy block
0xf8a4  testb $0x20, 0xd9(%r15) ; contact in the reserve zone?
0xf8ac  jne  0xf952             ;   -> BLOCKED  (pointer frozen)
```
So the block is **gated on transport == 4**. `_MTDeviceGetTransportMethod` returns the transport cached
at `MTDevice+0x90`, set by `_mt_CachePropertiesForDevice` from the AMD's **`Transport` property string**;
only `"Bluetooth"` maps to 4 (it's the only transport string in MultitouchSupport). We publish
`Transport="Bluetooth"` always (full-BNB via `createMultitouchHandler`; fDevice/USB via `makeHidProps`),
so we hit the block on every transport.

**FIX (identified, pending on-device verify):** make the recognizer see a **non-BT transport** (anything
≠ `"Bluetooth"` → numeric ≠ 4 → `jne 0xf902` skips the buggy block). fDevice/USB path: one-line change
in `makeHidProps`. Full-BNB path: harder — the AMD's `Transport` is set by `createMultitouchHandler`
(override after creation or via the props dict; same timing concern as geometry). **Risk to verify:**
no gesture/tap/pane regression from a non-BT transport (the only transport==4-gated thing found is this
block, but confirm). **Verify:** dead zone gone + gestures OK; `ioreg` shows `Transport` ≠ Bluetooth.
Relates to [[mt2-cursor-edge-clamp]]. (Test bed: the fDevice/USB path under `kFullBnb=false`.)

---

## Genuine USB presentation (AppleUSBMultitouchDriver) — viable but structurally unlike BT-genuine

RE'd 2026-06-23 (off-device) to inform "should the genuine presentation cover USB too / the full-BNB-BT
decision." Findings:
- **Genuine USB stack exists:** `AppleUSBMultitouchDriver` in `AppleUSBMultitouch.kext` (twin of
  AppleBluetoothMultitouch.kext), pane-matched, lineage `IOUSBHIDDriver : IOHIDDevice`.
- **Opposite object model to BT:** it IS the multitouch device (one object — has `cacheDeviceProperties`,
  `_deviceGetReportWithLookUp`, `_deviceGetReport`, `handleReport` itself), NOT a transport that spawns a
  separate AMD. No `createMultitouchHandler`/trigger. Touch feed seam = **`handleReport` @0x4196** (the
  USB pipe → its handleReport parses). Geometry seam = its own **`_deviceGetReport`** (USB), not a
  transport vtable. `newTransportString` = `IOUSBHIDDriver`'s → **"USB"**. (⚠️ CORRECTED 2026-06-24:
  this was once thought to dodge the edge-clamp — it does NOT. The edge-clamp is geometry-driven and
  **transport-independent** (see the Edge-clamp section); USB has the same frozen-X band as BT.)
- **So "genuine for both" is NOT one parameterized strategy** — BT (two-object transport+AMD, vtable
  geometry, L2CAP-interpose feed) and USB (one-object HID driver, _deviceGetReport geometry, handleReport
  feed) are different implementations. It buys the pane on both (~~+ no USB edge-clamp~~ — false; the
  edge-clamp is transport-independent geometry, present on USB too), at the cost of a
  SECOND genuine impl (vs today's working synthetic-USB). Genuine-USB looks *simpler* than genuine-BT
  (no manual-trigger/interpose/flap), possibly near-free IF handleReport parses MT2's USB format.
- **Decision input:** decide full-BNB-for-BT on its own merits (pane/genuine vs flap+dirty-tricks cost),
  NOT on a unify-with-USB benefit (there isn't one). See [[mt2-mission-interface-over-driver]].

**RESOLVED on-device 2026-06-24 (genuine-usb-prefpane spike): NO — Apple's USB driver rejects MT2's
packets; genuine-USB is not viable for input.** What the spike proved, in order:
- Inject a device-match personality for pid 613 (one past Apple's `{566..612}` list) → genuine
  `AppleUSBMultitouchDriver` starts on the MT2 interface, `Transport="USB"`, no panic. **Pane lights**
  (userspace `IOHIDLibUserClient`s attach) — BUT shows the **built-in-laptop-trackpad** animation, not
  `BTTrackpad` art (see [[mt2-prefpane-asset-swap]]).
- Apple's `AppleUSBMultitouchHIDEventDriver` matches `{page 65280, usage 1}`; MT2 presents
  `{65280, 12}`. Inject a twin event-driver personality (usage 12) → it binds + wires to `IOHIDSystem`.
  Cursor plumbing complete.
- Device only streams after MT2's USB enable (SET_REPORT Feature id 0x02 `{0x02,0x01}`); Apple's driver
  doesn't send it. Sent it from userspace (`tools/mt2_usb_enable.c`, `IOHIDDeviceSetReport`, returned OK)
  → device streams: `handleReport` fired 1755× on finger movement (dtrace `fbt:com.apple.driver.AppleUSBMultitouch`).
- **But every packet is rejected.** dmesg: `handleReport - not in path binary mode, received 0x2 data
  packet of length 21` + `validateChecksum - 21-byte packet checksum is incorrect`. Two independent
  incompatibilities between MT2 and Apple's 10.9 USB driver (a "path binary mode" the device isn't in,
  and a different checksum scheme over the 21-byte packet) → nothing dispatched → no cursor.

**Verdict:** genuine-USB assembles the *presentation* (pane) and the device streams, but Apple's driver
can't *consume* MT2's raw packets natively (mode + checksum mismatch). This is **not a dead end — it's a
translation target we haven't written**: the same translate-and-feed pattern we already use everywhere
(BT: MT2→MT1→BNB's AMD; synthetic-USB: MT2→MT1→our nub). Genuine-USB would be **MT2 packets → Apple's
older USB packet format ("path binary mode" + valid checksum) → feed `handleReport`** (interpose via an
instance vtable clone, as with the BT geometry override). Leverage we already hold: dmesg gives checksum
test vectors (`expected 0x499` vs `bytes 0x249` …) to crack `AppleUSBMultitouchDriver::validateChecksum`,
and `handleReport`/`_deviceGetReport` give the packet layout.

### ✅✅ SOLVED 2026-06-24 — genuine-USB translate-and-feed PROVEN END-TO-END
`tools/mt_frames_probe` (drives Apple's MTDeviceCreateList/MTDeviceStart + a contact callback) received **616
contact frames whose normalized coords track the finger** through the GENUINE `AppleUSBMultitouchDriver` +
MultitouchSupport. Three init-dict seeds + one reframe-format fix did it:
1. `IOUserClientClass`="AppleUSBMultitouchUserClient" via the genuine driver's **init dictionary** (its
   `setProperty` override drops unknown keys; init→super::init populates the table) → frames client opens.
2. Sensor geometry via the same init-dict (`Family ID`128, `Sensor Rows`13, `Sensor Columns`16,
   `Sensor Surface Width`13000, `Sensor Surface Height`11300; + `Driver is Ready`/`MTHIDDevice`) → `(0x80)
   family (16 cols X 13 rows)`.
3. **Reframe to CompactV4 PATH frame type `0x28`, NOT `0x60`.** `0x60` is a NOTIFICATION type in
   MultitouchSupport's dispatcher (func @0x4825, switches on packet[0]); its 0x24..0x29 jump table maps
   **0x28 → `_MTParse_CompactV4BinaryPath`** (which uses `_MTCompactV4HeaderUnpack` + `_MTCompactV4BinaryContactUnpack`
   = our exact 4-byte header + 9-byte contacts). New frame = `[0x28][4-byte CV4 hdr][N×9 contacts][2-byte
   checksum]`, contacts at offset 4, count `(len-4)/9` (checksum absorbed by integer div). The genuine
   driver's handleReport is NOT 0x60-exclusive (non-0x02 → validateChecksum → enqueue), so it forwards 0x28.
   `src/mt2_usb_reframe.c` updated; `test_usb_reframe` updated + green.
**REMAINING for cursor/pane — it's KERNEL actuation, NOT WindowServer** (RE'd 2026-06-24; commit `c84f0ec`).
The pointer is driven in-kernel: a matched `AppleUSBMultitouchHIDEventDriver` in the genuine driver's subtree
→ `AppleUSBMultitouchDriver::hidEventDriverPublished` (~0x5746) wires it (LocationID-gated on `this+0x5ac`,
then `*0x148`/`*0x20` feed-calls) → `IOHIDPointing` → cursor. Same mechanism as BT
([[mt2-fullbnb-cursor-actuation]]); the event-driver class is a thin `IOHIDEventService` subclass (only
`start`/`setPointingProperties`, NO report handling — it's fed by the multitouch frames, not raw HID reports).
- DONE: Apple's event driver wouldn't bind because our published interface advertises DeviceUsage `{0xFF00,12}`
  + no `MTEventSource` while Apple's personality wants `{0xFF00,1}` + `MTEventSource`. The
  **`MT2USBHIDEventDriver`** Info.plist personality (VID 1452/PID 613, high probe score — USB twin of
  `MT2HIDEventDriverBNB`) forces the bind: on-device the event driver now BINDS + attaches `IOHIDSystem`.
- STILL OPEN: the bound event driver is STARVED — `hidEventDriverPublished`'s frames-wiring isn't completing
  (no `AppleUSBMultitouchUserClient`; handleReport's `0x28` frames dropped at the enqueue gate). Next dig:
  did `hidEventDriverPublished` fire under manual-start (is StartFinalProcessing's notifier registered)?
  does the LocationID compare pass (dump event-driver LocationID vs genuine `+0x5ac`)? what do `*0x148`/`*0x20`
  do (addFramesClient/wrapper)? Needs a live diagnostic build. Prefpane is downstream of the same actuation.
### Genuine-USB physical/2-finger click + prefpane (manual-start path) — RE'd 2026-06-24

Cursor + scroll + 4-finger-swipe + tap-to-click already WORK on the genuine-USB path (reused pipeline →
`mt1_encode` 0x28 frame + Apple checksum). Two gaps remain; both are **manual-start-satisfiable** (no need
to switch to IOKit normal-match):

- **Physical + 2-finger click.** The recognizer's "clicky" gate is `parser-options` bit `0x2` (see
  `reference.md` → Properties). We must seed a value with it set (`39`); blast-radius RE confirms 39 changes
  only click capability, nothing else. **Button dispatch chain** (RE'd via `tools/re`): the button rides the
  SAME relative-mouse channel as the cursor — `forwardButtonState → _MTDeviceDispatchButtonEvent(dev,btn) →
  _mt_DeviceDispatchRelativeMouseEvent(dev,0,0,btn) → postRelativeMouseEvent`. The gate (`handleButtonState`,
  bit 0x2) is the gesture-side awareness (primary-vs-secondary by finger count); the raw click dispatch
  (`forwardButtonState`) is NOT gated. `forwardButtonState`/`_MTDeviceDispatchButtonEvent`/
  `_mt_PostButtonStateCallbacks` have no internal callers and **`hidd` references none of them** → the button
  apparatus lives entirely in the MultitouchSupport + recognizer-plugin layer (driven via CFPlugin COM), the
  SAME layer that processes our reframed frame. **OPEN (on-device-decided): does MultitouchSupport extract
  our frame's button (CompactV4 `flagA`, which `mt1_encode` carries) and fire `forwardButtonState`?** If yes,
  `parser-options=39` completes it; if 39 opens the gate but click stays dead, the next dig is that
  frame-button-extraction site in MultitouchSupport (also `_MTDeviceSetPickButtonShouldSendSecondaryClick`).
- **Prefpane.** The Trackpad pane imports only `IOServiceGetMatchingService` + `IOServiceMatching` (NO
  matching-notification API) — a one-shot **presence check by class** (`IOServiceGetMatchingService(
  IOServiceMatching("AppleUSBMultitouchDriver"))` sets a "present" flag). Our manual-started instance is
  `registered,matched` so it qualifies; the live auto-update comes from an `observerForService:` framework
  helper (the only manual-start-sensitive piece). So manual-start is fine; worst case fire the notification
  ourselves — not a `kIOMatchedNotification` requirement, not a normal-match forcing function.

Everything below is the layer-by-layer investigation that led here (kept for the audit trail).

### Genuine-USB contact frames malformed — current cursor blocker (2026-06-24)

Gate 1 is solved: seeding `HIDServiceSupport` + `IOCFPlugInTypes`→MultitouchHID.plugin (+ `IOUserClientClass`
+ geometry) in the manual-started driver's **init dict** makes `hidd` open `AppleUSBMultitouchUserClient`
(confirmed on-device; the personality-merge properties manual-start otherwise skips). But the cursor still
doesn't move, and a diagnostic interpose on the driver's `postRelativeMouseEvent` (vtable byte `0xb30`) stays
**silent** while `handleReport` keeps reframing — so the recognizer produces no pointer. `tools/mt_frames_probe`
(the contact oracle) shows **why: the contacts are garbage** — `n=4` contacts for one finger, lead contact
`state=0` (invalid touch ⇒ ignored), and normalized **X pinned to 0.000** while Y tracks fine.

Open sub-questions:
- **Why `n=4` not 1?** Our enqueued frame is 15 bytes (`[0x28][4-byte CV4 hdr][9-byte contact][2-byte cksum]`),
  so `(len-4)/9 = 1` — yet MultitouchSupport yields 4. It is **not** using our enqueue length; trace
  `_mt_ProcessPathFrame` / `_MTParse_CompactV4BinaryPath` for the frame-length source (candidate: the
  `"* Packet Size"` IORegistry prop the driver sets from `getMaxPacketSize`, or a header length field) — the
  parser is reading a longer buffer and slurping phantom contacts from the tail.
- **Why X normalizes to 0** while Y is correct (same geometry). Contact byte-offset vs the CV4 header size, or
  a sensor-region/X-range normalization (cf. the [[mt2-cursor-edge-clamp]] X-pin).
- **Regression vs session 2:** then (probe sole consumer) `c0` X *tracked*; now (hidd co-consumes) `c0` is
  garbage ⇒ a shared frame-buffer contention or a length bug exposed once hidd drains/reconfigures.

Fix the reframe (and/or `* Packet Size`) so the probe shows `n=1`, `state≠0`, X tracking **before** the next
on-device cursor test. The phantom-contact `(len-4)/9` boundary noted as "cosmetic" above is now load-bearing.

### ▶ ON-DEVICE TEST #2 — 2026-06-24 — reframe+checksum PROVEN; blocker = no frames client

The translate-and-feed build (branch `genuine-usb-translate-feed`) was loaded and tested on a clean
post-reboot box (residue cleared). **Bottom line: our reframe is correct and passes Apple's checksum; the
remaining blocker is that no `AppleUSBMultitouchUserClient` frames client is open to receive the validated
packets.** (This block records one mid-investigation wrong turn and its correction — read to the end.)

**Setup fact we didn't know (now part of the load sequence):** `allocClassWithName("AppleUSBMultitouchDriver")`
returns NULL unless the `AppleUSBMultitouch` kext is loaded — and removing the injected `613` IOCatalogue
personality (the reboot) ALSO removes the only thing that auto-loads that kext. So seam A needs an explicit
`sudo kextload /System/Library/Extensions/AppleUSBMultitouch.kext` first. Its on-disk personalities match
`idVendor 1452` + `bInterfaceNumber 1` + an `idProductArray` (`547,548,549,560…587…`, NOT `613`), so loading
it registers the class symbol **without** binding our device. Our `MT2USBReader` then matches the interface,
manual-starts a genuine `AppleUSBMultitouchDriver` (registered/matched/active, `Transport=USB`,
`Product="Magic Trackpad"`, 3 `IOHIDLibUserClient`s), and interposes its `handleReport` (vtable byte `0x8b8`
= slot `0x117` = 279; `tools/re vtable`-confirmed = `handleReport`).

**Data path PROVEN (persistent `/var/log/system.log`, NOT `dmesg` — reject spam rolls the small dmesg ring):**
- Device emits **21-byte `0x02` reports** → our SET_REPORT multitouch-enable (`{0x02,0x01}`, wValue `0x0302`)
  WORKED (device is in multitouch mode; the boot-mouse interface goes silent, which is why the generic-HID
  cursor stops the moment our path takes over).
- Our interpose reframes every report: `MT2hr[..]: reframe OK outlen=17 b0=0x60` (1 contact) / length 26
  (2 contacts). So the `0x60` framing + CompactV4 header + checksum reframe is being produced correctly.

**⚠️ FIRST READING WAS WRONG — corrected by kernelcache disassembly (same day). The reframe WORKS;
the blocker is downstream (no frames client).** The `dmesg` ring is tiny and the per-packet warning spam
rolled the checksum lines out of it, so an initial pass saw only `not in path binary mode, received 0x60
... length 26` and mis-concluded "path-binary mode is a hard gate." The PERSISTENT `/var/log/system.log`
plus disassembly of the ACTUAL running driver tell the real story:

- **`not in path binary mode` is a WARNING, not a gate.** RE'd `AppleUSBMultitouchDriver::validateChecksum`
  (running build, `0x4506`): at `0x455b` it tests `this->[0x179]` (the path-binary/mouse-mode flag) and, if
  zero, IOLogs `...not in path binary mode, received 0x%x ... length %d` — then **FALLS THROUGH** to the
  checksum compute. It never returns early. (The text says "handleReport" but the code lives in
  `validateChecksum`; `handleReport` just calls it.)
- **Checksum is the hard gate, and OUR reframe PASSES it.** `validateChecksum` = `sum(buf[0..len-3]) mod
  0x10000` compared to `(buf[len-1]<<8)|buf[len-2]` (low byte at `len-2`, high at `len-1`). Our
  `mt2_usb_bytes.c` (the checksum helper, formerly in `mt2_usb_reframe.c`) matches. Proof from `system.log`: of 5000+ `checksum is incorrect` lines, EVERY one is
  a raw `0x2` packet (21/8/2-byte) from the PRIOR spike; there are ZERO failures for our `0x60` packets
  (17-byte/26-byte). Our reframed packets validate.
- **Real blocker = validated packets are never DELIVERED.** After a valid checksum, `handleReport`
  (`0x4399`) only enqueues if `this->[0x170]` (a semaphore, created in `handleStart`, nulled in `init`/`free`)
  is non-null AND the `this->[0x168]` user-clients array has count > 0 (`callq *0x130`; `je 0x4436` skips
  enqueue when count==0). Live: **0 `AppleUSBMultitouchUserClient` instances** — no frames client is open, so
  `enqueueData` is never called → no frames → no cursor AND no prefpane (the pane needs a live MT device).
  The genuine instance has only 3 `IOHIDLibUserClient`s (HID), not the MT frames `AppleUSBMultitouchUserClient`.
- **NEXT:** get MultitouchSupport to open `AppleUSBMultitouchUserClient` frames on our manual-started
  instance (it isn't, today). The path-binary *warning* is cosmetic; don't chase it.

**Userspace consumer side — RE'd off-device (MultitouchSupport.framework), session 2.** Mapped how frames
clients are supposed to attach, to find why none did:
- `___MTDeviceCreateListForDriverType(type)` builds `IOServiceMatching("<class for type>")` and iterates
  `IOServiceGetMatchingServices` → `_MTDeviceCreateFromService` per match. For driverType 1 the class is
  **`AppleUSBMultitouchDriver`** — so enumeration WOULD find our instance (it is exactly that class).
- `_MTDeviceCreateFromService` (`0xe43`) ACCEPTS our device: `IOObjectConformsTo` maps
  `AppleUSBMultitouchDriver`→type `1` (stored `+0x8c`), Transport `"USB"`→`1` (`+0x90`); it only returns
  NULL for `Dummy` (type 3) lacking a `FramePumperPresent` prop. It reads the `"* Packet Size"` IORegistry
  property for the buffer size, then `_MTDeviceCreate`. So our service is BOTH discoverable AND acceptable.
- ⇒ **Discoverability/acceptance is NOT the theoretical blocker.** The actual frames-client open
  (`IOServiceOpen` of `AppleUSBMultitouchUserClient`) is done by the CONSUMER (WindowServer's
  MultitouchSupport client), which enumerates at its own startup + on a hotplug notification
  (`_mt_HotPlugMatchingDeviceAdded`). Our device was manual-started mid-session (after consumers were
  already up), so it depends on the hotplug path noticing our `registerService()`.
- **These last unknowns are LIVE-ONLY (can't confirm offline):** (a) does the hotplug notification fire for
  our manual-started/`registerService`'d instance? (b) does our service carry the props the consumer needs
  (`* Packet Size`, a Multitouch GUID/DeviceID, `MTHIDDevice`)? (c) does WindowServer actually `IOServiceOpen`
  it? **On-device probe for next session:** after manual-start, (1) `ioreg -w0 -l -c com_apple_driver_AppleUSBMultitouch`
  → does our instance have `* Packet Size` / GUID / `MTHIDDevice` props vs what `_MTDeviceCreateFromService`
  reads; (2) watch for an `AppleUSBMultitouchUserClient` appearing on touch; (3) if none, test forcing it —
  open the user client from a small userspace tool (mirror `MTDeviceCreateList`+open) to kick
  `registerUserClient`→`addFramesClient`→`configureDataMode`→streaming, and see if cursor/pane come alive.
  If that works, the fix is making our service auto-discovered (publish the missing props / fire the hotplug).

**RE GOTCHA — the running build ≠ the on-disk file (resolved).** `validateChecksum`'s path-binary branch is
ABSENT from the on-disk `/S/L/E/AppleUSBMultitouch` (240.10, Jan-11) but PRESENT in the booted build. Proof:
the reject string at file off `0x9376` is referenced by ZERO instructions in the on-disk binary, but the
running instance (load addr `0xffffff7f81af0000`) comes from the boot **kernelcache** (`/System/Library/
Caches/com.apple.kext.caches/Startup/kernelcache`, Jun-13) — a DIFFERENT build with the same `240.10` version
label. Both `handleReport`s are byte-identical except relocations; the divergence is in `validateChecksum`.
**Always disassemble the kernelcache build, not `/S/L/E`, for this driver.** Tooling added this session to do
that (all in `tools/` + `re/`): `tools/kc_lzss` (decompress `complzss` kernelcache → Mach-O), `tools/kc_carve`
(list segments / carve a prelinked kext out by vmaddr), `tools/macho_rebase` (rebase a carved kext's
vmaddrs+symbols to 0 so `tools/re disasm`/`tools/re syms` work — note it does NOT rebase vtable pointer VALUES, so read
vtable slots from the NON-rebased carve), `tools/re hex` (raw byte dump), and a `tools/re plist` `AppleUSBMultitouch`
alias. Carved+rebased copy: `captures/kc/AppleUSBMultitouch.rebased`. (`tools/re str-xref` quirk: it wraps the query
in literal quotes, so it only matches a WHOLE cstring — substring queries silently find nothing.)

**SIZING RE done 2026-06-24 (off-device):**
- **`validateChecksum` CRACKED — trivial.** 16-bit additive sum of bytes[0 .. n-3]; the last two bytes
  hold it little-endian (`low=byte[n-2], high=byte[n-1]`). Verified vs the live vector (`expected 0x499`
  = sum of bytes[0..18]; MT2's own trailing `0x0249` is a different scheme). A ~5-line encoder appends it.
- **The checksum is the hard gate (RE-CONFIRMED on-device 2026-06-24 test #2) — and our reframe PASSES it.**
  Disassembly of the running `validateChecksum` (`0x4506`) confirms: `not in path binary mode` is a WARNING
  that falls through; the checksum compare is the only thing that returns invalid. Live `system.log` shows
  ZERO checksum failures for our `0x60` packets. (A mid-investigation pass briefly mis-called path-binary a
  hard gate — that was a truncated-`dmesg` artifact; corrected in the "ON-DEVICE TEST #2" block below.)
- **Mode-coax route is OUT.** On seeing MT2's `0x2` packets (read as "mouse mode") the driver calls
  `configureDataMode`, which switches mode via Apple-specific feature reports (read/modify/write report id
  3 @8B, then set report `0xac` @1B). MT2 won't honor these → it won't natively emit Apple's format. So we
  can't just flip a mode; we **translate**.
- **Route = translate-and-feed:** emit Apple path-binary packets (header `0x60` + Apple's contact layout)
  + the now-known checksum, and interpose `handleReport` (instance vtable clone). Source contacts: we
  already decode them (`mt2_usb_decode`).
- **One sizing unknown left:** the exact `0x60` packet LAYOUT (contact struct fields/offsets/count). RE
  from `handleReport`'s parse (the `callq *0xb28` contact path) + cross-ref Linux `bcm5974`. That's the
  last read before a tight estimate.

**Effort: medium, bounded** — and likely SMALLER than feared. The kext doesn't parse contacts; after the
checksum it calls `AppleUSBMultitouchUserClient::enqueueData(raw,len)` → the format lives in MultitouchSupport's
parser family (`_MTParse_*BinaryFrameHeader` + `*BinaryPath`). **The decisive find:** `_MTParse_CompactV4BinaryPath`
computes contact count as `(length-4)/9` (signed div-by-9, `imul 0x38E38E39`) ⇒ format = **4-byte header + N × 9-byte
contacts** — and **MT2 is already a CompactV4 device** (its BT path uses `_MTCompactV4HeaderUnpack`, cracked during
tap-to-click; MT2 wire contacts are 9-byte records). So the parser wants the format MT2 already speaks. The
genuine-USB rejection is NOT a contact-layout mismatch — it's (a) framing/header (`0x02` report-id vs the `0x60`
path-binary the kext gates on) + (b) the checksum. So translate-and-feed ≈ **re-frame header + recompute Apple's
checksum**, contact bodies largely pass-through. Last confirmation before building: byte-align a captured MT2 USB
`0x02` packet against the CompactV4 frame the parser expects (header size, whether the enqueued length includes the
checksum / how the `(len-4)/9` lands). Then it's a small interpose at `handleReport`/`enqueueData` + a checksum fn.

**BYTE-ALIGN DONE 2026-06-24 — contacts are PASS-THROUGH (bit-for-bit).** Diffed `_MTCompactV4BinaryContactUnpack`
vs `src/mt2_decode.c`: X = `(b1&0x1F)<<8 | b0`; Y = `b1>>5 | b2<<3 | (b3&0x3)<<11` — IDENTICAL in both (we only
negate Y's sign). touch_major/minor = bytes 4/5, size = byte 6 (`&0x3F`), state in byte 3, id/orientation byte 8
— all the same bytes our decoder uses. So MT2's raw 9-byte USB contact IS the CompactV4 contact; no per-contact
transform. **Final genuine-USB build spec:** interpose `handleReport`/`enqueueData`; leave the 9-byte contact
bodies untouched; (1) reframe the header to the CompactV4 frame the parser expects (4-byte CompactV4 header —
packing already RE'd for tap-to-click: `ts=(b1>>2)|(b2<<6)|(b3<<14)` — plus the `0x60` path-binary framing the
kext gate wants) and (2) append Apple's 16-bit additive-sum checksum (replacing MT2's trailing bytes). SIZE =
SMALL: one buffer-rewrite shim + a ~5-line checksum. Only detail to pin while building: exact MT2-USB-header →
CompactV4-4-byte-header byte mapping + whether the enqueued length includes the checksum. No remaining unknowns
of consequence.

**PACKET LAYOUT PINNED 2026-06-24 (Task 0.2 RE — CORRECTS the "4-byte header" shorthand above).** Disassembled
`AppleUSBMultitouchDriver::handleReport` (file off `0x4196`), `::validateChecksum` (`0x4506`), the `enqueueData`
call site, and MultitouchSupport `_MTCompactV4HeaderUnpack` (`0x5c84`) / `_MTParse_CompactV4BinaryFrameHeader`
(`0x5ee3`) / `_MTParse_CompactV4BinaryPath` (`0x5f69`). The `0x60` is NOT part of the CompactV4 header — it is a
framing prefix that sits BEFORE it. The full packet `handleReport` validates and forwards is:
```
byte[0]            = 0x60                       framing magic — gated at handleReport+0xed (cmpb $0x60); CONFIRMED
byte[1]            = prefix/length byte         skipped by both content scans (start at +2); HYPOTHESIS = length
byte[2..5]         = 4-byte CompactV4 header    [type, flags+timestamp] — see below
byte[6 .. 6+9N-1]  = N x 9-byte contacts        pass-through bit-identical (proven)
byte[len-2..len-1] = 16-bit additive checksum   sum of bytes[0..len-3], little-endian; CONFIRMED (validateChecksum)
```
- **CompactV4 4-byte header** (`_MTCompactV4HeaderUnpack`): `cv4[0]`=frame/report type (copied verbatim, no value
  check); `cv4[1]`=`(ts&0x3F)<<2 | flagB<<1 | flagA` (low-6 ts + 2 flag bits); `cv4[2]`=`(ts>>6)&0xFF`;
  `cv4[3]`=`(ts>>14)&0xFF`. Timestamp packing `ts=(cv4[1]>>2)|(cv4[2]<<6)|(cv4[3]<<14)` (22-bit) — CONFIRMED, matches
  the tap-to-click RE (b1=cv4[1] etc.). Note cv4[1]'s low 2 bits are flags, not ts.
- **Contact count is length-derived, NOT in the header:** `_MTParse_CompactV4BinaryPath` computes `(flen-4)/9` on the
  FRAME (the post-`0x60`/prefix region the parser is handed, i.e. starting at byte[2]); `flen = 4 + 9N`. CONFIRMED.
- **enqueueData range = the WHOLE packet** including byte[0]=`0x60` and the 2 checksum bytes; `len` comes from the
  descriptor's `getLength` (`handleReport+0x69`), checksum-validated with that same len, then `enqueueData(%r14=buf,
  len)` at `handleReport+0x28b`. CONFIRMED. So total packet `len = 8 + 9N` (with a 2-byte `0x60`+prefix region).
- **Still pin on-device (static RE cannot decide — Apple's code never sees MT2's layout):** (1) prefix width 1 vs 2
  bytes (does the CV4 header start at byte[1] or byte[2]? → `len = 7+9N` vs `8+9N`); (2) `byte[1]` semantics;
  (3) the `cv4[0]` type constant a genuine CV4 USB frame uses; (4) the MT2-USB-header → `ts` source (locate a
  timestamp field in MT2's 12-byte header, or synthesize a monotonic full-res ts — the tap-to-click work showed a
  synthesized ts is acceptable to the recognizer). Resolve (1)/(2)/(4) with the Task 0.1 captured fixture.

**INTERPOSE SEAM PINNED 2026-06-24 (Task 0.3 RE).** Via `tools/re vtable` on the Apple kext:
- **`handleReport` vtable slot byte offset = `0x8b8`** → slot index `0x8b8 / sizeof(void*)` = `0x117` (279). CONFIRMED.
- **Signature CONFIRMED 3-arg:** `IOReturn handleReport(IOMemoryDescriptor *report, IOHIDReportType reportType,
  IOOptionBits options)` (not the 2-arg variant). Override C sig = `(OSObject *thisptr, IOMemoryDescriptor*,
  IOHIDReportType, IOOptionBits)`.
- **Clone span:** full vtable is `0xb70` bytes (`vtable for AppleUSBMultitouchDriver` @ `0xb5a0` → MetaClass vtable @
  `0xc110`); pass `span_bytes = 0x1000` to `vtc_clone_override` (covers it with margin; the helper adds the 2 ABI
  words itself). Reusing the BT path's `0x2000` is also safe.
- **Instance acquisition = (A) MANUAL-START** (mirrors the BT manual-start of BNB; endorsed by
  [[reuse-apple-code-construct-seam]]). `AppleUSBMultitouchDriver::gMetaClass` is an external symbol and its
  `MetaClass::alloc`/`init`/ctor are present → the class is externally constructable at runtime via
  `OSMetaClass::getMetaClassWithName("AppleUSBMultitouchDriver")`. Our `com_schmonz_MT2USBReader` matches the
  `IOUSBInterface` (vid 1452/pid 613/iface 1), instantiates + `init`/`attach`/`start`s the genuine driver on that
  interface, holds the pointer, clones slot `0x117`, and chains the saved original. **CRITICAL: our reader must NOT
  open the interrupt pipe in this route** — Apple's driver opens it itself; double-open would conflict. Route (B)
  (passive `addMatchingNotification` + post-start clone) rejected: races a report arriving before the clone installs.
  Teardown reverses with `vtc_restore` FIRST, then release the genuine instance ([[mt2-unload-while-streaming-uaf]]).

**BUILD DONE + FIRST ON-DEVICE TEST BLOCKED BY CATALOGUE RESIDUE 2026-06-24.** The translate-and-feed
build is complete on branch `genuine-usb-translate-feed` (checksum + reframe host-tested; manual-start +
`handleReport` interpose, flag `kGenuineUsb`). First on-device load: Apple's `AppleUSBMultitouchDriver`
bound the interface directly (matched `idProduct 613`, bundle `com.apple.driver.AppleUSBMultitouch`,
score 99000), our `com_schmonz_MT2USBReader` had **0 instances** and never ran `startGenuine` → no
interpose, no multitouch-enable → device emitted only 8-byte mouse-mode packets, all rejected. But 613
is **NOT in Apple's on-disk personalities** (`tools/re plist AppleUSBMultitouch` → zero "613"), and the box
had **not rebooted since the 2026-06-24 spike** that injected a live IOCatalogue personality
`idProduct 613 → AppleUSBMultitouchDriver`. **Injected catalogue personalities persist across
kextload/kextunload — only a reboot clears them** — and this residue pre-empted seam A (which needs OUR
reader to win the interface). Our code never ran; it is not implicated. NEXT: reboot to clear, reload
the genuine build, retest (does our reader now win + interpose + checksum-accept?). If Apple still grabs
613 after a clean reboot, seam A is flawed (pivot to intercepting Apple's match). Dev-loop lesson: verify
the *matching environment* (who won the match), not just the loaded binary. Full handoff in the
`mt2-genuine-usb-resume-here` memory.

**Cost/benefit (the real decision):** synthetic-USB already delivers working cursor+gestures. Genuine-USB
*adds* a genuine prefpane on USB and makes our role uniform across transports — on BOTH we'd just
translate MT2's native packets into what Apple's genuine driver expects and feed it (BT→BNB's AMD,
USB→`handleReport`). (Pane art is a non-issue per the user 2026-06-24 — the laptop animation is fine.)
Cost = RE Apple's USB packet format + `validateChecksum` (have dmesg test vectors), write the encoder,
interpose `handleReport` (instance vtable clone). This is the clearest concrete instance of the mission's
reusable engine capability — "translate a device's native format into whatever the target consumer
expects" ([[mt2-mission-interface-over-driver]]). Next cheap step if pursued: RE `validateChecksum` +
the packet layout off-device to size it. Until/unless pursued, **keep synthetic-USB** (works today).
Relates to [[mt2-prefpane-detection-mechanism]], [[mt2-prefpane-asset-swap]].

---

## Cold-boot and sleep/wake flap rate — unmeasured

**Measured clean (2026-06-22):** warm BT reconnect = 0 flaps across all observed cycles (CONNTRACE
oracle, all `STEADY`), with the committed defer-0xF1 fix. **Not measured:** cold boot (where the
historical "no cursor at boot" actually lived) and sleep/wake (classically fragile). Until measured
with the same oracle, we don't know whether the flap is genuinely put to bed or merely absent in the
warm-reconnect case.

**How to measure:** after a reboot (or a sleep/wake), `sudo sysctl debug.mt2_log=1` then
`dmesg | tools/re conn-trace` → STEADY/FAIL for that boot's first connect. A `FAIL` timeline that never
reaches `INTERRUPT_BOUND` ⇒ PSM 19 didn't open ⇒ the targeted fix is `waitForChannelState(OPEN)` on
PSM 17 (with, finally, a real repro to verify against). See `reference.md` → BT connect handshake for
the genuine sequence and `how-to.md` → fix the connect flap. Relates to [[mt2-bt-attach-flap-rootcause]].

---

## Which control-channel transition does the device key on to open PSM 19?

**Known (RE'd):** PSM 19 is device-initiated, and the genuine driver provokes it only by *correctly
accepting the control channel* (`listenAt`-bound + `waitForChannelState(OPEN)`), sending no HID
command first. **Not provable by static RE:** *which exact* control-channel state transition the
device watches for — pure L2CAP OPEN vs. `listenAt`-acceptance vs. an L2CAP-config detail.

**Why it matters:** if reproducing the genuine *order* (the flap fix above) doesn't fully fix
cold-boot/sleep-wake, we won't know which sub-step we're getting wrong without seeing the wire.

**The one justified live capture:** an `hcidump`/L2CAP trace of a genuine-driver connect vs. our
reader's connect, diffed on the control-channel exchange right before the device opens PSM 19. Only
needed if reproducing the order doesn't fix the flap — not up front.

---

## Trackpad prefpane live-update misses our manually-started driver (open, RE'd 2026-06-25, systematized 2026-06-28)

**Symptom (refined by the 2026-06-28 systematic run):** an open Trackpad pane fails to settle on a USB
trackpad live ONLY when System Prefs was launched while on BT; launched on USB it tracks both transports
live, and USB→BT always live-updates. A fresh launch always shows the right state. (Cosmetic — relaunch
is the workaround — but we want live updates both directions.) See the clean-room characterization below.

**Mechanism (RE'd, solid):** the pane's live update is an `IOServiceObserver` (in
`PreferencePanesSupport.framework`), stored in `MTTrackpadController.mMagicTrackpadServiceObserver`.
`-[IOServiceObserver initForService:target:selector:]` (PreferencePanesSupport @0xde8f) registers TWO
notifications via `IOServiceAddMatchingNotification` on the MT matching dict:
`kIOFirstMatchNotification` ("IOServiceFirstMatch") → `__ioServiceConnectCallback`, and
`kIOTerminatedNotification` ("IOServiceTerminate") → `__ioServiceTerminateCallback`; each invokes the
pane's target/selector to reload. So the pane IS wired for live appear+terminate. The fresh-launch path
works because the notification's iterator is pre-drained for already-present services (and the pane also
does a one-shot `IOServiceGetMatchingService` presence check) — that's why relaunch always lights it.

**Hypothesis FALSIFIED 2026-06-25 — the IOKit layer is healthy.** Built `tools/mt_svc_observe.c` (mirrors
the pane: registers the SAME `kIOFirstMatchNotification` + `kIOTerminatedNotification` on both classes,
logs each callback). On-device, powering the MT2 off→on and hot-swapping BT↔USB **both directions** fired
EVERY expected notification with fresh registry IDs each time (clean teardown + fresh instance):
`TERMINATED AppleUSBMultitouchDriver` on off/unplug, `FIRST-MATCH AppleUSBMultitouchDriver` on on,
`FIRST-MATCH BNBTrackpadDevice` on fall-to-BT, etc. So our manual-started drivers **DO** deliver exactly
the notifications the pane's `IOServiceObserver` listens for — `registerService()`/`terminate()` are not
the gap.

**Therefore the earlier "open pane didn't light" was NOT a notification-delivery failure.** (The IOKit
layer is healthy; `tools/mt_svc_observe` is the standing oracle for "are the notifications firing?".)

**SYSTEMATIC CHARACTERIZATION 2026-06-28 (supersedes the prior "probably transient / nothing to fix"
guess — that was unsystematic and is WRONG).** Ran the decisive open-pane retest physically, clean-room
(Cmd-Q System Prefs between every trial to clear accumulated process state), capturing device-truth
(`re mt-devices`/`re ioreg-class`) alongside the pane appearance. Baseline matrix + clean-room methodology:
`docs/mt-stack/prefpane-test-runthrough.md` → "Clean-room baseline (pre-fix)". Reproducible result:
- **Launch System Prefs while on USB → the open pane tracks BOTH transports live** (BT↔USB both ways, repeatably OK).
- **Launch while on BT → on a USB appear the open pane shows USB UI for a MOMENT, then reverts to "No
  trackpad found"** — every time, no "learning" from a prior USB exposure. (An earlier "OK on 2nd plug"
  reading was a contamination artifact: System Prefs process kept alive across a window-reopen-on-USB.)
- **USB→BT (fall back to BT) with the pane open ALWAYS live-updates correctly**, regardless of launch transport.
- A **fresh** launch always shows the right state (one-shot presence check + pre-drained iterator).

**ROOT CAUSE (reconciled, now precise) — it's a pane REDRAW/VIEW-SELECTION gap, not notification delivery
and not total USB blindness.** The brief USB-UI flash proves the pane DOES receive the USB FirstMatch
notification; it then can't *establish* the USB trackpad view live. USB display is set up from the
fresh-launch detection path (nib state `mBaseNibName` / cached `mFoundBTTrackpad`); the live reload can
toggle BT-found vs NoTrackpad but has no live path to render a USB trackpad unless the process was
launched on USB. So: notification arrives (flash) → reload finds no live USB-render path → NoTrackpad.

**DISCREPANCY RESOLVED 2026-06-28 (disasm `tools/re`, systematic finding wins): the live observer is
BT-ONLY.** In the pane's arm/detect routine (Trackpad @~0x2300–0x2440): `IOServiceMatching("BNBTrackpadDevice")`
-> `observerForService:target:selector:` (0x232e) is the ONLY live `IOServiceAddMatchingNotification`-class
observer (a 2nd `observerForService` at 0x38af is ALSO BNBTrackpadDevice). USB is handled by a ONE-SHOT
`IOServiceGetMatchingService("AppleUSBMultitouchDriver")` presence check (0x23be), as is the IOPropertyMatch
on `com.apple.AppleMultitouchTrackpad` (0x2414) and BT itself (0x2397). So USB has NO live observer.
=> The flash on BT->USB is the BT TERMINATE callback (MT2 drives one transport at a time, so cabling USB
drops BT) reloading + transiently re-detecting USB, before the terminate path forces mFoundBTTrackpad=0 ->
NoTrackpad. A USB change that doesn't move BT fires nothing.

**FIX TARGET (maps to the two goals).** MAIN (reliable live detection): give USB a live observer
(kIOFirstMatch + kIOTerminated on AppleUSBMultitouchDriver) that drives the SAME reload/availability path the
BT observer does — symmetric with BT. SECONDARY (no intermediate flash): make the reload pick the final view
in one pass (re-detect BT+USB+property-match, choose the winner) instead of letting the BT-terminate branch
force NoTrackpad before USB is honored. Delivery = the proven osax injection
([[mt2-prefpane-osax-injection-mechanism]]); we add the missing USB observer rather than poking a single
callback. Re-running the clean-room matrix (`docs/mt-stack/prefpane-test-runthrough.md`) is the done-bar.

**✅ RESOLVED 2026-06-29 — done-bar MET (shipped via the standalone-osax watcher loader).** The fix (suppress
the pane's own observer; observe BOTH transports ourselves; recompute via `[pane loadMainView]`; coalesce
appear-250ms / removal-1300ms so a USB-appear supersedes the BT-removal) is delivered by the osax + launch-
watcher (no SIMBL). The FULL clean-room matrix was re-run on-device through the loader and every row PASSES
(Rows 1/2/1b/3a/3b/4a + the capture-race) — see `decisions.md` → "Task C.2 FULL matrix" and the
runthrough/baseline in `prefpane-test-runthrough.md`. MAIN goal (reliable live USB detection) met; only-open
= the cosmetic single-flash on a redraw (SECONDARY; Apple's `loadMainView` own rebuild, not our logic).

## Three-finger-drag toggle: a live-USB availability RACE (RECHARACTERIZED 2026-07-10)

> **UPDATE 2026-07-10 — NOT "hidden on USB", NOT "Apple's per-transport difference".** On-device (Stage-2
> pref-dict work + the full prefpane matrix re-walk): the 3FD toggle **shows on a FRESH launch on USB** and
> the USB node DOES publish top-level `TrackpadThreeFingerDrag=true` (confirmed via `tools/re ioreg-props
> AppleUSBMultitouchDriver`). It drops only on a **LIVE USB appearance** (a BT→USB switch, or a
> NoTrackpad→USB power-on — even though that fires `loadMainView`). So `_isServiceAvailable:
> @"TrackpadThreeFingerDrag"` (= `IOServiceGetMatchingService` for a node with that top-level property)
> **races the USB node's property publication**: fresh launch → node already populated → available; live
> appearance → query runs before the just-appeared node is queryable → unavailable → the gesture row isn't
> built. BT shows it live because BT's 3FD availability comes from persistent system state, not a
> just-appeared node. **Supersedes the old "shown on BT / hidden on USB" framing AND the runthrough's
> "difference is Apple's, not ours" note — both WRONG.**
>
> **Fix direction (not built):** re-evaluate 3FD availability AFTER the USB node settles — a short deferred
> gesture-availability refresh on a USB appearance, or have the 2s reconcile tick force a gesture-array
> rebuild once it sees a matching USB node. The SEED is already correct (Stage 1 proved it byte-identical);
> this is purely a query-timing fix. Repro: live BT→USB switch with the pane open → 3FD row absent;
> relaunch → present.

## Prefpane transition cosmetics — two Apple-level-UX items (surfaced 2026-07-10 matrix re-walk)
Both pre-existing (NOT the capture-race render fix `e730175`, NOT the presence-SM unification); both block
"Apple-level UX" on transport transitions. Recorded so we come back to them.

- **① USB→BT NoTrackpad flash.** On a USB unplug, the USB driver terminates immediately but BT is deep-idle;
  the handoff daemon wakes it in ~1–1.5s. The removal-window **HOLD (1300ms)** absorbs the gap IFF BT wakes
  within it (→ same-view switch, no flash); when BT wakes slower, HOLD elapses → brief NoTrackpad → then BT.
  So it's variable BT-wake latency vs a fixed 1300ms window. NoTrackpad is device-truth-correct during the
  gap, but cosmetically jarring. **Constraint (decisions.md "USB enumeration timing"):** at removal time you
  can't distinguish a USB-unplug-handoff from a genuine USB power-off faster than ~1s, so you can't just
  "hold longer" (a real power-off would then show a stale view). Levers: make BT wake *reliably* <1300ms, or
  widen HOLD and accept a longer stale view on a real power-off. Needs its own investigation.
- **② BT power-on / reconnect tap-to-stream** — see the dedicated analysis section below.

## ② BT power-on tap-to-stream — off-device analysis (2026-07-10)
On BT (re)connect the device registers (`BNBTrackpadDevice`) but the pane momentarily shows NoTrackpad until
a touch (`MT2BTReader: … retrying gently until first frame`). Our SM renders `ON_BT` correctly (0 skips,
device present). Pre-existing; kext/device-level; related to [[bt-reconnect-enable-fails]].

**Mechanism (banked, MT2BTReader.cpp + reference.md/explanation.md).** Control (PSM17) + interrupt (PSM19)
channels bind → the `0xF1` enable is DEFERRED to `reEnableInGate` (firing it pre-OPEN blocks ~14s and flaps
the link) → `interposeTimerFired` re-sends `0xF1` (250ms ×8, then 1s "gently") because BNB's `handleStart`
knocks the device back to mouse mode (report 0x02) after our initial enable; the loop stops when
`bt_interpose_shim` decodes the first REAL `0x31` frame (`gSteadyConn = gConnId`).

**KEY REFRAME — the naive fix is a dead end.** MT2-over-BT is EVENT-DRIVEN (`src/mt2_pipeline.h`
`MT2_EVENT_DRIVEN` = "size-0 contact on lift, then silence"): an idle device emits NOTHING, so a *real* first
frame is physically impossible without a touch. So "provoke the first frame device-side (enable/nudge)"
cannot work. It's a **bootstrap circularity**: the kext (and the pane, downstream) treat BT as functional
only once a frame flows, but an untouched device sends none — everything waits on the tap.

**Fix shape — a SYNTHETIC kickstart frame; reuse the feed we own, NOT the deleted synthetic driver**
([[mt2-synthetic-removal-plan]] was removed for genuine-reuse and must NOT be revived — we keep Apple's
recognizer). We already own the feed into the genuine AMD and already synthesize frames in the session
(absence/pump/lifecycle, `mt2_session.c`); inject one empty/liftoff `0x31` on reconnect (which
`mt2_bt_decode`→`mt2_decode` accepts — it is the device's own idle shape) to bring the AMD/pane up without a
touch.

**CRUX RISK + mitigation (decouple display from device-mode).** A synthetic frame fixes the DISPLAY but not
the device's MODE — if the enable hasn't landed (device still mouse-mode, the `bt-reconnect-enable-fails`
case), the pane would LIE (shows BT; a real touch → rejected `0x02`). Mitigation: inject the synthetic frame
for the pane but KEEP the re-enable retry running — `gSteadyConn` is set ONLY by `bt_interpose_shim` on a
REAL decode, so the synthetic frame never satisfies the enable gate and the retry continues until the device
genuinely streams. Residual = a brief "pane says BT but touch not yet live" window in the rare enable-slow
case (measure on-device).

**Pane-revert half — the earlier disasm read was WRONG; CORRECTED on-device 2026-07-10.** A prior pass claimed
`loadMainView` gates the BT trackpad view on `ApplePreferenceIdentifier=="com.apple.AppleMultitouchTrackpad"`
(the "functional-AMD property"). **DISPROVED on the box:** in a fully-working BT state (`BNB=1, AMD=1,
MTDevice=1`, `multitouch confirmed`), `ioreg -l | grep ApplePreferenceIdentifier` returns NOTHING — that
property never exists on this system — yet the pane shows the **BT trackpad view** (user-confirmed). So the
property does NOT gate the view; the pane tracks `BNBTrackpadDevice` **presence** (the original assumption).
The `loadMainView` @0x2421 nib assignment (USB-or-property) is therefore not the operative BT path; the exact
mechanism (likely `mFoundBTTrackpad` set by BNB presence, and/or our osax's `_magicTrackpadAction` replay) is
not fully pinned but is MOOT — empirically the pane shows BT iff BNBTrackpadDevice is present. **Lesson: ground
an RE conclusion against a live registry BEFORE writing it down (this was the 3rd static-RE overclaim of the
session).**

**② is really TAP-TO-CONNECT, not tap-to-stream (on-device 2026-07-10).** On a MANUAL BT power-on (no USB
event → the handoff daemon's `openConnection` wake never fires), the deep-idle MT2 does NOT establish the BT
link on its own: after power-on the syslog stayed EMPTY (0 new lines) and `BNBTrackpadDevice` never appeared —
until a physical TAP, which drove the ENTIRE chain at once (both L2CAP channels bind → BNBTrackpadDevice
manual-start → `0xF1` enable → `0x60` trigger → `multitouch confirmed (first frame after 6 enables)`). The
earlier "found briefly → NoTrackpad" is a NON-SUSTAINED connection attempt (BNB flashing), NOT a property gate.
So the pane's NoTrackpad-until-tap is just BNBTrackpadDevice being absent until the link establishes, and the
tap is what establishes it. **Fix direction — connection establishment, not frames:** proactively wake the
paired MT2 when it's powered-on-but-disconnected — the same `openConnection` the USB-unplug handoff uses, but on
a non-USB trigger (a periodic wake attempt while paired-but-disconnected, or a device power-on signal). NB the
handoff's `openConnection` itself returned `0x00000004` (FAILURE) on a recent USB-unplug — the wake path is not
100% reliable and needs its own look ([[bt-attach-flap-rootcause]], [[mt2-usb-unplug-bt-handoff]]). The
event-driven "no frame without a touch" fact still holds for STREAMING once connected, but the CONNECTION is
the prior blocker on a manual power-on — the synthetic-kickstart-frame idea only becomes relevant AFTER the
link is established, and even then only for streaming, not for the pane (which already shows BT on presence).

**ON-DEVICE GROUNDING (2026-07-10 evening) — the wall is host-initiated WAKE, and it's (so far) unsolved.**
- **A physical click is the only reliable way to bring the disconnected MT2 up.** User-confirmed, deterministic:
  "if I don't click, it never leaves the NoTrackpad screen." The intermittent "Found Mavericks Trackpad 2"
  flash has no discernible pattern (transient BT-level attempts that don't sustain).
- When it comes up (always after a click, every captured instance) the full chain fires cleanly: both L2CAP
  channels bind → `BNBTrackpadDevice` manual-start → `0xF1` enable → `0x60` trigger → `multitouch confirmed
  (first frame after N enables)` (N=1 and N=6 seen) → sustained → BT UI. The pane tracks BNBTrackpadDevice
  presence throughout (re-confirms the property-gate was wrong).
- Powered-on-but-not-clicked: `BNB=0`; the syslog shows the prior connection's `BT-`/terminate and NO
  subsequent `BT+` — it does not re-establish on its own.
- **Proactive host wake FAILS both ways tried:** (a) `sbin/mt2_bt_bounce` (CoD-matched `openConnection`) →
  "no matching device among 2 paired (looked for CoD 0x594)" whether the device is off OR powered-on-disconnected
  — CoD is live-sourced on connect, so a disconnected device has no CoD for blued to match (mt2_bt_bounce only
  bounces an ALREADY-connected device); (b) the USB→BT handoff's address-based `openConnection` returned
  `0x00000004` on a recent unplug.
- **⇒ ② is really "host cannot wake the deep-idle/disconnected MT2 without a physical touch."** Likely Apple-BT
  peripheral behaviour (device-initiated reconnect; a touch wakes its radio) that the host may be unable to
  override. Next-session leads (all on-device, start from FACT not the disproved framings above): decode the
  `openConnection 0x4` IOReturn + why; try an ADDRESS-based bounce (not CoD) against a paired-but-disconnected
  device; determine whether the host can page/wake it at all while idle. If it genuinely can't, ② may be
  unfixable from the host and the answer is the USB-OOB "plug in once → auto-paired → unplug to go wireless"
  onboarding ([[mt2-usb-oob-pairing-api]], [[mt2-bt-onboarding-designs]]) rather than a wake hack.

## Three-finger-drag toggle — prior RE (2026-07-03), superseded by the 2026-07-10 recharacterization above

**User goal:** parity — expose the three-finger-drag toggle (and the working gesture) on USB as on BT.

**Pane mechanism (proven, Trackpad.prefPane):** `-[BaseTrackPadController awakeFromNib]` sets
`mBuildin3FDragAvailable = [self _isServiceAvailable:@"TrackpadThreeFingerDrag"]`. `_isServiceAvailable:@"X"`
(`-[BaseTrackPadController _isServiceAvailable:]` @0x2ae8) = `IOServiceGetMatchingService({ IOPropertyMatch:
{ X : kCFBooleanTrue } })` → YES iff SOME IOService in the registry carries top-level property `X=true`.
Every gesture toggle gates the same way (`TrackpadSecondaryClickCorners`→mCornerClickAvailable,
`TrackpadMomentumScroll`→mBuildinMomentumScrollAvailable, `TrackpadFourFingerGestures`, `TrackpadEditing`→
mBuildinEditing). So the toggle shows iff a node publishes `TrackpadThreeFingerDrag=true`.

**The twist (checked our own kext — do this BEFORE theorising from the pane side):**
- USB `MT2USBReader.cpp:198-211` ALREADY seeds `TrackpadThreeFingerDrag=true` at the TOP LEVEL of the
  genuine AppleUSBMultitouchDriver init props (`initp`) — plus Momentum/FourFinger/SecondaryClick — AND
  inside `MultitouchPreferences` + `TrackpadUserPreferences`.
- BT `MT2BTReader.cpp:364-366` (`bt_build_bnb_props`) seeds Momentum/SecondaryClick/FourFinger in
  `DefaultMultitouchProperties` but **NOT** ThreeFingerDrag.
- Yet the user sees **BT shows the toggle, USB hides it** — the INVERSE of "seed the top-level property →
  toggle appears." So the availability query resolves true on BT / false on USB for a reason other than our
  seed presence.

**Hypotheses:** (a) our USB top-level seed doesn't survive onto a registry node
`IOServiceGetMatchingService` matches (initp consumed/moved by the genuine driver's `start`); (b) BT's
3FDrag comes from the genuine AMD's own defaults / MultitouchSupport-by-device-family, not from any props we
set (createMultitouchHandler copies `DefaultMultitouchProperties`→AMD, but 3FDrag isn't in ours → it's
Apple's). **REAL DIAGNOSTIC (on-device, BOTH transports):** `ioreg -l -w0 | grep -i -B25
TrackpadThreeFingerDrag` — which node carries it top-level on BT (the one the pane finds), and does the USB
node actually carry our seed at top level? THEN the fix = put the property on the matchable node/level on
USB. There is NO off-device one-liner; the diagnostic must run first. (Memory: `mt2-three-finger-drag-usb-parity`.)

---

## Prefpane shows "No Trackpad Connected" on synthetic USB — ROOT CAUSE CONFIRMED (2026-07-15); RESOLVED BY DECISION (2026-07-16)

> **RESOLVED 2026-07-16 → `decisions.md` "② subclass … USB=genuine, BT=synthetic".** The two candidate fixes
> both fell: ② subclass **panics** (bare `IOHIDDevice`-lineage `registerService`), ③ device-emulate is futile
> (AMD spawns only from the `IOUSBInterface` transport driver). Decision: **USB goes genuine** — Apple's real
> `AppleUSBMultitouchDriver` starts on the MT2, which lights the pane **natively** (no swizzle, no synthetic
> node). This entry is kept for the root-cause RE; the fix is the genuine-USB recovery, not anything here.

**Symptom (verified live, screenshot):** with synthetic-USB connected and gestures working, the Trackpad
prefpane renders the **NoTrackpad** view ("No Trackpad Connected", Continue/Go Back). Anticipated when the
synthetic pivot deleted the genuine nodes.

**Root cause (RE-confirmed, three ways):** Apple's `-[Trackpad loadMainView]` decides Trackpad-vs-NoTrackpad
**solely** by `_IOServiceMatching("AppleUSBMultitouchDriver")` (USB) / `("BNBTrackpadDevice")` (BT) →
`IOServiceGetMatchingService` → sets `mFoundBTTrackpad` → picks nib (`MTTrackpadController` vs `NoTrackpad`).
Both genuine classes are now `count 0` (deleted by `292a08d` USB / `89cad00` BT). We publish a fabricated
`AppleMultitouchDevice` instead, which the pane **never** matches. `_magicTrackpadAction:deviceConnected:`
only manages the BT battery UI (re-matches `BNBTrackpadDevice` when `connected=0`); it does NOT pick the nib.
`_isServiceAvailable:` (a *different* method: `IOServiceMatching("IOService")` + `IOPropertyMatch{prop:true}`)
runs only *after* awakeFromNib to gate per-gesture options (3FDrag/cornerclick/4-finger) — the secondary
[3FD-parity] concern, not the view decision. So `loadMainView` is the sole gate.

**FIX OPTION A — kext presence-shim subclass (VALIDATED viable both gates; likely cleanest):** publish a
node that **subclasses** `AppleUSBMultitouchDriver` (USB) / `BNBTrackpadDevice` (BT). Then Apple's
`loadMainView` matches it **natively** (no swizzle) and lights up. Both gates checked 2026-07-15:
- **Subclass matching works:** `loadMainView` uses `_IOServiceMatching` → `{IOProviderClass:name}`, and IOKit
  matches IOProviderClass by `metaCast` → class **and all subclasses**.
- **Apple classes are subclassable:** `__ZN24AppleUSBMultitouchDriver10gMetaClassE` and
  `__ZN17BNBTrackpadDevice10gMetaClassE` are **exported** (`nm -g`, `S`), with `superClass` exported.
  → declare a dependency on `com.apple.driver.AppleUSBMultitouch` / `AppleBluetoothMultitouch`,
  `OSDefineMetaClassAndStructors(OurShim, AppleUSBMultitouchDriver)`, override `start()` to just
  `registerService()` (NO `super::start()` → never drives hardware), existing purely to satisfy matching while
  the fabricated AMD carries input. Threads the needle between full-synthetic and genuine (cf.
  `decisions.md` / the iphone2g&3gfan hybrid noted in memory `genuine-vs-owned-device-reeval`).
  **Caveats:** (a) re-introduces a dependency on the Apple classes the SP1/SP2 rip-out deliberately shed;
  (b) matching is **system-wide** — anything matching `AppleUSBMultitouchDriver` now finds the shim, not just
  the pane; verify no other Apple consumer tries to drive/prod it; (c) confirm registration timing + that an
  inert `start()` is safe for these particular classes.

**FIX OPTION B — osax `loadMainView` swizzle (no fishhook):** swizzle `+[Trackpad loadMainView]`; when a
trackpad is present but Apple picked NoTrackpad, re-run Apple's OWN factory
`[self controlerForNIBName:@"MTTrackpadController"]` (reuses nib + awakeFromNib + feature-gating). ObjC-only,
fits the existing osax. BUT it depends on the osax reliably DETECTING the device in-pane — which currently
fails (see the anomaly below). Keeps everything userland, no kext coupling, but adds osax weight and inherits
the detection puzzle.

---

## In-pane presence poll returns usb=0 for the synthetic reader — UNEXPLAINED (2026-07-15); MOOT under genuine-USB (2026-07-16)

> **MOOT 2026-07-16.** This anomaly only mattered because the osax was driving the pane's present/absent for a
> *synthetic* USB node. Per the USB=genuine decision (`decisions.md`), a real `AppleUSBMultitouchDriver` is
> published, so the pane detects USB **natively** and the osax poll is no longer the view's presence signal.
> Left recorded (not chased) — revisit only if the osax still needs an in-`SysPref` USB poll for some other
> reason (e.g. battery/transport UI), where the same discrepancy could resurface.

The osax's `presence_observer_reconcile` (polls `service_present("com_schmonz_MT2USBReader")` =
`IOServiceMatching`+`IOServiceGetMatchingService`) reports **usb=0 inside System Preferences** (no
`reconcile … -> action`, no `perform: ON_USB` in syslog for pids 17113/21615), so even with reconcile-at-open
(committed, deployed 19:18 build is current) the pane is never driven to the trackpad view. YET:
- a standalone probe (`clang`; `IOServiceGetMatchingService("com_schmonz_MT2USBReader")`) as the **same uid
  501, unsandboxed** returns **FOUND (0x1007)**, stably (4×);
- the node is `registered, matched, active`, `IOProviderClass=IOUSBInterface`;
- System Preferences is **not** sandboxed (0 app-sandbox entitlement, no sandboxd denials);
- under the **old** kext at 19:15 the same in-pane reconcile returned **usb=1** → `ON_USB` and the pane lit up.

So the *new* synthetic reader node is matchable by a standalone uid-501 process but **not** by System
Preferences, while the *old* reader node was matchable by both — same class name, same process uid. Not
staleness, not sandbox. **NEEDS on-device instrumentation:** add a temp log to `service_present` printing the
raw `io_service_t` per class *inside* SysPrefs, reopen the pane, read what it sees. Note: `AppleMultitouchDevice`
IS matchable standalone (probe FOUND) — if it's also matchable in-pane, keying "light up" detection on the
transport-agnostic AMD (presence is all the view needs) could sidestep this. Option A (subclass) makes the
whole anomaly moot for the view (native match, no poll).

---

## BT reconnect-on-click at the login screen — an owned-BT REGRESSION (recharacterized 2026-07-16)

> **RECHARACTERIZED 2026-07-16 (user): this is a regression the genuine→synthetic pivot introduced, not a
> mystery.** Earlier **genuine-BT reliably reconnected on click at the login screen** (recent genuine for sure;
> earlier synthetic unknown). Owned/synthetic BT does not. Apple's `IOBluetoothHIDDriver` provides
> host-initiated reconnection of a *paired* HID device for free; by binding L2CAP directly (not going through
> that driver) owned-BT lost the hook. So the fix is to **recover reconnection inside owned-BT** — register the
> paired MT2 for reconnect / keep page-scan armed / re-arm on the click — NOT to reconsider genuine-BT (which is
> disqualified by its teardown panic regardless; see `bt-decisions.md` §4, the two-part forcing conjunction).
> The MT2 is confirmed bonded (blued link key present for `04-4b-ed-ec-02-07`), so it is not an unpaired-device
> problem. **Intermittent** — a fresh boot sometimes connects (e.g. 2026-07-16 16:46 did). Catch a failure live
> with `tools/re bt-timeline` and read the ladder: no `*MT2*`/`[PAGE]`/`[blud]` near the click = device didn't
> page / host didn't scan (recover the reconnect trigger); `[blud]*MT2*` + connection but no `[OURS]` = link
> formed, our reader missed it. Original 2026-07-15 write-up (mis-framed as "link-layer upstream") below.

### Reconnection mechanism — RE'd 2026-07-16 (corrects the "owned lost Apple's hook" hypothesis)

**Ground truth of the current owned reader (reconciles the stale genuine-BNB RE elsewhere in the KB).**
`com_schmonz_MT2BTReader::start` takes the matched `IOBluetoothL2CAPChannel` as its provider
(`MT2BTReader.cpp:270`), then `listenAt(self, incomingData)` on the interrupt channel (PSM 19 = 0x13) and
sends the deferred `0xF1` on control (PSM 17 = 0x11). **No BNB is manual-started, no delegate is interposed**
— those descriptions in `reference.md`/`explanation.md` are genuine-BNB-era (deleted `89cad00`). Our reader is
a *consumer that matches an already-published channel nub*.

**The reconnection ladder (RE'd from IOBluetoothFamily / IOBluetoothHIDDriver on this 10.9 box):**
1. **Host page-scan enabled** — `IOBluetoothHCIController::BluetoothHCIWriteScanEnable` /
   `CallWriteScanEnableToEnableScan` / `…WritePageScanActivity`. Without it the host never hears the device page.
2. **PSM 17/19 on the allowed-incoming list** — `IOBluetoothHCIController::AddAllowedIncomingL2CAPChannel` /
   `IsAllowedIncomingL2CAPChannelForDevice` / `RemoveAllowedIncomingL2CAPChannel`. The incoming L2CAP connection
   is only accepted if its PSM is allowed for that device.
3. **Channel nub published → a consumer matches it.** Both Apple's `IOBluetoothHIDDriver`
   (`IOProviderClass=IOBluetoothL2CAPChannel`, `PSM=17`, confirmed in its Info.plist) **and our
   `MT2BTReader`** are equal consumers here — identical matching layer.

**Correction to the earlier hypothesis.** `IOBluetoothHIDDriver` does **not** import `AddAllowedIncomingL2CAPChannel`
or `*ScanEnable` (checked its symbols) — the HID driver does *not* register scan / allowed-incoming itself. Those
are `IOBluetoothHCIController` methods driven via `IOBluetoothHCIUserClient::DispatchHCIWriteScanEnable` etc., i.e.
**`blued` (userland) manages page-scan + allowed-incoming system-wide from the paired-device database**, independent
of which kext consumer matches the channel. So **owned-vs-genuine at the consumer layer does NOT change reconnection
acceptance** — steps 1–2 are the same whether Apple's HID driver or our reader would consume the nub. The regression
is therefore *not* "owned dropped Apple's HID reconnection hook."

**So what differs (genuine reliably reconnected, owned intermittent)?** Narrowed to, in order of likelihood:
(a) **lower-layer page-scan / device-wake intermittency** that is independent of the terminal choice (steps 1–2,
blued/controller state) — plausibly not causally tied to genuine-vs-owned at all; (b) **link-keepalive**: genuine-BNB
may have held the ACL link up (or re-armed scan) so a true reconnect was rarely needed, where owned lets it idle-disconnect;
(c) a boot-order/timing effect at login. **The live `bt-timeline` capture decides which**, mapped to the ladder:
- **No `*MT2*` / no ACL at all near the click** → step 1/2: host wasn't page-scanning or PSM not allowed-incoming
  (blued/controller state) — RE target is **blued** (the scan / allowed-incoming manager) + `IsAllowedIncomingL2CAPChannelForDevice`.
- **ACL/`[blud]*MT2*` forms but no channel** → step 2: allowed-incoming missing for PSM 17/19.
- **Channel forms (`[OURS]` setupInGate) but no stream** → our side (the enable/first-frame path, overlaps the
  reconnect-enable-fails item).

Do NOT deep-RE blued until the capture points below L2CAP — it may prove to be lower-layer intermittency we fix by
re-arming scan, not a terminal concern. See `bt-decisions.md` §4/§5.

### First live capture of a failed login-screen boot — maps to ladder step 1/2, but CONTAMINATED (2026-07-16)

User rebooted on BT; **no cursor at the login screen even after clicking**, then plugged USB. `tools/re bt-timeline`
+ `klog` captured both the failed boot and, for contrast, an earlier clean BT boot the same day:

| | clean boot 16:46 (self-healed) | failed boot 21:48 |
|---|---|---|
| BT controller up (`[HCI]`) | 16:46:13 | 21:48:05 |
| `MT2Gesture: nub up` | 16:46:15 | 21:48:08 |
| **`[blud]*MT2*` link key saved** (= BT connect completes) | **16:46:20 (+7s)** | **21:48:15 (+10s)** |
| `AppleUSBMultitouchDriver` present? | **no** — pure BT self-reconnect | **yes, at 21:48:15** (the USB plug) |

**Reading against the ladder.** During the actual login-screen dead window (21:48:08→:14) there is **no `[blud]*MT2*`,
no channel nub, no `[OURS]` stream** — matches the top rung: *device didn't page / host wasn't page-scanning* (ladder
step 1/2), i.e. **not our reader**. Corroborated by `bluetoothaudiod: Failed to create connection to the daemon:
connection timeout` at 21:48:14 → BT userland was sluggish on that boot. Ruled out on this boot: **not**
enable-fails (no `_enableMultitouch`/`0xD7`/`0xe00002bc`/basic-HID `0x02` anywhere), **not** kext-unloaded
(`AppleBluetoothMultitouch 80.14` + `IOBluetoothHIDDriver` both loaded). `conn-trace` empty (kernel syslog carries
no per-device HCI ACL request/complete — that layer needs blued HCI logging to see).

**Why this capture is NOT yet decisive — the repro is contaminated.** The MT2's BT link key + connection completed at
**21:48:15, the exact second the USB plug enumerated** (plugging USB cannot make blued save a *BT* link key, so BT
genuinely connected then too — both transports arrived in the same second, USB won per the single-transport rule and
drove the cursor). So we cannot separate the two surviving hypotheses:
- **(a) slow-but-fine reconnect** — BT reliably connects at login in ~7–10s (clean boot did at +7s; this one shows +10s)
  and the user's patience simply ran out at ~10s, cabling USB right as BT arrived. Would be a *latency*/UX problem, not
  a connect failure.
- **(b) genuinely stuck until stimulus** — BT would not have connected on its own, and the USB plug (device
  re-advertise / physical wake / host re-scan) is what unstuck it.

**Decisive next repro (clean, uncontaminated):** reboot on BT; at the login screen **wait ≥60s WITHOUT plugging USB and
WITHOUT tapping**, watching whether the cursor self-connects (the clean 16:46 boot proves it can, at +7s). If it comes
alive on its own → (a), measure the delay, fix = keepalive/scan re-arm to shrink it. If it never does until a
tap/other stimulus → (b), fix = recover a host- or device-initiated reconnect trigger. Only cable USB as the escape
hatch *after* the observation window closes. To also capture the missing ACL/page layer, enable blued HCI logging
before the boot so the next `conn-trace` shows the connection request/complete for `04-4b-ed-ec-02-07`.

### CONFIRMED empirically 2026-07-16 (fair test) — owned-BT does NOT reconnect at login; hypothesis (b)

**The two earlier captures were both contaminated; this one was clean and the user is a reliable witness.** User
rebooted on BT, started a 60s timer **when loginwindow appeared** (so it was 60s of real BT airtime, not boot time),
did NOT plug USB and did NOT tap — cursor stayed dead to motion AND to clicks for the full 60s. Plugging USB then
worked instantly. **Verdict: owned/synthetic BT does not self-reconnect at the login screen given a fair window →
hypothesis (b) "genuinely stuck," NOT (a) "slow-but-fine."** (My contaminated-capture theories — "flaky cable at
boot," "firmware ate the 60s" — were wrong; the earlier same-second USB/BT coincidences were an artifact of me
plugging near the natural connect time, not evidence. Do not resurrect them.)

### CODE-GROUNDED regression candidate — we EVICT Apple's IOBluetoothHIDDriver (overturns the RE conclusion above)

Pointing skepticism at the recently-changed code (owned-BT pivot `53b3f6d` "direct L2CAP listener" + `89cad00`
"delete genuine-BNB machinery"; the latter's own commit message: *"residual risk: on-device BT untested until
Task 5"*) surfaced a concrete behavioral difference the earlier RE missed. **`kext-gesture/Info.plist.in`
`MT2BTControl`/`MT2BTInterrupt` (PSM 17/19) match with `IOProbeScore=100000` and NO `IOMatchCategory` — the
comment states the intent explicitly: "we compete in the DEFAULT category against the generic IOBluetoothHIDDriver
(probe score 0) and win … EXCLUDING it. That hands us the device so our interrupt listener is the sole reader."**
So the owned pivot does not merely *replace* BNB at the actuation layer (genuine did that and left Apple's HID
stack running) — it **evicts Apple's `IOBluetoothHIDDriver` from the device entirely.** That HID driver carries the
bonded-HID wake/idle machinery (`handleWake`, GET/SET_IDLE, `decrementOutstandingIO`→`commandWakeup`); the
host-initiated reconnect primitive is `IOBluetoothFamily::BluetoothHCICreateConnection`. **This directly overturns
the prior claim in this file that "owned-vs-genuine at the consumer layer does NOT change reconnection acceptance"**
— empirically it does, and the eviction is the most likely reason.

**Still-open edge (needs the HCI capture or a coexistence experiment to close):** whether login reconnect is
host-initiated (blued/kext pages the device) or device-initiated (click pages the host), and therefore whether
IOBluetoothHIDDriver's presence is *causal* or merely *correlated*. Don't over-claim past this — I already
over-swung once from doubting the user into a confident code theory; this candidate is strong but not yet proven.

**Minimal test = a coexistence experiment (crash-risk kext change → needs go/no-go + the genuine-teardown-panic
tradeoff, see `bt-decisions.md` §4).** Stop excluding Apple's `IOBluetoothHIDDriver`: drop our L2CAP probe score
below Apple's / add an `IOMatchCategory` so both consume, or let Apple's HID stack win the channel and reconnect
the device while we keep only the actuation-layer personalities. If login reconnect returns → candidate confirmed.
⚠️ Re-admitting Apple's genuine BT HID stack risks re-introducing the BNB teardown panic the pivot was meant to
escape — design the experiment so Apple *starts* BNB (normal flow), we never *manual-start* it (the panic was our
manual-start racing teardown). This is a genuine architecture fork (owned-reconnect-fix vs the genuine-teardown
panic); surface it to the user before loading anything.

### ROOT CAUSE NARROWED 2026-07-17 (on-device) — owned-BT never implements the HID-host role

A long live-debugging session (device + dtrace + kernel log) reframes the whole item. **The reconnect ladder
above is NOT where it dies — the ladder assumes the failure is page-scan / allowed-incoming, and it isn't.**
What was actually observed, in order:

1. **The connection does not persist even during PAIRING.** Fresh re-pair (`06:04:33`): both L2CAP channels
   open, BOTH reader personalities bind (PSM 17 + 19), the fabricated AMD registers and the OS configures it
   (`SET-REPORT 0xdc/0xdd/0xc8`, `ENABLE-MT enable=1`) — then `connection closed` **within ~1 second**, clean
   teardown, and it stays down (channel count 0). So this is not "never forms a link"; the link forms and
   **collapses in ~1s.**
2. **On reconnect (touch/click the paired-but-disconnected trackpad), the host sees NOTHING.** A clean
   connect-path dtrace (`AcceptConnectionRequest` / `RejectConnectionRequest` / `CreateConnection` /
   `IsAllowedIncomingL2CAPChannelForDevice` / `WriteScanEnable`) caught **zero events** while the user clicked
   repeatedly. No page reaches the controller — consistent with the device not being held in a reconnectable
   HID relationship (device state), not with a reject. `system_profiler`: Paired=Yes, Configured=Yes,
   **Host Connectable=Yes, Connected=No** (device VID 0x4C/PID 0x265, fw 0x0318 — device is fine).
3. **Confirmed code gap (`MT2BTReader.cpp`):** our reader does a bare `listenAt` + `0xF1` and **nothing else** —
   `grep` confirms it NEVER calls `waitForChannelState(OPEN)` and implements none of Apple's HID-host bring-up.
   Apple's evicted `IOBluetoothHIDDriver` does the full job in `prepInterruptChannelWL`/`processCommandWL`:
   drive the interrupt channel to OPEN (`waitForChannelState`), set HID protocol, set idle rate, probe reports,
   VirtualCable, and **hold/maintain** the connection. We evict it (probe 100000 vs 0) and replaced all of that
   with two lines.

**Conclusion: owned-BT never implemented the HID-host connect/maintain role — it is a passive channel consumer
of a link Apple's driver used to establish AND hold.** That is why it "worked before" (Apple did it in every
era: genuine-BNB, and the earlier `f24766e` owned build was only ever *live-verified in-session*, never across
a real idle→reconnect). The "spot we missed in the restore" is not a lost line — it is the entire HID-host role.

**NOT yet proven:** the exact byte-level reason for the ~1s drop (device-initiated disconnect vs host close vs
missing-handshake timeout). Live disconnect capture failed operationally this session (traced disconnects while
no connection existed; then process-management churn). The reliable trigger is a **re-pair** (produces a
connect→drop every time) — arm BOTH connect+disconnect tracers cleanly, re-pair, read the reason. That one datum
tells us WHICH handshake step, when absent, drops the link → what to implement.

**Also ruled out this session:** the immediate-`0xF1`-enable "restore `f24766e`" change (built, loaded, tested) —
it fired *after* teardown on an already-inactive channel and did nothing; reverted. The enable timing is not it.

**Fix fork (see `bt-decisions.md §4`):** (1) reimplement the HID-host role inside owned-BT (mission-aligned;
needs the disconnect-reason datum first), or (2) stop evicting `IOBluetoothHIDDriver` and let Apple connect/hold
while we observe/actuate (fast, but reverts toward genuine-BT and re-opens the teardown-panic decision).

### RESOLVED (Layer B) 2026-07-17 — waitForChannelState(OPEN) handshake wired in + on-device proven (`4d5350a`)

The connect-then-drop is FIXED. `MT2BTReader` now does the genuine acceptance (`reference.md` "BT connect
handshake"): control `listenAt(controlData)` + `waitForChannelState(OPEN)`, interrupt `waitForChannelState(OPEN)`
before arming. On-device a re-pair reaches `STEADY`+`MT_MODE_CONFIRMED`, both channels stay up, gestures stream,
battery interpose finally installs. Device-initiated **click-reconnect now works and persists**. This was the
RE'd-but-never-implemented fix (`how-to.md` "fix the connect flap"); the user's "we've had synthetic BT before,
missed a spot" was exactly right — the spot was the HID-host acceptance handshake.

### STILL OPEN (Layer A) 2026-07-17 — host-initiated boot reconnect + list-persistence = the evicted VIRTUAL-CABLE role

The handshake did NOT fix the login-screen/cold-boot reconnect. On-device reboot: the MT2's L2CAP channels
**never open at boot**, our reader **never runs** (`grep` of the boot log: no `setup on PSM`), so the handshake
can't engage — while a *different* paired device reconnected normally. **User's key observation (unifying clue):**
after reboot the MT2 **vanished from the Bluetooth device list** even though it stayed paired
(`com.apple.Bluetooth.plist` PairedDevices still lists `04-4b-…` "Mavericks Trackpad 2"), and it **reappeared only
after a USB attach/detach**. A genuine Magic Mouse stays listed while disconnected; ours used to.

**Understanding (RE'd):** list-persistence-while-disconnected AND host-initiated boot-reconnect are the **same
missing role** — the **HID Virtual Cable** that Apple's `IOBluetoothHIDDriver` maintains for a bonded HID device
(it only sends `VirtualCableUnplug` — RE'd in its `processCommandWL` — to *break* the cable on real removal).
Structurally: **our readers are children of the transient `IOBluetoothL2CAPChannel` nubs; nothing of ours holds
the `IOBluetoothDevice` at the device level.** So when the channels drop, the device object is not held → it
leaves the list and nothing re-pages it at boot. The USB attach transiently recreates the device object → it
reappears. We evicted the one thing (Apple's HID driver, probe 100000 vs 0) that held the device as
virtually-cabled → we lost list-persistence AND boot-reconnect together.

**Fix direction for Layer A (next):** hold/register the MT2 at the DEVICE level as a reconnectable
virtually-cabled HID device — candidates: (a) a minimal owned personality matching `IOBluetoothDevice` (not the
channel) that just holds it + registers reconnect, coexisting with our channel readers (mission-aligned, owned);
or (b) re-admit Apple's `IOBluetoothHIDDriver` for device-level management only (re-opens the genuine/teardown-panic
tension, `bt-decisions.md §4`). Confirm the exact registration first (what keeps a Magic Mouse listed+reconnectable
vs the MT2) before building — likely one live capture of a genuine device's boot reconnect vs ours.

### DECISIVE 2026-07-17 — Layer A is DEVICE-SIDE, not host-side (login HCI capture)

A boot LaunchDaemon ran an HCI connect-path DTrace (`tools/bt_login_capture.d`: `WriteScanEnable`,
`Accept/RejectConnectionRequest`, `CreateConnection`, `CreateDeviceFromConnectionResults`,
`IsAllowedIncomingL2CAPChannelForDevice`) live across the login window while the user clicked the trackpad.
**Entire capture:** `WriteScanEnable = 0x02` (page-scan ON) at boot +4s — and then **nothing** for 240s.
No incoming connection, no accept, no reject. **The host is page-scanning (ready to hear a reconnect); the
MT2 simply never pages it on a login-screen click.** Corroborated by the earlier daemon result: a host
`openConnection` also got HCI `0x04` **Page Timeout** (device didn't answer the host's page either).

**Conclusion: the login-screen no-reconnect is DEVICE-SIDE — after a host reboot the MT2 is in a deep sleep
it neither pages out of (on a click) nor answers a host page from.** In-session it's a lighter sleep, so the
handshake-fixed click-reconnect works. This **overturns the whole host-side program above** (eviction /
allowed-incoming / page-scan / virtual-cable-hold / an owned `IOBluetoothDevice` personality / a wake daemon):
none can make a silent device transmit. `#3` and `#(a)/(b)` as Layer-A fixes are **ruled out** for this
symptom — the host is already doing its half.

**BUT it is NOT a device limitation — genuine-BT reconnects on click at login (observed; user certain,
battery 100%).** So the device CAN page at login; owned just doesn't put it in the state to. The capture
localizes *where* (device silent), not *why-unfixable*. The difference is host-side and we haven't done it.

**Leading candidate — owned's HID handshake is INCOMPLETE.** `reference.md` "BT connect handshake" step 5 =
`deviceReady`: **`SET_PROTOCOL` (`0x70 | bit`, subclass bit-inversion for 05AC:0309) + `SET_IDLE`** after both
channels OPEN — the full HID device setup Apple's `IOBluetoothHIDDriver` performs. Owned-BT never wired it in
(we send only `0xF1` + now `waitForChannelState`). The hypothesis: the full genuine HID setup is what leaves
the device "properly HID-connected" and thus reconnect-ready across a reboot; a `0xF1`-only session streams but
does not arm the device's reconnect. This is the same RE'd-but-unimplemented gap the `waitForChannelState` fix
just closed for the in-session flap. **Next: implement step 5 (SET_PROTOCOL + SET_IDLE), reboot, test.** If the
device still won't page at login after the full handshake, the remaining difference is captured by temporarily
restoring genuine-BT and diffing its connect HID exchange vs ours (mind the genuine teardown panic).
Battery ruled out (100%). Shutdown-disconnect cleanliness is a secondary candidate.

### RESOLVED root cause + NEW direction 2026-07-17/18 — reconnect is blued device-management; wear a known identity

Exhaustive on-device session. **Ruled out (all tested):** (1) deviceReady handshake — `SET_PROTOCOL(0x71)`
+`SET_IDLE(0)` on control (RE'd from `IOBluetoothHIDDriver::setProtocol`@0x5cd2 / `setIdle`@0x5dbc; the
`0x5AC:0x309` bit-inversion does NOT apply to our 0x4C:0x265 MT2). Fires clean in-session (multitouch confirmed
in 1 enable vs old 6–24), but did NOT change cold-boot: device still silent at login. Per-connection commands
don't persist to arm the next boot. (2) host-side wake daemon — `openConnection` → HCI `0x04` Page Timeout. (3)
letting Apple's HID driver take the channel by lowering our probe to -1000 — **our specific VID/PID+PSM match
still wins**; Apple has **NO HID personality for VID 76/PID 613** (only VID/PID-specific ones + a generic that
loses to us). This is the exact reason the project manual-started BNB originally.

**ROOT (definitive):** login reconnect is **blued's HID-device management** — it reconnects devices it recognizes
(`hidDeviceWeAreConnectingTo`, `setConnectableAdvertising:`, `addToFavorites`, `HIDNormallyConnectable`, keyed on
the device having an Apple personality / SDP HID reconnect attrs `HIDReconnectInitiate`/`HIDNormallyConnectable`;
`RecantConnection` (feature report 0x41) is the *dis*connect signal, not reconnect). **Owned-BT's direct-L2CAP
grab bypasses blued's HID flow**, so the MT2 is never kept reconnectable. LIVING PROOF on this host: the paired
`34-15` **Magic Mouse reconnects at every boot** (Apple has its personality) while our MT2 stays silent — same
host, same blued, same page-scan-on. Not a device limitation; a management gap.

**NEW DIRECTION (user, mission-aligned — the layer, not a driver):** don't reimplement blued's reconnect — make
the device **wear an identity the system already reconnects** (a Magic Trackpad 1: VID 1452 / PID 782, which
Apple's `BNBTrackpadDevice` matches). Same pattern we already use one layer up (`MT2HIDShell` wears the MT1
identity at `IOHIDInterface` for cursor actuation) — push it DOWN to the BT identity layer. If the MT2 presents
as an MT1 at the BT level, Apple's `BNBTrackpadDevice` matches it **naturally** → login reconnect + native
multitouch + HUD/pane identity for free, AND **no manual-start teardown panic** (natural match rides Apple's own
clean lifecycle; the panic was OUR manual-start racing teardown). This could dissolve the owned-vs-genuine
tension entirely. **Next:** find where the BT device identity (VID/PID) is read for Apple's `BNBTrackpadDevice`
L2CAP match + blued's management (DeviceID SDP / EIR / `IOBluetoothDevice` node), and whether the layer can
present MT1 there without disturbing the real L2CAP link. Check `device-identity-map.md` first (may already RE
the identity-read path).

### ★ MECHANISM FOUND 2026-07-18 — login reconnect = controller HID-Emulation (UHE), gated on device CLASS

RE of **blued** pinned it (device-free, provable on this host). Login reconnect is **not** an OS/blued
software page — it's the **Broadcom controller's HID Emulation Mode (UHE)**: blued writes a recognized HID
device's link key **into the controller hardware** (`addDeviceToHIDEmulationMode:` → `BroadcomHostController
addHIDEmulationDevice:classOfDevice:linkKey:` → `BluetoothHCIWriteStoredLinkKey`), and the **controller
autonomously re-pages + reconnects that device at boot/reset, before the OS is up** (`DisableHIDAutoConnectOnReset`
gates it). **It is Mouse + Keyboard ONLY** — the written-in-hardware paths are `HIDEmulationMouseWasWrittenInHardware`
/ `HIDEmulationKeyboardWasWrittenInHardware`; there is **no Trackpad-in-hardware**. Gate log:
`"addDeviceToHIDEmulationMode … not Apple-supported hardware"` / `"unrecognized HID device; NOT storing the link
keys to the module."`

**PROOF on-host:** `com.apple.Bluetooth` has `HIDEmulationMouse = "34-15-9e-cd-0e-2c"` (Magic Mouse — key in
controller HW → reconnects every boot) and **NO** `HIDEmulationTrackpad` for our MT2 `04-4b`. The MT2 doesn't
reconnect **because it is a trackpad** (CoD `0x594`); the controller only HW-reconnects mice/keyboards. Same
host/blued — the ONLY difference is device class. (Explains the DTrace: host page-scan on, device silent — the
*controller* would page it if its key were stored, but it isn't.)

**THE FIX (the "wear a known identity" idea, made precise):** register the MT2 into HID-Emulation as a
**Mouse-class** device so the controller stores its key + autonomously reconnects it. The two classification
systems are **independent** (this file's "two classification systems … do NOT share a signal"; the multitouch
layer keys on `parser-type`/geometry and NEVER consults CoD) — so the device can be a **mouse for
HID-emulation/reconnect** AND a **trackpad for gestures/pane** at once. We ALREADY RE'd the API
(`device-identity-map.md` comb#2): `BroadcomHostController addHIDEmulationDevice:classOfDevice:linkKey:` is a
**real Broadcom impl** (HCI vendor cmd) + `BluetoothHCIWriteStoredLinkKey:inDeviceAddress:inLinkKey:`. Call
`addHIDEmulationDevice` for the MT2 with a **mouse CoD + its link key**, decoupled from its real trackpad CoD.
**NEXT:** trace the exact call — `addHIDEmulationDevice` args/signature, where to get the stored link key
(`ReadStoredLinkKey`), and the call point (our reader's connect, or a userland helper on connect notification).
Same shape for USB later. NOT `RecantConnection` (disconnect). Wear a MOUSE, not an MT1, for the reconnect layer.

**2026-07-18 — plist shortcut is DEAD; the lever is blued's CLASS classification.** Tested editing the nested
`DaemonControllersConfigurationKey → <controller-addr> → HIDEmulationMouse` (and `…Keyboard`) to our MT2:
**blued RE-DERIVES every UHE slot from the actually-paired mice/keyboards on each respawn/boot and drops
anything else** — our edit was reverted to the Magic Mouse (mouse slot) and removed entirely (keyboard slot).
So we cannot just park the MT2 in a slot, and a direct `addHIDEmulationDevice` call would likewise be evicted
on the next re-derivation. **The only durable path: make blued CLASSIFY the MT2 as a mouse/keyboard so its OWN
machinery arms the controller** — i.e. the MT2 must present a mouse (or keyboard) CoD where blued reads it.
Tradeoffs: gestures unaffected (multitouch layer is CoD-independent); the Bluetooth/Trackpad PANE keys on CoD
minor `0x25` so it'd show as mouse (papered over by the osax swizzle we already run); slot choice = keyboard
(empty, keeps Magic Mouse) vs mouse (displaces the unused Magic Mouse). **OPEN — RE next:** (1) the exact
`"not Apple-supported hardware"` gate criteria — is UHE-eligibility keyed on **CoD** (wear a mouse CoD) or on a
specific **Apple mouse/keyboard MODEL VID/PID** (wear that model)? (2) WHERE blued reads that classifying
identity — the device's live advertisement (overwrites the paired-store cache on every connect) vs the cached
paired-store record — and whether the layer can present it durably. The single-slot + re-derivation constraints
are the "weird constraint" the whole approach must satisfy.

**2026-07-18 — gate is CoD-CLASS-based, NOT model-based (good).** The UHE eligibility gate in blued (the fn
holding `"not Apple-supported hardware"` @vmaddr `0x1000327xx`) LOGS `"major class: … minor class: …"` right at
the decision and branches on the **major/minor device class**, not on a specific VID/PID model. CoD math:
Magic Mouse CoD `0x2580` = major 5 / **minor 0x20 (mouse)** — eligible; our MT2 CoD `0x594` = major 5 /
**minor 0x25 (trackpad)** — rejected. So the lever is confirmed simple: **present a mouse-class CoD (minor 0x20,
e.g. `0x2540`/`0x2580`)** and blued classifies the MT2 as a mouse → puts it in `HIDEmulationMouse` → arms the
controller. No specific Apple model needed. REMAINING: (1) WHERE to present the mouse CoD durably so blued's
re-derivation reads it — the paired-store `ClassOfDevice` cache (settable) vs the device's live advertisement
(re-writes on connect); mind that the mouse slot is single + Magic-Mouse-occupied (so consider the empty
keyboard-class CoD instead, minor 0x40, if the controller HW-reconnects keyboards — it does), and (2) the pane
tradeoff (mouse/keyboard CoD → pane shows non-trackpad; osax swizzle papers it; gestures unaffected). This is
the concrete build target: seed/interpose a mouse-or-keyboard CoD where blued's UHE re-derivation reads it.

**2026-07-18 — build obstacle pinned: classification is at PAIRING off the LIVE CoD, not cache-editable.** Tested
setting `DeviceCache:04-4b…:ClassOfDevice` to mouse (9600) and keyboard (9536) with the MT2 disconnected, then
bouncing blued: **blued did NOT re-classify the MT2 into any UHE slot** — it keeps the stored `HIDEmulationMouse`
(Magic Mouse) and does not iterate DeviceCache to re-derive. Also: while CONNECTED, blued overwrites the cached CoD
with the device's live `0x594` on every connect. So a cache CoD edit is a dead end — **`addDeviceToHIDEmulationMode`
(the gate + slot add) runs at PAIRING/connect off the device's LIVE-advertised CoD**, not at respawn off the cache.
**THE BUILD = interpose the CoD blued sees at classification time:** (a) rewrite the remote CoD in the HCI
connection-complete/inquiry path so the MT2 presents as keyboard(0x2540)/mouse(0x2580) to blued (mission-aligned
layer; `IOBluetoothFamily::CreateDeviceFromConnectionResults` reads the remote CoD — same fn family already RE'd in
`tools/bt_login_capture.d`), or (b) DYLD-interpose blued's CoD read for our address, or (c) re-pair while (a)/(b)
is active. Slot: keyboard (empty, keeps Magic Mouse). Verified device-free tooling: `/tmp/mt2_disc` (closeConnection),
PlistBuddy on `DeviceCache`. **NEXT SESSION STARTS HERE:** find the HCI connection-complete CoD seam + whether our
kext / a filter can rewrite it so blued classifies the MT2 as a keyboard once, arming the controller.

**2026-07-18 — ★ CORRECTION from disassembling `-[BluetoothHIDManager addDeviceToHIDEmulationMode:]` (blued
`0x1000322bf`): the "class gate rejects minor 0x25" claim above is WRONG. The gate does NOT reject the trackpad.**
Actual branch structure (device is the arg, `deviceClassMajor`/`deviceClassMinor` read off the `IOBluetoothDevice`):
- `deviceClassMajor != 5` → return (MT2 major 5 PASSES).
- `deviceClassMinor == 0x20` → **`HIDEmulationMouse`** slot.
- else `isPointingDevice && (minor & 0xf) == 5` → **`HIDEmulationTrackpad`** slot ← **MT2 (minor 0x25) LANDS HERE — recognized, NOT rejected.**
- else `minor == 0x10` → **`HIDEmulationKeyboard`** slot.
- else → NSLog `"unrecognized HID device; NOT storing the link keys"` → return.
The `"…not Apple-supported hardware"` reject (`0x32811`) is gated on **`r12` = is the HOST CONTROLLER an Apple-supported
Broadcom** (a USB-product-ID bitmap test on the local controller), **NOT** on the device's class. On our genuine Apple
Broadcom host that passes. So blued IS willing to register the MT2 — as a **Trackpad** — and calls
`-[IOBluetoothHostController(BroadcomHostController) addHIDEmulationDevice:classOfDevice:linkKey:]` with the device's real
CoD, provided a link key is present (`cmpb $1` on the fetched key at `0x32701`; else logs `"no link key"`).

**WHERE it's called from:** a `[LinkKeyNotification]` handler in `EventNotifications.m` (blued `~0xa190`) — i.e. **at
PAIRING**, when a link key is created for a device that passes `isConfiguredHIDDevice:`. Not at every connect; at the
pair. Path: `writeLinkKeyToHardwareForDevice:` then `addDeviceToHIDEmulationMode:`.

**SO WHAT ACTUALLY BLOCKS TRACKPAD LOGIN-RECONNECT (structural, disasm-confirmed):** blued persists both
`HIDEmulationMouse` **and** `HIDEmulationMouseWasWrittenInHardware` (+ the same pair for Keyboard) — the
`…WasWrittenInHardware` flag tracks the controller's autonomous boot-reconnect HW slot. For Trackpad there is only
`HIDEmulationTrackpad` — **NO `TrackpadWasWrittenInHardware` string exists in blued.** So a trackpad link key can be
stored in the config plist but is **never written into the controller's autonomous-reconnect hardware slot**; the
controller (which HW-re-pages stored devices at reset before the OS is up) only tracks Mouse + Keyboard. THIS — not a
class gate — is why the MT2 doesn't reconnect. The KB's overall direction ("wear a mouse/keyboard identity for the
reconnect layer") is therefore RIGHT; only the stated reason was wrong.

**REVISED SEAM (the real target):** make blued's `addDeviceToHIDEmulationMode:` take the **KEYBOARD** branch for the MT2
(`deviceClassMinor == 0x10`, major already 5) **at pairing time**, so the key lands in the empty `HIDEmulationKeyboard`
slot + its `…WasWrittenInHardware` HW path. Keyboard slot is EMPTY on the live host (`defaults read
com.apple.Bluetooth DaemonControllersConfigurationKey` → only `HIDEmulationMouse = 34-15-9e-cd-0e-2c` Magic Mouse; no
keyboard/trackpad slot, and the MT2 has NO slot at all today). Choose keyboard (empty) over mouse (displaces Magic
Mouse). Seam value = `-[IOBluetoothDevice deviceClassMinor]` (and `classOfDevice`, the arg passed to `addHIDEmulationDevice`)
as READ INSIDE BLUED at pairing. Cleanest surgical option: **DYLD-interpose in blued** — swizzle
`deviceClassMinor`/`deviceClassMajor`/`classOfDevice` to keyboard-class **only for our MT2 address**, then re-pair once.
(A kext-level CoD rewrite in IOBluetoothFamily has a much larger blast radius — pane, `isPointingDevice`, everything —
so blued-local interposition is preferred.)

**BEFORE ANY CODE — the cheap on-device oracle (U1):** the MT2 has no UHE slot at all today, so we don't yet know the
MT2 pairing even REACHES `addDeviceToHIDEmulationMode` (maybe `isConfiguredHIDDevice:` is false, or synthetic/manual-start
BT bypasses the normal LinkKeyNotification). blued's branch NSLogs to ASL (`"major class: %d minor class: %d"`,
`"unrecognized HID device…"`, `"…no link key"`, `"addHIDEmulationDevice, error = 0x%04X"`), or enable
`/var/log/blued.log` via `IOBluetoothFileLogHelper`. **Next step: watch the log while doing a FRESH MT2 BT pair and read
which branch fires** — that tells us whether the seam is "flip trackpad→keyboard slot" or "the call never happens for us."
Only then write the interposer.

**2026-07-18 — ★★ ON-DEVICE ORACLE RUN (blued file log) — the real barrier is `isConfiguredHIDDevice`, UPSTREAM of the
class branch. `addDeviceToHIDEmulationMode` NEVER RUNS for the MT2. Supersedes BOTH the class-gate claim AND the
"flip to keyboard slot" seam above.** Method: `sudo touch /var/log/blued.log` + `killall blued` (writability is
cached per-path per-process, so a restart is required to re-enable file logging), then a FRESH Remove+re-pair of the
MT2. The log is decisive:
- Pairing works end-to-end: `[LinkKeyNotification] New link key <04-4b-ed-ec-02-07>` saved to `kDaemonPrefsLinkKeys`,
  `deviceClassMinor = 37` (0x25 trackpad), `isPaired = 1`. So link-key creation is NOT the problem.
- BUT `BluetoothHIDManager.m:816 newlyConnectedHIDDevice - 04-4b… isconfigured: 0` → `:821 device IS NOT a configured
  device; saving it.` The MT2 is **not a "configured HID device."**
- The `LinkKeyNotification → addDeviceToHIDEmulationMode` caller (blued `~0xa190`) gates on `isConfiguredHIDDevice:`
  (`testb %al; je skip`). isConfigured=0 ⇒ **the whole UHE registration is skipped**; `addDeviceToHIDEmulationMode`,
  every `HIDEmulation*` line, and the mouse/trackpad/keyboard slot branch NEVER execute (grep of the post-pair log
  confirms zero occurrences). Durable proof: the config plist still has ONLY `HIDEmulationMouse = 34-15-9e-cd-0e-2c`
  (Magic Mouse); the MT2 gets no slot of ANY class. **So the earlier disasm branch analysis (mouse vs trackpad vs
  keyboard slot) is downstream of a gate we never reach — "wear a keyboard CoD" does NOT help by itself.**

**What "configured" means + WHY the MT2 fails it (mechanism, disasm + live-ioreg confirmed):** `isConfiguredHIDDevice:`
(blued `0x100030457`) is a pure `containsObject:` membership test against the `configuredDevices` set. The set's WRITER
is `configureHIDDevice:` (`0x100004efc`) — a **Distributed-Objects server method** a *client* calls into blued
(unwraps `isProxy`/`isKindOfClass`, then `newlyConnectedHIDDevice:` to register / `removeDevice:` to unregister). The
normal caller is the standard BT-HID driver stack (`IOBluetoothHIDDriver`) when a HID device attaches. **Our MT2
manual-starts `BNBTrackpadDevice` and has NO such driver instance** — LIVE ioreg (MT2 shows `Connected: Yes`):
`IOBluetoothHIDDriver`, `AppleBluetoothHIDDriver`, `BNBTrackpadDevice`, `AppleBluetoothMultitouch` all **count=0**. So
nobody calls `configureHIDDevice:` for the MT2 → it never joins `configuredDevices` → UHE is skipped. **The BT-reconnect
barrier is a direct consequence of our manual-start architecture.**

**Barrier 2 still stands even if we fix "configured":** the 2026-07-15 note below records that when
`IOBluetoothHIDDriver` WAS loaded and the plist DID hold `HIDEmulationTrackpad` for the MT2, it STILL didn't reconnect
at the login screen — because a Trackpad slot has no `…WasWrittenInHardware` HW-reconnect path (only Mouse/Keyboard do).
So login-reconnect fundamentally requires the MT2's link key to land in the **Mouse or Keyboard hardware slot**, not the
trackpad slot. Two barriers stacked: (1) not configured → no UHE at all; (2) trackpad slot ≠ HW-reconnect slot.

**REVISED SEAM (both barriers at once): bypass blued — have OUR LAYER call
`-[BroadcomHostController addHIDEmulationDevice:classOfDevice:linkKey:]` directly** with a **keyboard/mouse CoD** + the
MT2's link key (readable: it's in `kDaemonPrefsLinkKeys`/`GetLinkKeyForAddress`, e.g. `6eaf4760 29cfaf91 48fc231c
702bb145`), skipping BOTH the `configureHIDDevice:` gate AND blued's class-based slot selection. This writes the
controller's autonomous-reconnect HW slot ourselves (`BluetoothHCIWriteStoredLinkKey` under the hood). OPEN before
building: (a) does blued's UHE re-derivation on respawn EVICT a slot we wrote for a non-configured device? (the HW
`WriteStoredLinkKey` may survive independently of the plist slot — verify); (b) confirm `addHIDEmulationDevice` is
reachable from our layer (it's an `IOBluetoothHostController` category impl on `BroadcomHostController`, in-process to
blued/IOBluetooth — may need a small helper linking IOBluetooth, or a DYLD interpose in blued that ADDS the call). The
alternative (make blued configure us by faking the `configureHIDDevice:` DO call as a mouse/keyboard) also solves both
but reintroduces blued's re-derivation risk. Verified device-free oracle now in place: `sudo touch /var/log/blued.log`
+ `killall blued` gives a full branch trace of any future attempt.

**2026-07-19 — ★★ addHIDEmulationDevice REACHABILITY CONFIRMED from our own (non-blued) process. Spike:
`tools/spikes/uhe_reach_probe.m`.** `[IOBluetoothHostController defaultController]` returns the concrete
`BroadcomHostController` (real impl, not the `0xE00002C7` base stub) in ANY IOBluetooth-linked root process; it
`respondsToSelector:` both `addHIDEmulationDevice:classOfDevice:linkKey:` and `removeHIDEmulationDevice:`. Calling it
returns `0x0000` and — traced from process launch with `dtrace -c` + `pid$target:IOBluetooth:*BluetoothHCISendRawCommand*`
— actually SENDS the Broadcom vendor HCI command. Live BT connection undisturbed; precedent that a client process may
drive controller HCI = `tools/mt2_bt_bounce` (openConnection).

**EXACT SIGNATURE (all three args BY VALUE — disasm of the IMP @ IOBluetooth `0x10002`, matches blued's call site
`0x32724`):** `-(unsigned short)addHIDEmulationDevice:(BluetoothDeviceAddress /*6 bytes, rdx*/)addr
classOfDevice:(unsigned int /*3 bytes used, ecx*/)cod linkKey:(BluetoothKey /*16 bytes, r8:r9*/)key`. Passing a
`device*` or a key `void*` (pointers) sends GARBAGE — the IMP `bcopy`s the register bytes inline. The IMP builds a
28-byte raw command and calls `_BluetoothHCISendRawCommand` (userland framework fn — a KEXT fbt probe on
`WriteStoredLinkKey`/`SendRawCommand` does NOT see it; use `pid$target:IOBluetooth:*`). Confirmed-correct 28-byte wire
payload (tracemem of the command buffer): `37 fc | 19 | <6 addr LE> | <16 linkkey LE> | <3 CoD LE>` — e.g. for the MT2
+ keyboard CoD 0x2540 + key 6eaf4760…: `37fc19 0702eced4b04 45b12b701c23fc4891afcf296047af6e 402500`. opcode 0xFC37 =
the Broadcom "add HID-emulation device" vendor command. `BluetoothDeviceAddress`={u8 data[6]}, `BluetoothKey`={u8 data[16]},
both from IOBluetooth headers. Link key is readable from blued's file log at pair (`[LinkKeyNotification] New link key`)
but NOT from the world-readable plist (root-only daemon store `kDaemonPrefsLinkKeys`).

**STILL OPEN (next milestones, beyond reachability):** (1) does the controller RETAIN the entry + actually autonomously
LOGIN-reconnect the MT2 after reboot? (the acid test — not yet run); (2) durability vs blued's respawn re-derivation
(blued reads the HW HID-emul table via `readHIDEmulationDevices` on startup and may evict a slot for a device it doesn't
consider configured — untested; restarting blued would test it but risks evicting our entry before a reboot test); (3)
where this call lives in OUR layer (kext vs a small IOBluetooth-linked helper daemon) + on what trigger (pair/connect
notification), for both BT and later USB; (4) CLEANUP: the spike's early runs sent 2 garbage-address + 1 garbage-key
0xFC37 commands to the controller before the signature was pinned — may have created junk HID-emul entries; a BT reset
(or `removeHIDEmulationDevice:` per bad addr, not reproducible) clears them.

**2026-07-19 — cleanup + clean re-arm DONE; the write PERSISTS and blued ADOPTS it (durability = positive).** On the
next `killall blued`, blued's startup does `EventNotifications.m:1183 readHIDEmulationDevice: 04-4b-ed-ec-02-07` →
`BluetoothHIDManager.m:893 addDeviceFromController - adding: 04-4b-ed-ec-02-07` → `link key found … 6e af 47 60`. So the
`0xFC37` write is retained in controller NVRAM and blued **adopts** our entry (does NOT evict it — resolves open
durability Q#2 favorably). The feared garbage-address junk never materialized: blued read back ONLY the MT2 from the HW
table (the malformed early writes left no persistent entry). `removeHIDEmulationDevice:` = vendor opcode **`0xFC39`**
(9-byte cmd: `39 fc 06 <6 addr LE>`); `addHIDEmulationDevice:` = **`0xFC37`**. Did a clean remove→re-add of the MT2
(keyboard CoD 0x2540 + correct key), verified by blued read-back (link keys present for both Magic Mouse + MT2). NOTE:
the plist `DaemonControllersConfigurationKey` still shows ONLY `HIDEmulationMouse` (no keyboard slot for the MT2) — blued
adopts the controller entry + link key but does NOT write a plist UHE slot (that's the separate `addDeviceToHIDEmulationMode`/
`isConfiguredHIDDevice` path). Whether the controller-level entry alone drives autonomous **login reconnect** — or whether
the `…WasWrittenInHardware` plist bookkeeping is also required — is exactly what the **reboot acid test** (still pending)
will decide. Current state: MT2 cleanly armed as keyboard-class in the controller, connected, ready for that test.

**2026-07-19 — ★★ REBOOT ACID TEST: FAILED ("zilch" — no MT2 reconnect at the login screen). The runtime `0xFC37`
write does NOT survive a cold boot; the warm-restart "adopt" was misleading.** Boot log (pid 45, cold boot 08:37):
`BluetoothHIDManager.m:721 hciControllerOnline; disableHIDAutoConnectOnReset? 0` (auto-connect-on-reset IS enabled),
then blued re-pages the **Magic Mouse** from its `HIDEmulationMouse` plist slot (`link key found … 34-15-9e-cd-0e-2c` →
`Create connection failed (0x4)` — mouse was off) — but **ZERO** MT2 HID-emul activity: no `readHIDEmulationDevice:
04-4b`, no connection attempt. Post-boot a warm `killall blued` no longer reads back the MT2 entry either. **Conclusion:
our directly-injected controller entry is volatile — a full power cycle wipes the controller's HID-emul table, and only
entries backed by a blued PLIST `HIDEmulation` slot get re-armed at boot (blued re-writes its plist slots into the
controller when `hciControllerOnline`).** The MT2 has no plist slot (writing one needs blued's `addDeviceToHIDEmulationMode`
→ gated by `isConfiguredHIDDevice`, unreachable via manual-start). So the whole `addHIDEmulationDevice`-from-our-layer
seam, while reachable and correct at runtime, is a DEAD END for login reconnect on its own: it never persists the plist
slot that survives boot. (Earlier "plist shortcut is DEAD — blued re-derives/drops a hand-edited slot" compounds this.)

**REDIRECT — two live directions:** (A) get the MT2 a DURABLE plist `HIDEmulationKeyboard` slot — either make it
`isConfiguredHIDDevice` (fake the `configureHIDDevice:` DO call as mouse/keyboard so blued's own machinery writes +
re-derives the slot), or write the plist slot AND defeat the re-derivation; both fight blued. (B) **[LIKELY SIMPLER,
mission-aligned] our OWN boot-time reconnect** — a `RunAtLoad` root LaunchDaemon that, once `blued` is up at the login
screen, does `openConnection` to the MT2 using its stored link key (the exact primitive `tools/mt2_bt_bounce` /
`mt2_usb_bt_handoff` already use). This sidesteps UHE/blued's plist machinery entirely: host pages the device at boot
instead of relying on the controller's autonomous store. OPEN for (B): does a login-screen LaunchDaemon `openConnection`
actually bring the link up pre-login, and — the separate sub-question — does our manual-start kext then DRIVE the cursor
at the login screen (the link forming ≠ multitouch working). Next session: prototype (B) as a boot LaunchDaemon +
re-test at login.

**2026-07-19 — ★★ CURSOR-DEAD-AT-LOGIN root cause is DOWNSTREAM of the BT bind (NOT reconnect, NOT half-open).**
Login-screen sampler (`/usr/local/sbin/mt2_login_probe`, one-shot boot LaunchDaemon `com.schmonz.mt2loginprobe`):
`BT` reader count reaches **2 (both L2CAP channels) 13 s after boot and HOLDS for the full 90 s** at the login screen —
yet the cursor is dead to move/tap/**click** the entire time (user-confirmed "90s, no response to anything"). So the
connection-keeper daemon + L2CAP bind are fine; the dead cursor is a separate, downstream layer. Rules out slow-bind,
half-open (BT=1), and channel-level login-gating. LOGGED-IN WORKING baseline trace (`sysctl debug.mt2_log=2`): each touch
= `MT2: BT edge n=1 x.. y.. ts..` (0x31 report received on PSM 19) → `MT2: feed x.. y.. -> amd 0xffffff80a2d41f00` (fed
to the genuine AppleMultitouchDevice) → `post_button_edge mask=0x1` on click. **NEXT (reboot, probe now retries
`debug.mt2_log=2` until the kext loads):** touch at the login screen and read `system.log` for these signatures to pin
the break — (a) no `BT edge` = device not sending (enable didn't take); (b) `BT edge` but no `feed -> amd` = reader's AMD
target null pre-session; (c) `BT edge`+`feed -> amd <ptr>` but no cursor = AMD→WindowServer delivery is session-gated.
Note: first probe run's `debug.mt2_log=1` failed ("invalid") because the kext wasn't loaded yet at 10:25:28 — hence the
retry-until-loaded fix. Genuine trackpads DO work at the 10.9 login screen, so a faithful AMD-feed should too.

**2026-07-19 — ★★★ ROOT CAUSE + FIX DIRECTION CONFIRMED ON-DEVICE. The MT2 IS usable as a BASIC-HID cursor at the
login screen — via Apple's own `IOBluetoothHIDDriver`, which OUR reader evicts.** Two on-device tests settled it:
(1) A diagnostic kext that only SKIPPED `SET_PROTOCOL` did NOT rescue basic HID — the device still sent no `0x02`, only
`0x90` then `0x31` after 18 enables (~17s). That was a FLAWED test: our reader was still loaded, still evicting Apple's
HID driver, still driving the connection. (2) The RIGHT test: `kextunload com.schmonz.MT2Gesture` + bounce →
**`IOBluetoothHIDDriver` attaches (count 1) and the user confirms a working basic cursor + click (no gestures)**.
So the dead-cursor-at-login is NOT a device limitation and NOT the slow `0xF1` enable per se — it is that our owned
reader (`MT2BTControl`/`MT2BTInterrupt`, IOProbeScore=100000, no match category) **evicts Apple's `IOBluetoothHIDDriver`
entirely** (open-questions.md:941), and Apple's driver is exactly what gives basic HID (cursor) at the login screen.
Our reader replaces it with a multitouch-only path whose enable is slow/unreliable at cold boot → mute pad until session.

**FIX DIRECTION (A), user-endorsed:** let Apple's `IOBluetoothHIDDriver` own the MT2 pre-session (basic cursor at the
login screen — "necessary and sufficient there"), and load OUR reader only at USER-SESSION start to take over for full
multitouch. Confirmed cheap + clean: reloading our kext + bounce is a WARM takeover that evicts Apple's HID and reaches
`BT=2` in ~5s (no cold-boot 17-39s enable delay — that penalty is cold-boot-only). Likely BONUS: Apple's
`IOBluetoothHIDDriver` "carries the bonded-HID wake/idle machinery" (open-questions.md:941), so letting it drive at the
login screen may make the MT2 **reconnect natively** there too — potentially retiring the login-screen half of the
connection-keeper daemon (which still earns its keep for USB→BT handoff + in-session reconnect). OPEN for the design:
(a) trigger to load our kext at session start + unload at logout (LaunchAgent + logout hook vs relocating mt2d-run's
boot load); (b) transition hiccup (bounce) on login/logout; (c) verify Apple's HID actually reconnects the MT2 at the
login screen; (d) does relocating the load reopen the ownership/panic reasons we went owned. NEXT: design A.

**2026-07-19 — (B) primitive VALIDATED post-login (chosen direction; mission fit = "a layer that can drive ANY device",
not a mouse/keyboard-only UHE hack).** With the MT2 connected, `tools/mt2_bt_bounce 04-4b-ed-ec-02-07` (a non-blued root
process): `closeConnection -> 0x0`, `openConnection -> 0x0 (CoD 0x594)`, MT2 back to `Connected: Yes`, our reader
`com_schmonz_MT2BTReader` (×4) re-bound, cursor drives. So a userland `openConnection` re-establishes the link on demand
— the whole of (B). (`BNBTrackpadDevice` shows count 0 in `ioreg -c` even while working — our manual-start doesn't leave
a persistently-enumerated instance under that class name; not a regression, just a probe caveat.) **REMAINING RISK,
reboot-only:** (1) does a `RunAtLoad` LaunchDaemon's `openConnection` succeed PRE-login (blued is up early — pid 45 — so
likely, but loginwindow/BT-stack readiness is untested); (2) does our boot kext + reader DRIVE the cursor at the login
screen once the link is up. **BUILD PLAN for (B):** a small root LaunchDaemon — reuse the `openConnection` primitive
(generalized by our device-match predicate `mt2_cod_is_mt2`, not a hardcoded addr, to stay device-agnostic), gated on
`hciControllerOnline` (poll for `[IOBluetoothHostController defaultController].powerState`/paired-device availability)
with bounded retry/backoff, `RunAtLoad`+`KeepAlive`-off. Install to `/Library/LaunchDaemons`, reboot, watch at login.
Keep it in the same family as `tools/mt2_usb_bt_handoff` (already a RunAtLoad IOBluetooth daemon).

**2026-07-19 — ★★ (B) BUILT + REBOOT-TESTED: the reconnect layer WORKS pre-login; the remaining gap is
cursor-drive-at-login (a kext/reader issue, NOT reconnect).** Implemented as a resident connection-keeper in
`tools/mt2_usb_bt_handoff.m` (idempotent `reconnect_matched()` actuator on a serial queue + a 15s `dispatch_source`
timer for boot/login wake + periodic-while-disconnected + the existing USB-removal edge; `mt2_reconnect_policy.h`
predicate + `tests/test_reconnect_policy.c`; `--reconnect-once`/`--disconnect-once` test hooks). Installed + on-device
validated. **Reboot boot log (decisive):** boot 09:24:37 → daemon armed 09:24:40 (pid 108, early/pre-login) →
`[timer] openConnection 04-4b… -> 0x00000000` at 09:24:50 (~13s in) → blued `newlyConnectedHIDDevice 04-4b…
isconfigured:1` at the same instant. So **pre-login openConnection SUCCEEDS and the BT link comes up at the login
screen** (resolves open unknown #1 = YES). **BUT user report: "zilch at the login screen; cursor worked as soon as I
logged in."** So the link is up pre-login yet the multitouch cursor does not drive until the user session starts —
open unknown #2 = the kext/reader does not deliver multitouch at the login screen (same shape as prior "connected but
cursor dead" notes; possibly the device-initiated PSM 19 interrupt channel / multitouch-enable not established on a bare
`openConnection`, or event delivery gated on the Aqua/loginwindow session). **NEXT THREAD (separate from this daemon):
why the cursor is dead at the login screen while the link is up — investigate our reader's bind + multitouch-enable +
event delivery pre-login vs at session start.** The broader-name rename of the daemon stays deferred until the full
login-screen experience (cursor too) works. Daemon commits: `9f1a178`→`493e048` on main.

## BT trackpad never forms a link at the login screen — link-layer, upstream of synthetic-BT (2026-07-15)

**Observed:** after reboot, clicking the BT trackpad at the login screen did nothing; user logged in via
keyboard, then plugged USB. Log (`19:21:36`→`19:24`): BT **host controller** up (`19:21:37`), trackpad
**validly paired** (`com.apple.Bluetooth.plist`: `04-4b-ed-ec-02-07` "Mavericks Trackpad 2",
`HIDEmulationTrackpad`), all Apple BT kexts loaded (`IOBluetoothHIDDriver`, `AppleBluetoothMultitouch`) — yet
**zero** HCI connection / `BNB` / control-channel-attach for the trackpad (`conn-trace` empty). The click never
reached the host as a connection. Because the current BT path is synthetic (`89cad00` deleted genuine-BNB;
`53b3f6d` = direct L2CAP listener drives the fabricated AMD), and the connect SM (`mt2_connect_sm.h`) only
advances on `CSM_EV_CONTROL_ATTACH`, **none of the synthetic-BT machinery ran** — the failure is one layer
below it (device didn't page, or host page-scan). We don't gate Apple's baseband accept, so a page would have
logged. This was the FIRST on-device exercise of restored synthetic-BT and it learned nothing about synthetic-BT
itself. **NEEDS a live, logged reconnect test** (disrupts USB; mind the reconnect-panic/`enableMultitouch`-fails
history). Rule out device-side (asleep/battery/bonded elsewhere) vs host page-scan.

---

## CoD exact-match misses the MT2 (live CoD carries service bit) — CONFIRMED bug (2026-07-15); FIXED 2026-07-16

> **FIXED 2026-07-16.** All three sites now match via one shared predicate `mt2_cod_is_mt2()` in
> `tools/mt2_cod_match.h` — `(cod & 0x1FFF) == 0x594` (mask off the service-class bits, compare the
> device-class field). Guarded by `tests/test_cod_match.c` (asserts `0x594` and `0x2594` and any service
> bits all match; wrong minor / mouse / zero do not). Unblocks USB→BT handoff. On-device confirmation of the
> handoff still pending a live unplug test (the predicate itself is host-tested). Below is the original bug.

The MT2's stored `ClassOfDevice` is `9620` = **`0x2594`**: device-class `0x594` (major 5 / minor 0x25, correct)
**plus** the `0x2000` Limited-Discoverable service bit. Three tools compare with **exact equality** and so
silently miss the MT2 when the service bit is set: `tools/mt2_usb_bt_handoff.m:38,60` (this is why `19:23:49`
logged "USB removed but no paired CoD-0x594 MT2 found" → USB-unplug→BT handoff currently broken),
`tools/mt2_bt_bounce.m:34`, `tools/mt2_prefpane_refresh.c:1246`. **Fix:** mask — `(cod & 0x1FFF) == 0x594` (or
compare only the device-class bits) instead of `== 0x594`. Small, low-risk. Passed validation on 2026-07-04
because the runtime CoD didn't carry the discoverable bit then.

**2026-07-19 — ★ connect/disconnect BEZEL GLYPH trigger CONFIRMED on-device: it fires on APPLE `IOBluetoothHIDDriver`
attach/detach (ownership transitions), NOT on our reader's own connect/disconnect.** Two A/B triggers with the user
watching: Case 1 = owned-mode `--disconnect-once`+`--reconnect-once` (our reader stays, `IOBluetoothHIDDriver` = 0
instances) → NO glyph. Case 2 = `kextunload MT2Gesture`+bounce (Apple HID attaches) → CONNECT glyph; then reload+bounce
(Apple HID detaches, our reader re-owns) → DISCONNECT/"connection lost" glyph. Explains the long-standing "we can't
predict when it appears" (mt2-connect-disconnect-bezel-hud): in steady OWNED operation the poster
(`IOBluetoothHIDDriver::deviceConnectTimerFired` / `sendDeviceDisconnectNotifications`, device-identity-map.md §Bezel HUD)
is never attached, so the glyph never fires; it only shows when Apple's driver cycles (our load/unload dances). **A
IMPLICATION:** direction A (Apple HID at login ↔ our reader in-session) makes Apple-HID attach/detach routine, so a
connect glyph fires at login-screen-appear (MOUSE image — Apple's driver defaults `MouseConnected` for MT2 PID 613), a
disconnect glyph at login (our reader evicts Apple HID), and again on logout. UX wrinkle to design around (suppress, or
seed trackpad art onto Apple's `IOBluetoothHIDDriver` instance for our address — harder than the BNB-node seed since we
don't own that instance).

**2026-07-19 — ★ A IMPLEMENTED + on-device validated (login-screen basic HID via deferred takeover). Commits
43e35a5→585f60d.** `mt2d-run` gained a `/dev/console`-owner session guard; a per-user Aqua LaunchAgent touches
`/usr/local/var/mt2d/session.trigger` which the `mt2d` daemon WatchPaths → runs `mt2d-run` at login. On-device reboot:
`14:44:03` boot run logged "login screen (console=root): leaving the MT2 to Apple's generic HID" → **login screen shows
an INSTANT basic cursor, NO glyph** (user-confirmed) ✓. At login `14:44:14` the trigger fired mt2d-run → our reader
bound in ~1s (`MT2BTReader: setup on PSM=19`). So the login-screen goal is ACHIEVED and the takeover PLUMBING is fast.
**BUT multitouch didn't work until ~30s post-login** — our reader binds instantly but the `0xF1` enable is slow (device
coming from Apple's basic-HID mode is slow to switch to multitouch). This is the SAME uncracked cold-enable lag
(direction B), RELOCATED from boot to post-login — NOT a takeover-speed problem (no glyph on takeover either). Accepted
A as-is (login screen fixed); the enable-lag is now the active dig. Host test: `tests/test_mt2d_run.sh` (session-guard
cases). Design/plan in `docs/superpowers/` (transient).

**2026-07-19 — enable-lag dig (started). Ruled out the KB's leading hypothesis; genuine-stack RE started.** The
standing theory (reference.md:200) was "0xF1 fired before PSM 19 opens → device never opens PSM 19". CONTRADICTED by
the 14:44 A-takeover log: `MT2BTReader: setup on PSM=19` fired (interrupt channel open) BEFORE the 0xF1 retry loop, and
the device still ignored ~30s of 0xF1. So PSM-19 ordering is NOT it — the device silently ignores the 0xF1 SET_REPORT
for 17–39s on a cold connect even with both channels open. Next unanswered Q: does the device ACK/NAK our 0xF1 during
the dead window (transport) or accept-but-delay (device mode-switch)? — needs an early control-channel (PSM 17) capture.
Per user, started RE'ing the GENUINE stack we used to manual-start (IOAppleBluetoothHIDDriver::deviceReady @0x18d6 in
IOBluetoothHIDDriver.kext — BNBTrackpadDevice's superclass). First read: it arms a batteryLevel timer and carries timing
constants 0xDBBA00 (14,400,000) + 0x36EE80 (3,600,000) — the genuine connect/enable is TIMER-managed on ~14s/~3.6s
scales, i.e. Apple ALSO works around a slow device enable rather than having an instant path. Supports "enable-lag is
device-level; A already mitigates it (basic cursor at login while it settles)". NEXT SESSION: finish the genuine enable
RE (deviceReady @0x18d6 + setProtocol @0x1bfa + the setReport report id/sequence + those timer semantics) and compare to
our reEnableInGate; decide whether to mirror the genuine timing or accept the lag as device-level.

**2026-07-19 — ★ CORRECTION to the entry above: the enable-lag is NOT device-level — it's OUR 10.9 enable.** User
(ground truth): the MT2 enters multitouch mode INSTANTLY on newer macOS. So the device is fine given the right enable;
our 10.9 owned-stack enable is the bug. The ~14s/3.6s timers in 10.9's IOAppleBluetoothHIDDriver::deviceReady are a RED
HERRING — 10.9 predates native MT2 support, so its "genuine" BT-multitouch path was never a real MT2 driver. Do NOT
conclude "device-level / accept it". Corrected dig (next session):
1. **Reliable repro (no reboot):** the cold state must be reproduced by getting the MT2 into Apple-HID BASIC mode first
   (`kextunload MT2Gesture` + `mt2_usb_bt_handoff --bounce-once` → confirm `IOBluetoothHIDDriver`=1), THEN reload our
   kext + bounce + `debug.mt2_log=2` and time from load to the first `MT2: BT shim saw report id 0x31`. (First attempt
   was confounded: after unload, Apple HID needs a bounce to actually attach; and a stale "multitouch confirmed" from a
   prior boot matched the grep — use a fresh log marker.)
2. **Capture the device's RESPONSE to our 0xF1 SET_REPORT** during the dead window (does it ACK/NAK/ignore?) — install
   the control-channel (PSM 17) shim EARLY (before streaming) or sniff L2CAP. Distinguishes transport-not-landing vs
   device-accepts-but-delays.
3. **Compare to a known-correct enable:** Linux `hid-magicmouse` drives the MT2 (public enable sequence); and our USB
   path works locally — check whether USB multitouch is instant vs BT slow, then diff the two enable paths.

**2026-07-19 — enable-lag dig, sharpened: device ACKs the enable (SUCCESS) but delays streaming; prime suspect = OUR
re-enable spam.** Built a reliable no-reboot repro: `kextunload MT2Gesture` → `mt2_usb_bt_handoff --bounce-once`
(confirm `IOBluetoothHIDDriver`=1, Apple-HID basic) → reload our kext → bounce → touch → time to `multitouch confirmed
(first frame after N enables)`. Instrumented `controlData` (throwaway, reverted) to log control-channel replies:
**the device answers every one of our control writes (SET_PROTOCOL / SET_IDLE / 0xF1) with `CTRL-IN len=1 [00]` = HIDP
handshake HANDSHAKE_SUCCESSFUL.** So the enable is NOT ignored/NAKed — the device ACCEPTS it, then simply doesn't
stream 0x31 for ~N seconds even while touched (`N ≈ elapsed seconds`, since reEnable fires ~1/s). One repro run showed
a 51-min "gap" that was a RED HERRING (genuine no-touch — 0x31 came the instant a touch arrived, after 1944 idle
enables); a controlled run WITH the user touching immediately showed a real ~16s dead-to-touch window (first frame after
16 enables). **PRIME SUSPECT (next experiment): our `interposeTimerFired`/`reEnableInGate` re-sends 0xF1 every ~1s
"until first frame" — a retry built for the GENUINE-BNB era (reference.md:235: "BNB's interrupt-channel bring-up knocks
the device back to mouse mode after our initial enable"). We no longer use BNB, so nothing knocks it back — and the
constant re-enable may itself be RESETTING the device's just-started stream every second, causing the lag. TEST: send
0xF1 ONCE (or stop re-enabling after the first SUCCESS handshake) and time the first frame on the repro loop. If faster
→ the retry meant to help was the bug (matches "instant on newer macOS", which does a clean one-shot enable).
