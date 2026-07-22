# Device-identity map — every signal the 10.9 HID stack reads about our device

RE'd 2026-07-04 by a parallel comb of 11 binaries (IOBluetooth, IOBluetoothUI, the
Bluetooth/Trackpad/Mouse/Keyboard prefpanes, BluetoothUIServer, MultitouchSupport, BezelServices, blued,
AppleMultitouchDriver.kext) via `tools/re`. This is the config-dimension inventory for the mission
([[mt2-mission-interface-over-driver]]): every place the OS decides "what kind of device is this / what can
it do", and how we set it. Supersedes the one-off icon/name/gesture notes by unifying them.

## The two classification systems (they do NOT share a signal)

1. **Bluetooth/HID identity layer** — IOBluetooth, IOBluetoothUI, the Bluetooth pane, BluetoothUIServer,
   blued. Keys on **Class-of-Device (CoD `0x594`)** (major/minor) + **`HIDDefaultBehavior`** string + IOKit
   **nub-class presence**. Decides: device *category* (trackpad/mouse/keyboard), pairing UX, the BT-list
   icon, status-menu routing, wake/encryption policy.

2. **Multitouch layer** — MultitouchSupport, AppleMultitouchDriver.kext, and the Trackpad pane's
   multitouch reads. Keys on **`parser-type`** + **`Family ID`** + **sensor geometry** + MT flags. Decides:
   MTDevice "Device Type" (Trackpad/Mouse/Simple), surface size, built-in-vs-external, gesture behavior.
   **It never consults CoD.**

The Trackpad pane straddles both: it uses the CoD **minor `0x25`** to recognize the BT device for battery,
and IOService/property matching for presence + gesture toggles.

## The levers (signal → who reads it → what it drives → how we set it → status)

