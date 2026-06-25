# Decisions — big-ticket roads not taken

Why we *didn't* go certain ways, so we don't re-tread them. Two kinds:

- **Not a functioning choice** — a fact makes it impossible. Won't revisit.
- **Not a sufficiently desirable choice** — it works (or could), but lost on a constraint. The entry
  names the constraint, so it's clear what would reopen it.

For *open* unknowns (vs these resolved ones) see `open-questions.md`.

---

### Own / bare `IOHIDDevice` for the prefpane — *not functioning*
To light the prefpane we tried publishing our own `IOHIDDevice` of the matched class. Publishing an
**un-started** `IOHIDDevice` null-derefs in `_publishDeviceNotificationHandler` → kernel panic. You
can't `registerService()` a bare nub of that class. (Led to: manually start a *genuine*
`BNBTrackpadDevice` instead, which is a real started IOHIDDevice.)

### REPLACE — drop genuine BNB, own the device outright — *not desirable*
A clean reimplementation that owns the BT channels + input was viable, but the user pinned **stock
Apple prefpane = mandatory**, and the pane matches `BNBTrackpadDevice`. Dropping genuine BNB loses the
pane. **Reopening criterion:** if "stock pane mandatory" were relaxed (e.g. an own-pane workstream
shipped), REPLACE becomes viable again.

### VID/PID match path (route-2 matching) — *not functioning*
We hoped to get matched onto the device by advertising the MT2's IDs. The matcher reads the **real
DID from the controller-side store**, not from our personality, so we can't win matching that way.
Manual-start is the only route.

### Seam C — wire injection (intercept HIDP GET_REPORT on PSM 17) — *not functioning / not desirable*
Premise was that geometry crosses the L2CAP wire and we could fabricate the response. It doesn't —
the geometry query terminates at in-kernel transport stubs, never hitting the wire. Even if it could
work, the in-kernel vtable override is simpler.

### Single-slot geometry override (`0xcc8` only) — *not functioning as designed*
Overriding only `getMultitouchReport` (`0xcc8`) never fired: `_deviceGetReportWithLookUp` probes the
**LENGTH via `getMultitouchReportInfo` (`0xcd8`) FIRST**, which hit the stub and short-circuited
before the data fetch. Proven on-device (override live, `MATCH`, but never called). **Fix that
shipped:** override *both* `0xcd8` and `0xcc8`. Don't re-try single-slot.

### Late / passive geometry publish — *not functioning*
Installing a geometry handler *after* the AMD started, or late `setProperty` of the geometry keys,
had no effect: userspace `MultitouchSupport` caches geometry at **first attach**, and the AMD is born
(empty) before it's reachable at `transport+0x1b0`. The publish must be correct *before* `AMD::start`
— hence the vtable override installed before `bnb->start()`.

### `IOMatchCategory` to coexist with `IOBluetoothHIDDriver` — *not functioning*
Trying to coexist (instead of exclude) so PSM 19 would open reliably: it opened, but the HID driver
then grabbed the interrupt-channel `listenAt` delegate (single-owner) → our listener rejected → no
frames, and the HID driver still didn't drive a cursor. Worst of both; reverted.

### `waitQuiet` on manual-BNB teardown — *not a functioning choice* (removed 2026-06-23)
Added 2026-06-21 as a "synchronous teardown" to drain BNB before `release()` (hoping to fix the §S2.9
unload wedge / §S2.14 reconnect flap). On-device probe (`getBusyState` before/after, 8s bound):
the manual `BNBTrackpadDevice` is `busyState=1` *before* `terminate()` and **still `busy=1` after a
full 8001 ms** wait; its AMD child is `busy=0`. So the busy is BNB's own — its genuine connect
lifecycle never completes in our hybrid flow (`deviceReady` never reached; the 5s "Forcing MT restart"
watchdog cycles), so the start-time busy is never balanced and `waitQuiet` can **never** succeed; it
only stalled every disconnect for the full bound. Removed → plain `terminate()` + `release()`. Unload
safety rests on the in-gate delegate/vtable restores, not on quiescence (no panic; reconnects work).
**Reopening criterion:** if the full `waitForChannelState(OPEN)` handshake fix lands and BNB reaches
`deviceReady` genuinely, it should settle to `busy=0` — a real synchronous teardown becomes possible
then; re-check.

