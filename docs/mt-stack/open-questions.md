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
  transport vtable. `newTransportString` = `IOUSBHIDDriver`'s → **"USB"** → no `isBlocked` edge-clamp.
- **So "genuine for both" is NOT one parameterized strategy** — BT (two-object transport+AMD, vtable
  geometry, L2CAP-interpose feed) and USB (one-object HID driver, _deviceGetReport geometry, handleReport
  feed) are different implementations. It buys the pane on both + no USB edge-clamp, at the cost of a
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
  `mt2_usb_reframe.c` matches. Proof from `system.log`: of 5000+ `checksum is incorrect` lines, EVERY one is
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