| Signal | Read by (load-bearing site) | Drives | How to set | Status |
|---|---|---|---|---|
| **CoD `0x594`** (major 5 / minor `0x25`) | IOBluetooth `isPointingDevice`; Bluetooth-pane `deviceClassMinor` masks `(m&0x30)==0x20`→pointing, `(m&0xf)==5`→Trackpad; Trackpad-pane `_checkBatteryTimer` `==0x25`; blued `addDeviceToHIDEmulationMode`; BluetoothUIServer pairing branch; Keyboard-pane inquiry (excludes us, correctly) | device category, pairing UX, menu routing, which battery UI, wake/encryption | **CoD-derived-locked** — we present `0x594`; can't per-consumer swizzle | ✓ satisfied natively |
| **`HIDDefaultBehavior` == "Trackpad"** (string on HID node) | IOBluetooth `+[BluetoothHIDDevice isTrackpadDevice]` @0x3f8b3 | **THE trackpad gate**; true excludes us from `isMouseDevice` (which else fires on usage==2). NOT CoD-derived | seed-a-property `HIDDefaultBehavior="Trackpad"` | ✓ USB (`MT2USBReader.cpp:197`); **VERIFY on BT node** |
| **`PrimaryUsage`=2 / `PrimaryUsagePage`=1** | IOBluetooth `primaryUsage` → `isMouse`(2)/`isKeyboard`(6) | HID-usage classification (feeds the mouse/keyboard tests) | seed-a-property (Generic Desktop / Pointer) | verify seeded |
| **`parser-type` ∈ [1000,1999]** | MultitouchSupport `_createDeviceInfoForDevice` | MTDevice **"Device Type" = "Trackpad"** ([2000..]=Mouse, else Simple); + the Opaque flag | seed-a-property `parser-type=1000` | ✓ seeded (both paths) |
| **`Family ID` = 129** | AppleMultitouchDriver `decodeDeviceProperty`; MultitouchSupport | device-family / model discriminator | in the `rDEVICE_BASIC_INFO` seed | ✓ ([[mt2-genuine-geometry-groundtruth]]) |
| **Sensor geometry** (30×22, 15600×11040) | MultitouchSupport `_createDeviceInfoForDevice` | surface rect; **zero dims ⇒ device dropped entirely** | seed the basic-info report | ✓ (fixed the edge dead-zones) |
| **`MTHIDDevice`=1, `MT Built-In`=false, `TrackpadEmbedded` absent/false** | MultitouchSupport `_mt_CachePropertiesForDevice`; kext `attachToChild` propagates `TrackpadEmbedded` | MT-HID parse path; **external-vs-built-in** treatment | seed-a-property | ✓ MTHIDDevice; **verify `TrackpadEmbedded` NOT set** (must present external) |
| **`BatteryPercent`** (+ `UpdateBatteryLevel` write-trigger; + `ExtendedFeatures` gate on `AppleBluetoothHIDDevice`) | IOBluetooth `batteryPercent`; Bluetooth/Trackpad/Mouse panes; blued low-battery | battery readout + "Change Batteries" everywhere | seed `BatteryPercent` on the BNB node; the gate makes `withBluetoothDevice:` non-nil | ✓ (kext GET_REPORT 0x90) — but see the always-100% suspicion ([[mt2-battery-reporting-plan]]) |
| **`Product` / `Manufacturer`** (HID node strings) | IOBluetooth; product-name matching | display + name-based row matching | seed-a-property | ✓ (both paths) |
| **On-device name** = HID Feature report **`0x55`** (64-byte vendor report) / host **alias** (`setDisplayName:`) | IOBluetooth `Name` cache, pane Rename, menu title, blued `deviceName` | the visible + onboard name (follows the device across hosts) | **`SET_REPORT(Feature,0x55,name)`** then `remoteNameRequest:` refresh; clear alias with `setDisplayName:nil` | ✅ SOLVED + on-device proven 2026-07-05 (`setDeviceName:` was a no-op here — empty `ExtendedFeatures`) ([[mt2-device-writable-name]]) |
| **Trackpad\* capability props** (`TrackpadThreeFingerDrag`, `…MomentumScroll`, `…SecondaryClickCorners`, `…FourFingerGestures`, `…Editing`) | Trackpad pane `_isServiceAvailable:` = "does any IOService publish `<key>`=true" (IOPropertyMatch) | which gesture toggles the pane shows | seed the boolean on our matching IOService | ✓ three-finger-drag works both transports |
| **Nub class present** (`BNBTrackpadDevice`; mouse analog `BNBMouseDevice`) | Trackpad pane `loadMainView` IOService probe; Mouse pane `_iterateServices` | Trackpad-vs-NoTrackpad (and Mouse pane's TouchMouse route) | register the nub (genuine-reuse) | ✓ |
| **`isConfiguredHIDDevice`** | Bluetooth pane; blued | whether HID affordances (battery/prefs shortcut) appear | pairing-managed (address in `IOBluetoothPreferences hidDevices`) | ✓ via normal pairing |

## The icon problem, unified (pane row, menu, connect/disconnect HUD)

All device icons funnel through **IOBluetoothUI's CoD vault**:
`+[IOBluetoothDeviceImageVault imageForDevice:forMacTarget:]` → reads `deviceClassMajor/Minor` → the vault
dictionaries (keyed by CoD class numbers, with a literal `'none'` fallback). Our CoD `0x594` correctly
resolves to the **Trackpad** category. BUT the Apple-vs-generic ART is chosen by an **`isApple` boolean**
(r14 in `_IOBluetoothGetGenericTypeStringForDeviceClasses`): `kVaultTrackpadApplePeripheralKey` (Apple
trackpad art) vs `kVaultTrackpadPeripheralKey` (generic). So Apple trackpad art is **not reachable by
seeding a property** — this is the root of the "picture is a CoD/vault limitation" and why we swizzle the
pane row's `NSImageView` directly.

**Two new leads from the comb:**
- **`isApple` is THE root icon lever.** If we made the caller's `isApple` determination true (vendor/OUI-
  derived), *every* icon site — BT pane row, menu extra, and the connect/disconnect bezel HUD — would get
  Apple trackpad art at once. Worth RE'ing how `isApple` is computed (OUI/vendorID?).
- **`imageForModelString:`** (an alternate vault entry) resolves an icon via LaunchServices
  `_LSCreateDeviceTypeIdentifierWithModelCode` from a **product/model-code string** — a name/model identity
  path independent of CoD. A correct model-code could resolve Apple art without swizzling.

