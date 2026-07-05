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
- **`src/mt2_pane_sm.{h,c}`** — pure Trackpad-prefpane transport SM (states/events/actions + reconcile);
  the osax (`tools/mt2_prefpane_refresh/mt2_prefpane_refresh.c`) is a thin adapter that maps IOKit
  edges + a poll to events and performs render actions. Delivery (osax / SIMBL bundle / DYLD dylib) is
  *orthogonal* to the logic — three build targets, one payload. Tested in `tests/test_pane_sm.c`.
- **`mt2_should_inject`** in the launch watcher — pure inject-decision + thin AppKit adapter
  (`tests/test_pane_watch.m`).
- **`kext-gesture/gh_default_adapter.cpp`** — half-realized: 7 shared generic callbacks with the provider
  threaded through a seam. The config-engine in miniature (a fuller engine waits on a real 3rd device).

**Latent targets (apply the shape here next — ranked by payoff):**
1. **The interpose/splice as a declarative plan (highest leverage + stakes).** `MT2BTReader.cpp` manual-
   starts a genuine BNB, yields the interrupt delegate, and interposes our MT2→MT1 shim on the delegate-
   callback slot via `vtable_clone` + magic offsets in `src/mt2_stack.h`. `csm_teardown_steps()` already
   models teardown as data; the latent object is the symmetric **install** plan — `{slot, save-original,
   install-shim, restore}` as a table the adapter walks. Because this code can panic, a declarative plan
   is unit-testable off-device (assert save/restore pairing + ordering + offset provenance) — turning the
   scariest code into the most-checked. (= the `refactor-to-explainability` "magic interpose offsets".)
2. **Stream conditioning as a pure policy (highest mission value).** The RE'd gates are scattered magic
   constants: `MT1_FIRM_RADIUS` (`src/mt1_encode.c` density), `MT2_SETTLE_MS` (`src/mt2_session.c`
   settle-gate), geometry normalization (`src/mt2_geometry.c`, the edge-clamp fix), the downstream clamp
   band (`MT2BTReader.cpp:172`). Extract a pure **conditioning policy** struct + `condition(frame,policy)
   → frame`, pipeline as the thin adapter → each RE finding becomes a config dimension; a new device
   becomes a config file. Gives every gate an off-device oracle (`mt2-behavior-tests-required`).
3. **Transport presence as one object shared by kext *and* pane.** `mt2_pane_sm` models transport truth
   `{BT,USB present}` for the UI; the kext's single-transport arbitration *and* the queued USB→BT handoff
   model the same reality independently. A shared transport-presence SM (same shape) that the handoff
   adapter reuses avoids a second ad-hoc SM over the identical truth.

Smaller: `src/vhid_mt1.c`'s feature-report acks → a pure report-id→response table + thin HID adapter;
`src/mt2_usb_reframe.c`'s `mt2_usb_button_edge` is already a clean pure edge-detector (the shape done
small). Standing direction + running debt list: memory `mt2-refactor-to-explainability`.

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

## Retired synthetic approach (pre-2026-06-24)

Before the genuine paths became the default, the kext drove the MT2 a second way: **decode the
device ourselves and feed our own fabricated `AppleMultitouchDevice`.** That code was deleted
2026-06-25 once genuine-BNB (BT) + genuine-USB both shipped, but its hard-won RE is preserved here —
several of these findings are reusable for the "97% API" mission and were not obvious.

**The fabricated AMD nub (the strict-cast story).** We `allocClassWithName("AppleMultitouchDevice")`,
`init`'d it with **`IsFake=false`**, installed an enable stub + geometry handler, then `attach`+`start`
under our `com_schmonz_MT2Gesture` nub. `AppleMultitouchDevice::start` reads `getProperty("IsFake")`:
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
| Synthetic-USB (retired) | MT2 USB `0x02` → MT1 | our own `MT2Gesture` nub | `submitFrame` (no interpose; we own the nub) |
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
choice). **GATE** the write to `gPsm==PSM_BT` + no transport transition in flight, keeping the
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
