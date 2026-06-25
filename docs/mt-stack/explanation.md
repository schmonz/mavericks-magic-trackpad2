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
| Synthetic-USB (shipped) | MT2 USB `0x02` → MT1 | our own `MT2Gesture` nub | `submitFrame` (no interpose; we own the nub) |
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

### Picture — CoD-driven, no per-device hook, only fixable by in-process hook/patch
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
- There is **no per-device image override** like `displayName`, and the **CoD is re-fetched from the
  live device every connect** (a cache override of `ClassOfDevice` does NOT stick — proven: set
  `1428`→`9600`, restarted `blued`, it re-fetched and overwrote back to `1428`). So the picture cannot
  be fixed via the cache.

**To make the Bluetooth pane show the MT1 picture (or any custom file) for our device**, the resolution
happens in-process from the live CoD, so we must hook `IOBluetoothUI` in the processes that render BT
device rows — **System Preferences (Bluetooth pane), `BluetoothUIServer`, the Bluetooth menu extra** —
by either: (i) inserting a `vault[5][0x25]` entry after `initImageDictionaries`, or (ii) swizzling
`imageForDevice:`/`imageForMajorDeviceClass:` to return our image for our device. The entry/return is:
- **MT1 asset:** `{ BundlePath = "/System/Library/PreferencePanes/Trackpad.prefPane"; ResourceName = "TrackpadPicture.png"; }`
- **Arbitrary file:** `{ BundlePath = "<our dir>"; ResourceName = "<our file>"; }` or `{ ImageObject = <NSImage from our file>; }`

Delivery: `DYLD_INSERT_LIBRARIES` / a SIMBL-style plugin into those processes, OR a binary patch of the
on-disk `IOBluetoothUI` (feasible only because **10.9 has no SIP**). Tradeoffs (see `decisions.md`):
invasive (shared Apple UI), per-process or system-file, reverted by OS updates — cosmetic, unlike the
clean `displayName` name fix.