> ⚠️ **ARCHITECTURE-STALE (2026-07-22): the mouse-bezel mechanism + node-seed fix below are GENUINE-BNB-ERA.**
> In the current owned-BT + VoodooInput-satellite build, Apple's BT-HID posters are ALL excluded — live
> `ioclasscount` with the MT2 connected over BT: `IOBluetoothHIDDriver` / `IOAppleBluetoothHIDDriver` /
> `BNBTrackpadDevice` / `BNBMouseDevice` = **`<no such class>`** (our `Info.plist` out-bids + excludes the
> generic `IOBluetoothHIDDriver`; we never start a `BNBTrackpadDevice` — `MT2BTReader.cpp:9`). So
> `deviceConnectTimerFired` (the `"MouseConnected"` poster) does NOT run, and step 3's node-seed has no node to
> seed. Current bezel behavior UNCONFIRMED (hypothesis: **no connect OSD at all** — DriverServices keys on the
> driver classes we exclude, and our `MT2BTReader` class isn't registered with BezelServices; needs a visual
> connect/disconnect check on BT). The "shows a mouse" report predates the owned-BT pivot. RE-SCOPE before
> implementing: to get the genuine trackpad bezel we'd need a poster BezelServices recognizes (register a
> `BS_UI_Plugin` for our class, or emit the notification ourselves) — NOT the BNBTrackpadDevice node-seed.
>
> **MECHANISM RESOLVED 2026-07-22 (disasm of BezelServices) — the OSD trigger is ACTIVE, not passive.**
> `-[DriverServices installListenersForClasses:]` scans `/Library/Application Support/Apple/BezelServices/*.plugin`
> and per declared class arms `IOServiceNameMatching(className)` → on a matching node it `deviceArrived:` and
> registers an `IOServiceAddInterestNotification`/`IOGeneralInterest` watch on that node. The connect/disconnect
> OSD is dispatched ONLY when the owning driver posts a **private message `messageClients(0x62736B32 'bsk2', arg)`**
> on its node: `processMsgForService:` builds key `"<PrefixID>.<arg>"` → `_messageDict` (from the plugin's
> `Messages`, e.g. `Connected`/`Disconnected`) → `Action` → `_actionDict` → OSD. Node *appear* and
> `kIOMessageServiceIsTerminated` do NOT fire the OSD (terminate = teardown/battery-timer only). PROOF:
> `AppleTopCase` (built-in trackpad, class `AppleMultitouchTrackpadHIDEventDriver`) declares only
> `PreferenceDefaults`, **no `Messages`** — a passive trigger would flash it every boot. So the fix = **(A)** a
> `BS_UI_Plugin` keyed to `AppleMultitouchDevice` (the class our terminal fabricates + `registerService()`s;
> live `ioclasscount AppleMultitouchDevice = 1`) with `Connected`/`Disconnected` → trackpad-art actions
> (`Version=2`), shipped by our installer; **(B)** the **terminal** posts `messageClients(0x62736B32, arg)` on
> that node on connect (post-registerService, on a short timer to beat the interest-arm race — cf. Apple's
> `deviceConnectTimerFired`) and on disconnect (before teardown). Belongs in `MavericksAMDTerminal` (framework,
> reusable), NOT a satellite. REMAINING RE: the exact `'bsk2'` arg encoding for Connected/Disconnected, from
> `AppleBluetoothMultitouch.kext`'s `messageClients` call sites, so we replicate the proven contract.
> `IOServiceNameMatching` matches the node's registry name string exactly (no subclasses) → the plugin key must
> be exactly `AppleMultitouchDevice`.
>
> Genuine-BNB-era analysis retained below.

