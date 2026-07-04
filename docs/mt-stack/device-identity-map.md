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
| **On-device name** (`setDeviceName:`) / **alias** (`setDisplayName:`) | pane Rename, menu title, blued `deviceName` | the visible + onboard name | write via `-[BluetoothHIDDevice setDeviceName:]` / alias | in progress ([[mt2-device-writable-name]], mirror) |
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