### Hybrid `+0x1b0` poke — *not desirable (stability)*
The hybrid architecture routed prefpane settings by poking BNB's handler slot (`+0x1b0`) to our
fDevice. It worked (controls applied) but its teardown drove genuine HID teardown against a
half-restored object → the `ultimate-hat.panic`. Motivated the move to full-BNB, which deletes the
poke (BNB's node *is* the input *is* the pane target). **Note:** the *hybrid architecture itself*
remains a live alternative for a livability comparison; only this *mechanism* is graveyarded.

### `OSBundleLibraries` to force-load the Apple driver kexts — *not functioning for BT → uniform script-load*
The genuine paths manual-start `BNBTrackpadDevice` (BT) and `AppleUSBMultitouchDriver` (USB) via
`allocClassWithName`, which needs the owning Apple kext (`AppleBluetoothMultitouch.kext` /
`AppleUSBMultitouch.kext`) loaded so the class is registered. On a clean boot they are NOT loaded in
time → `allocClassWithName` returns NULL → the genuine path is dead with **no fallback** (proven
2026-06-24: BT had no cursor / no BT prefpane bits until `kextload AppleBluetoothMultitouch.kext` +
reconnect). The IOKit-idiomatic fix is to declare those kexts in our `OSBundleLibraries` so `kextload`
pulls them in as dependencies — which is exactly how we already force-load
`com.apple.driver.AppleMultitouchDriver`. **It does NOT work for BT:** `AppleBluetoothMultitouch.kext`
exports **no `OSBundleCompatibleVersion`**, and a bundle with no compatible version cannot be an
`OSBundleLibraries` dependency (nothing to link/version-match against). `AppleUSBMultitouch.kext` *does*
export `OSBundleCompatibleVersion=1.0.0`, so USB *alone* could use it — but we deliberately **don't**,
to avoid splitting mechanisms (Info.plist dep for USB, script for BT). Instead BOTH are loaded
best-effort by `kextload` in the boot wrapper (`dist/mt2d-run`) and dev `make load`, before our kext.
Two reasons beyond uniformity: (1) **soft** (`|| true`) — our kext still loads and the other transport
still works if an Apple kext is absent, whereas a hard `OSBundleLibraries` dep would block our *entire*
kext (losing BOTH transports) on a missing/incompatible Apple dep; (2) one mechanism is simpler.
**Reopening criterion:** move USB to `OSBundleLibraries` only if a deploy path loads our kext outside
the wrapper/Makefile (it has the compatible version); BT can't until Apple's kext gains one.

### Run VoodooInput on 10.9 / become a VoodooInput plugin — *not functioning / wrong direction*
(Researched 2026-06-24, web.) The obvious "why not just use the Hackintosh kext?" question, answered
so we don't re-tread it. **First, the landscape:** there is **no public Apple API for the valuable
half** (device → gesture engine) on *any* macOS version. The public device path is the HID **digitizer**
(modern: HIDDriverKit `IOUserHIDEventService::dispatchDigitizerTouchEvent`; usage page `0x0D`,
Contact Identifier `0x51`), but third parties report stock digitizer descriptors yield *no cursor / no
gestures* and provider-binding is **built-in-only** gated — the same wall as our [[builtIn]] lens. The
gesture engine (`MultitouchSupport`) stays private everywhere; the only public gesture API is
app-level `NSGestureRecognizer`. So there's nothing of Apple's to be ABI-compatible *with*.

**VoodooInput** (acidanthera) is the community analog and was this project's original *reference* — but
it is the **inverse** of us: `VoodooInputSimulatorDevice` **fabricates a fake MT2** so IOKit *matching*
binds Apple's native MT HID driver to a virtual nub; clients (VoodooPS2/I2C) translate *non-Apple*
hardware into MT2 frames. It has **zero code to drive a real Magic Trackpad** — the "read a real MT2
over USB/BT" front-half is uniquely ours. Three blockers kill "just run it on 10.9": (1) it's a
modern-toolchain kext — loading on the 10.9 kernel is the exact unsolved build problem; (2) it assumes
clean IOKit **matching** to a modern stack, the very path we had to replace with manual-start + interpose
+ built-in-gating dodges; (3) it's **redundant for a real MT2** (already an MT2 — we need 10.9 to *wire
up* the real device, which VoodooInput doesn't do). **Reopening / convergence criterion:** the right
collaboration is inverted — be the **10.9 back-port that speaks VoodooInput's contact interface**
(`VoodooInputEvent{contact_count,timestamp,transducers[]}` + `VoodooInputTransducer{type,id,fingerType,
isValid,isPhysicalButtonDown,supportsPressure,maxPressure, Touch{x,y,pressure,width} current/previous}`,
IOKit msg `kIOMessageVoodooInputMessage`=12345), so one device targets both eras. Separately worth a look
(open question, not decided): VoodooInput's normal **publish-a-nub-and-let-IOKit-match** is cleaner than
our manual-start — does it work on 10.9? (Has teeth: catalogue residue, built-in gating, the un-started
`IOHIDDevice` panic above.)

### Override the cached `ClassOfDevice` to fix the Bluetooth-pane picture — *not functioning*
The pane's device picture is chosen from the device's Class-of-Device via `IOBluetoothDeviceImageVault`
(see explanation.md "Bluetooth prefpane device identity"). We hoped to override the MT2's CoD in
`blued`'s cache so it maps to the trackpad vault entry. **Doesn't stick:** set `DeviceCache[<addr>]
.ClassOfDevice` `1428`→`9600`, restarted `blued`; on the MT2's reconnect `blued` **re-fetched the CoD
from the live device and overwrote it back to `1428`** (`LastNameUpdate` advanced; file reverted). So
CoD is live-sourced every connect, and there is no per-device image override key (unlike `displayName`
for the name). **Reopening criterion:** none via the cache — the only ways to change the picture are
hooking/patching `IOBluetoothUI` in the BT-UI processes (System Prefs / `BluetoothUIServer` / menu
extra) or a binary patch of the framework (feasible only because 10.9 has no SIP). Cosmetic; deferred.
Contrast: the **name** has the `displayName` user-override key → clean, persistent fix (do that).