**Bezel HUD (connect/disconnect) — RE'd 2026-07-06 (mechanism found; NOT the CoD vault).** The earlier
"caller resolves via the CoD vault" guess is **WRONG**. The connect/disconnect OSD is drawn by the
**BezelServices login plugin** — `/System/Library/LoginPlugins/BezelServices.loginPlugin/Contents/MacOS/BezelServices`
(hosted per-session in **loginwindow**; it renders the actual bezel through `BezelUIServer` at
`…/BezelServices.loginPlugin/Contents/Resources/BezelUI/BezelUIServer`). Neither the `BezelServices.framework`
dylib (just `_BSDoGraphic*`) nor `BluetoothUIServer` is involved — `BluetoothUIServer` only posts
NSUserNotifications (`deliverNotification:`), and IOBluetoothUI's vault is used **only internally** (no
external caller references `IOBluetoothDeviceImageVault`). Flow: `+[DriverServices listenForClass:]` arms an
IOKit `IOServiceMatched`/connect/disconnect notification (`DeviceArrivalCallback`/`driverConnectedCallback`,
HIDMonitor) → `+[DriverServices processMsgForService:messageType:messageArgument:]` →
`+[DriverServices dispatchOSDAction:]`. **The OSD is 100% data-driven, NOT computed from CoD/`isApple`:**
`dispatchOSDAction:` pulls `Image`, `MessageKey`, `Priority`, `DurationMS`, `ResourceBundle` straight from a
matched **`BS_UI_Plugin` plist Action dict** (it even asserts `image != NULL` — there is *no* device-type→image
fallback in code). The event NAME comes from the **driver personality**: `AppleBluetoothHIDMouse.kext`'s
"Wireless Mouse 2004" personality (CFBundleIdentifier `com.apple.driver.IOBluetoothHIDDriver`) declares
`ConnectionNotificationType=MouseConnected` / `DisconnectionNotificationType=MouseDisconnected`. Our MT2, a BT
HID pointing device, matches that pointing-class personality → posts the **`MouseConnected`/`MouseDisconnected`**
messages → whatever `BS_UI_Plugin` Action handles those supplies the **mouse** `Image`. (The keyboard analogue
is fully visible: `AppleHIDKeyboard.kext/Contents/Resources/BS_UI_Plugin_IOAppleBluetoothHIDDriver-Info.plist`
maps `2007KeyboardConnected`→Action `2007KeyboardConnectOSD` `{Type=OSD, Image=BtEmbeddedKeyboard.pdf,
MessageKey=KeyboardConnectedText, Priority=600}`.)

**RESOLVED 2026-07-06 — who posts, why "mouse", and the exact scoped seam (all confirmed, incl. a live probe).**

1. **Who reads the type + posts the message.** `IOBluetoothHIDDriver` (`/System/Library/Extensions/IOBluetoothHIDDriver.kext/Contents/MacOS/IOBluetoothHIDDriver`) is the reader AND poster. `IOBluetoothHIDDriver::handleStart` does `getProperty("ConnectionNotificationType")` (str-xref at `+0x1c` of that fn; sibling reads of `DisconnectionNotificationType`/`…BatteryLow…`/`PoweredOffNotificationType` — all string literals live in this binary) and stashes it on the instance. `IOBluetoothHIDDriver::deviceConnectTimerFired` (fires off a timer after start) then builds the OSString to post: **if the stashed property is non-NULL → post it verbatim; else DEFAULT** — a `vtable+0xac8` predicate (is-pointing-device) true → hardcoded **`"MouseConnected"`**, else a `+0xac0` predicate → `"Connected"`, else nothing (`binary:IOBluetoothHIDDriver::deviceConnectTimerFired`, the `MouseConnected` literal is at file `0x957b`). `IOAppleBluetoothHIDDriver::sendDeviceConnectNotifications` (same binary) is a bare tail-jump into `IOBluetoothHIDDriver` vtable slot `0xb58` — it does NOT override the value logic. Our `BNBTrackpadDevice`/`BNBMouseDevice` (in `AppleBluetoothMultitouch.kext`) subclass `IOAppleBluetoothHIDDriver` (their `BNBDevice::handleStart` calls the inherited `sendDeviceConnectNotifications`), so they inherit exactly this posting path.

