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

**Bezel HUD (connect/disconnect):** BezelServices itself has **zero** device-identity sites — it's the HUD
backend; the icon is passed IN by the caller, which resolves it through the same CoD vault. So the
mouse-image HUD is the same CoD/`isApple` limitation, and the `isApple` lever (or a per-caller swizzle)
would fix it. ([[mt2-connect-disconnect-bezel-hud]])

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

## ⭐ Genuineness — the root icon lever (SEEDABLE)

`-[IOBluetoothObject isAppleDevice]` (@0xcd80) = **`(PnPVendorIDSource==2/USB && PnPVendorID==0x05AC)` OR
`(source==1/BT-SIG && PnPVendorID==0x004C)`**. This one boolean is what `_IOBluetoothGetGenericTypeStringForDeviceClasses`'s
`isApple` flag ultimately reflects — i.e. **Apple-vs-generic art at every icon site (BT-pane row, menu extra,
connect/disconnect bezel HUD) hinges on it.** Our unit IS genuine Apple hardware (vendor `0x05AC`), so this
is *accurate to seed, not impersonation* — the icon is generic today only because `PnPVendorID`/`…Source`
aren't reaching the node `isAppleDevice` reads. **Seed `PnPVendorID`=0x05AC (USB)/0x004C (BT) + `PnPVendorIDSource`
on our node → `isApple` true → Apple trackpad art everywhere, no swizzle.** Closes the pane-picture + bezel-HUD
items at the root. (Verify which node/path the bezel HUD actually resolves through — it showed a mouse image,
so confirm CoD→trackpad + isApple on that path.) Sibling checks (`appleBluetoothMousePresent/HIDDevicePresent/
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

1. **Seed `PnPVendorID`=0x05AC/0x004C + `PnPVendorIDSource`** → `isApple` true → Apple art at ALL icon sites
   (pane/menu/bezel), no swizzle. Verify our node currently lacks it; it's genuine so it's correct to seed.
2. **Gestures are per-key on the node** (`TrackpadUserPreferences`/`MultitouchPreferences`) — full enable/tune
   control by seeding; `hwSupports3FDrag` needs the `+0xb0` bit `0x2` cap set + the property.
3. **Connect/disconnect hooks** (`registerForConnect/DisconnectNotification`) = the clean seam for the
   click-free USB→BT handoff.
4. **OOB onboarding is real on Broadcom** (`BroadcomHostController addHIDEmulationDevice` + `WriteStoredLinkKey`).