2. **Source of our node's value + why we get `MouseConnected`.** The value is a **personality Info.plist key** merged into the driver's property table at match time — NOT computed from CoD/`isApple`. Apple's `BNBTrackpadDriver` personality declares `ConnectionNotificationType="Connected"` / `DisconnectionNotificationType="Disconnected"` / `PoweredOffNotificationType="TrackpadOff"` (verified in `AppleBluetoothMultitouch.kext/Info.plist`; the mouse personalities in `AppleHIDMouse.kext/Contents/PlugIns/AppleBluetoothHIDMouse.kext` instead declare `MouseConnected`/`MouseDisconnected`, matching only VendorID 1452 + ProductID 780/777 — NOT our MT2 PID 613). **BUT: because our kext *manual-starts* `BNBTrackpadDevice` rather than letting IOKit match the personality, the personality dict is never merged onto our node.** LIVE PROBE (device connected as `BNBTrackpadDevice`, count=1; no `IOBluetoothHIDDriver`/`IOAppleBluetoothHIDDriver` instance): `re ioreg-props BNBTrackpadDevice` shows **zero** `ConnectionNotificationType`/`DisconnectionNotificationType`/`PoweredOffNotificationType`/`HIDDefaultBehavior` keys (all the plist-only keys are absent — the manual-start bypass proven). So `getProperty` returns NULL → `deviceConnectTimerFired` takes the DEFAULT branch → our device is-pointing → **`MouseConnected`** → mouse HUD. This is the whole cause: not a mismatched personality, but a *dropped* one.

3. **The scoped fix (BEST — no new plist, no swizzle): re-seed the 3 dropped keys on our own node.** The BezelServices plugins are keyed by **driver CLASS then event name**, and Apple ALREADY ships the trackpad mapping for our exact class: `…/AppleBluetoothMultitouch.plugin/Contents/Info.plist` has a top-level `BNBTrackpadDevice` dict whose `Messages` map `Connected→ConnectOSD`, `Disconnected→DisconnectOSD`, `TrackpadOff→TrackpadOffOSD`, each `Image="BtTrackpad.pdf"` (a TRACKPAD PDF; `MagicTrackpad.icns` also present in that bundle's Resources — both confirmed on disk). A sibling `BNBMouseDevice` dict maps the SAME generic `Connected`/`Disconnected` to `BtMouseRev.pdf` — so DriverServices disambiguates identical event names by the posting driver's class, and real Apple MT1 trackpads (also `BNBTrackpadDevice`) already resolve to the trackpad art via this path. Therefore the SEAM is: **our kext `setProperty`s `ConnectionNotificationType="Connected"`, `DisconnectionNotificationType="Disconnected"`, `PoweredOffNotificationType="TrackpadOff"` on our `BNBTrackpadDevice` node before `deviceConnectTimerFired` runs** (same class of node-property seed as the `HIDDefaultBehavior="Trackpad"` we already set; the connect message is on a post-start timer, so seeding during our start wins the race). Effect: our node posts `"Connected"` → DriverServices resolves `(BNBTrackpadDevice, Connected)` → `BtTrackpad.pdf` → **trackpad HUD**, and `TrackpadOff` fixes the power-off HUD too. Scoping is automatic and clean: only OUR node carries these props; a real Magic Mouse matches its own mouse personality/`BNBMouseDevice` and keeps `MouseConnected`/`BtMouseRev.pdf` untouched (honors [[mt2-dont-perturb-coconnected-apple-devices]]). This reuses Apple's exact config + art with ZERO new files. The earlier "ship a distinct `TrackpadConnected` event + a new `BS_UI_Plugin` plist" idea is now UNNECESSARY (Apple's shipped `Connected`/`BNBTrackpadDevice` mapping already carries trackpad art); a `DYLD_INSERT_LIBRARIES` swizzle of `+[DriverServices dispatchOSDAction:]` in **loginwindow** stays *rejected* (logout-loop risk). RESIDUAL (needs on-device validation of the actual product change): confirm DriverServices keys the trackpad `Connected` to our node's class at post time (strongly implied — real MT1 works this way) and that seeding during our start beats the connect timer; validate by seeding the 3 props, reconnecting BT, and observing the HUD art flip mouse→trackpad.

4. **`BS_UI_Plugin` scan directory (Thread 2 — PINNED).** The login plugin scans **`/Library/Application Support/Apple/BezelServices/*.plugin`** — `contentsOfDirectoryAtPath:@"/Library/Application Support/Apple/BezelServices"` then `pathsMatchingExtensions:@[@"plugin"]`, in the DriverServices plugin-load path (`binary:/System/Library/LoginPlugins/BezelServices.loginPlugin/Contents/MacOS/BezelServices`, string+selector xrefs near `0xfe80`; the disassembler mislabels the enclosing fn `_ESDisposeContext`). On-disk it holds `AppleBluetoothHIDKeyboard/…HIDMouse/…Multitouch/AppleHIDMouse/IOAppleBluetoothHIDDriver/IOBluetoothHIDDriver{Generic,Keyboard,Mouse}.plugin` — so the earlier "only the keyboard plist exists" residual is CLOSED: the mouse config is `AppleBluetoothHIDMouse.plugin` (`MouseConnected→MouseConnectOSD→BtMouse.pdf`) and, crucially, the trackpad config we need is already present in `AppleBluetoothMultitouch.plugin`. ([[mt2-connect-disconnect-bezel-hud]])

5. **ON-DEVICE 2026-07-06 — seed lands but the HUD does NOT fire (necessary ≠ sufficient).** Loaded the
   kext with the 3-prop seed (`feat(bt)…bezel HUD` `8ef5b37`); `re ioreg-props BNBTrackpadDevice` confirms
   `ConnectionNotificationType="Connected"`/`DisconnectionNotificationType="Disconnected"`/
   `PoweredOffNotificationType="TrackpadOff"` ARE on our node — so the seed persists, beat the connect
   timer, and `IOHIDDevice::start` did not clobber them. BUT a REAL trackpad power-off→on shows **NO bezel
   at all** — not even the old mouse one. So the connect/disconnect notification is **not being posted for
   our node**: manual-starting `BNBTrackpadDevice` bypasses the normal BT connection-complete flow that
   arms/fires `deviceConnectTimerFired`, so no OSD event is emitted (mouse or trackpad). The seed is correct
   + harmless (would render a trackpad IF the event ever fired), but making the HUD appear needs us to ALSO
   arm/post the connect notification from our start path — a bigger change. The earlier "2026-07-04 mouse
   HUD" sighting was likely a transient genuine-path moment we've since diverged from. **DEFERRED (cosmetic).**
   If revisited: dtrace `deviceConnectTimerFired`/the notification post to confirm the bypass, then arm/post
   it ourselves for the manual-started node (or find why the timer isn't armed under manual-start).

## Highest-value follow-ups (surfaced by the comb)

1. **Verify `HIDDefaultBehavior="Trackpad"` on the BT node** (it's the load-bearing trackpad gate; we seed
   it on USB, unconfirmed on BT). If missing, seeding it is the single biggest correctness lever.
2. **RE how `isApple` is determined** — the one lever that fixes ALL icon sites (pane/menu/HUD) at the root.
3. **Verify `TrackpadEmbedded` is absent/false** on our node (must present as external, not built-in).
4. `imageForModelString:` / model-code as a non-swizzle icon path.
5. Battery always-100% ([[mt2-battery-reporting-plan]]) — cross-check the `UpdateBatteryLevel` write-trigger
   + whether we return a real value or a constant.

Method + raw per-binary findings: the parallel `tools/re` comb `hid-device-identity-comb` (session workflow).

---

# Behavior & control surfaces (comb #2, 2026-07-04)

A second parallel comb (10 binaries + the `MultitouchHID.plugin` recognizer) over five NEW surfaces:
gesture, apple-genuineness, preferences, notifications, pairing/OOB. Method: `hid-multi-surface-comb`.

## Genuineness / `isApple` — RE'd, but NOT the icon lever (seed ABANDONED 2026-07-04)

> **CORRECTION 2026-07-04 (on-device read): the "seed → Apple art everywhere" claim below is FALSE.**
> Our `BNBTrackpadDevice` node already shows `VendorID=76`(0x4C)/`VendorIDSource=1`(BT-SIG) → `isAppleDevice`
> is **ALREADY TRUE** over BT — yet the connect/disconnect bezel HUD *still* rendered a MOUSE image. So the
> art is **NOT gated on `isApple`**; seeding `PnPVendorID`/`…Source` would change nothing (and it's SDP-DI-
> sourced, not a settable node property, so it isn't even seedable the way the comb assumed). Icon fixes stay
> **per-site**: the pane-row icon is fixed via the osax image swizzle; the menu-extra picture is a 10.9 CoD-
> vault limitation; the **bezel HUD lives in a separate process (BezelServices/BluetoothUIServer) with its own
> CoD→art lookup that must be RE'd on its own.** Do NOT re-propose the isApple seed. See
> [[mt2-device-identity-unlocks]] #1 for the full reassessment.

`-[IOBluetoothObject isAppleDevice]` (@0xcd80) = **`(PnPVendorIDSource==2/USB && PnPVendorID==0x05AC)` OR
`(source==1/BT-SIG && PnPVendorID==0x004C)`** — logic confirmed and still useful as RE, but per the correction
above it is already satisfied on our node and is not the art gate. (The icon has two paths: a pure-CoD
`imageForDevice:`→`imageForMajorDeviceClass:` with no isApple, plus the isApple-keyed
`_IOBluetoothGetGenericTypeStringForDeviceClasses` — so "one flag fixes all art" was an oversimplification.)
Sibling checks (`appleBluetoothMousePresent/HIDDevicePresent/
KeyboardPresent`) instead match the IOKit service class `IOAppleBluetoothHIDDriver` + HID usage.

## Gesture — the recognizer is a data-driven chord engine

- **Recognizer** = `MultitouchHID.plugin` (`MTTrackpadHIDManager`). `determineHIDManagerSettings()`
  (@0x1c390) reads two per-device dicts off our IOService node — **`TrackpadUserPreferences` +
  `MultitouchPreferences`** — and pulls one enable-key per gesture into a settings struct:
  `TrackpadThreeFingerDrag, TrackpadScroll, TrackpadHorizScroll, TrackpadPinch, TrackpadRotate,
  TrackpadRightClick, TrackpadCornerSecondaryClick, Trackpad{Two,Three,Four,Five}Finger…Gesture,
  TrackpadBasicMode, HIDScrollZoomModifierMask`. **Every gesture is enabled/blocked purely by its key** →
  seed-a-property on the node.
- **`MTTrackpadHIDManager::hwSupports3FDrag()` (@0x1dabc)** — the deeper three-finger-drag gate: a
  **hardware-capability bit test (`test $0x2, 0xb0(rdi)`) AND** the `TrackpadThreeFingerDrag` node property.
  This is the real gate behind the USB-parity item ([[mt2-three-finger-drag-usb-parity]]) — if USB ever lacks
  it, it's this cap bit, not the pref.
- **Gesture vocabulary** = a plist parsed by `PListGestureParser` (`Gesture Sets` / `Chord Mappings` /
  `Action Events` / `Motion Sensitivities`; categories Swipe / Polar Swipe / **Edge Swipe** / Pinch / Rotate /
  Scroll / ScrollPan / Drag / Tap / Zoom / Momentum / **Slide/BeginSlide**). This is the data-driven
  definition of which gestures exist; `Slide/BeginSlide` + `Edge Swipe` are where the edge-clamp behavior
  ([[mt2-cursor-edge-clamp]]) lives.
- **Pane side** (`Trackpad.prefPane`): the full gesture list is `MTTrackpadGesture` subclasses (Click,
  SecondaryClick, Lookup, Drag, Pinch, ScreenZoom, SmartZoom, Scroll, Dashboard, MissionControl, Launchpad,
  NotificationCenter, ShowDesktop, Rotate, Navigation, AppExpose) + `MTTGestureBackEnd`; each toggle
  read/writes `com.apple.trackpad.<name>Gesture` into BOTH driver domains and pushes to the kernel via
  `BSKernelPreferenceChanged`. 4-finger rows are gated by `fourFingerGesturesAvailable` ← `magicTrackpadFound`.
- Notification-Center edge gesture: `NotificationCenterGestureMode` / `AlwaysGenerateNotificationCenterGesture`
  / `TwoFingerNotificationCenter`, gated on the NC process being alive.

## Preferences — where every knob lives

- **Per-device (the config seam):** `TrackpadUserPreferences` + `MultitouchPreferences` dicts published on our
  IOService node — the recognizer reads these directly. This is how settings reach the gesture engine.
- **Per-transport domains:** `com.apple.AppleMultitouchTrackpad` (USB) vs
  `com.apple.driver.AppleBluetoothMultitouch.trackpad` (BT) — the pane writes toggles here; keep both in sync.
- **Global hidden knobs — `com.apple.MultitouchSupport`:** `ForceSimpleParser` (kill gestures),
  `ThumbZoneHeight`, `DispatchAllHIDEvents`, `NoPointing`, `ForceAutoOrientation`/`TrackpadOrientationMode`,
  `ScrollMomentum`, `PointerInertia`, `DisableGestureStats`. **`com.apple.Bluetooth`:**
  `BluetoothAutoSeekPointingDevice`/`…Keyboard` (auto-pair-on-plug). Debug flags seen: `Debug-ShowLowBatteryButton`,
  our own `debug.mt2_batt`.

## Notifications — connect/disconnect/wake (feeds the USB→BT handoff)

- **Per-device BT:** `-[IOBluetoothObject registerForConnectNotifications:selector:]` +
  `registerForDisconnectNotification:selector:` — the direct connect/disconnect hooks for a clean
  click-free USB→BT handoff ([[mt2-usb-unplug-bt-handoff]]) and reconnect handling.
- **Multitouch:** `MTSimpleEventDispatcher` posts distributed `com.apple.MultitouchSupport.HID.DeviceAdded`/
  `…DeviceRemoved` on attach/detach; `MultitouchHID::registerForSleepWakeNotifications` (`IORegisterForSystemPower`)
  for wake re-init.
- **Panes:** distributed observers for `com.apple.Bluetooth` (pref refresh) + `com.apple.menuextra.added/removed`.

## Pairing / OOB — "plug in once" is reachable on Broadcom hosts

- `-[IOBluetoothHostController addHIDEmulationDevice:classOfDevice:linkKey:]` is a **STUB** (returns
  `0xE00002C7`) on the base class — BUT **`-[BroadcomHostController addHIDEmulationDevice:…]` is a REAL impl**
  via `_BluetoothHCIDispatchUserClientRoutine` (HCI vendor cmd). So OOB HID-emulation IS available if the host
  controller is Broadcom — upgrades [[mt2-usb-oob-pairing-api]] from "stubbed on 10.9" to "stubbed on the base
  class, real on Broadcom."
- Real primitives confirmed: `BluetoothHCIWriteStoredLinkKey:inDeviceAddress:inLinkKey:` (host-side key store),
  `-[AppleBluetoothHIDDevice connectToHost:linkKey:]` (device-side connect-back, writes an IORegistry CF prop),
  `BluetoothHCIReadLocalOOBData`/`RemoteOOBDataRequestReply`, `pairSSPWithJustWorks:`/`…NumericComparison:`.
  `IOBluetoothAutomaticDeviceSetup deviceSetupWithDelegate:…notifyWhenMousePluggedIn:` = the auto-pair-on-plug hook.

## Top actionable unlocks (ranked)

1. ~~Seed `PnPVendorID` → `isApple` → Apple art everywhere~~ **ABANDONED 2026-07-04 — dead end.** `isApple`
   is already TRUE on our node yet the bezel HUD still showed a mouse → the art is not gated on it (see the
   correction under "Genuineness / `isApple`" above). Icon fixes stay per-site; the bezel HUD needs its own
   process + CoD→art lookup RE'd.
2. **Gestures are per-key on the node** (`TrackpadUserPreferences`/`MultitouchPreferences`) — full enable/tune
   control by seeding; `hwSupports3FDrag` needs the `+0xb0` bit `0x2` cap set + the property.
3. **Connect/disconnect hooks** (`registerForConnect/DisconnectNotification`) = the clean seam for the
   click-free USB→BT handoff.
4. **OOB onboarding is real on Broadcom** (`BroadcomHostController addHIDEmulationDevice` + `WriteStoredLinkKey`).
