# Credits & Acknowledgments

This driver stands on a lot of other people's reverse-engineering, code, and writing. Thank you.
If you contributed and aren't listed (or want different wording/attribution), please open an issue.

## License-obligated attribution

- **Linux `hid-magicmouse.c`** (GPL-2.0) — the Magic Trackpad USB/Bluetooth frame decode in this
  project derives from it. This is why the project is **GPL-2.0-or-later**.
  https://github.com/torvalds/linux/blob/master/drivers/hid/hid-magicmouse.c
  - **Michael Poole** — original `hid-magicmouse` (Magic Mouse) driver.
  - **Chase Douglas** — added Magic Trackpad support ([LKML, 2010](https://lkml.org/lkml/2010/8/31/324)):
    the finger format, the full multitouch-enable feature sequence, and the MT1 coordinate ranges.

## Reverse-engineering & guidance

- **f54da** (MacRumors) — started the ["Magic Trackpad 2 on 10.9 or lower" thread](https://forums.macrumors.com/threads/magic-trackpad-2-on-10-9-or-lower.2332288/),
  reverse-engineered the MT2 protocol and the 10.9 multitouch gesture internals (incl. the
  `MTSlideGesture::isBlocked` right-edge dead-zone bug), and pointed the way to the
  `MultitouchSupport` private API. This project began from that thread. Later shared detailed
  multitouch-stack exploration notes ([post #34643868](https://forums.macrumors.com/threads/magic-trackpad-2-on-10-9-or-lower.2332288/?post=34643868)):
  the `BNBTrackpad → AppleMultitouchDevice → AppleMultitouchHID` pipeline, the gesture→action
  mapping (`MTPListGestureConfig`), `MTChordCycling`/`MTSlideGesture`, and a full CFG analysis of
  the Bluetooth-transport right-edge dead-zone (`isBlocked` block `0xf8a4`, `handstats[0xd9]`).
- **Daniel** — [encyclopediaofdaniel.com: "Making the Magic Trackpad Work"](https://encyclopediaofdaniel.com/blog/making-the-magic-trackpad-work/),
  remapping the trackpad's right half to right-click via the MultitouchSupport API + event taps.
- **Nathan Vander Wilt / Calf Trail Software** — the `MultitouchSupport.h` header definitions
  (TrackMagic, Touch). https://github.com/calftrail/TrackMagic · https://github.com/calftrail/Touch
- **rmhsilva** — [gist compiling the MultitouchSupport private API](https://gist.github.com/rmhsilva/61cc45587ed34707da34818a76476e11)
  (Finger/MTTouch structs, the MTTouchState lifecycle, device-inspection functions).
- **Steike** (steike.com) — the original C MultitouchSupport example (2009), source of the Finger
  struct + `MTDevice*` usage many later projects build on
  ([via Wayback](https://web.archive.org/web/20151012175118/http://steike.com/code/multitouch/)).
  - **comex** — the Snow Leopard / 64-bit fix (`MTDeviceRef`→`long`, extra `MTDeviceStart` arg).

## Reference drivers & projects

- **VoodooInput** (acidanthera) — a maintained multitouch-device *simulator*; the reference for
  presenting a device the native gesture engine adopts (our "BNBDevice-equivalence" approach), and
  for the haptic actuator. We also **speak its multitouch wire interface**: we vendor acidanthera's
  VoodooInput ABI headers **verbatim** (GPL-2.0, identical to this project) at `third_party/VoodooInput/`
  (pinned SHA `d897813`), so a driver authored for VoodooInput can be compiled for Mavericks and target
  this 10.9 back-port. Our `com_schmonz_VoodooInput` multiplexer receives `kIOMessageVoodooInputMessage`
  and translates each `VoodooInputEvent` into our internal `mt2_frame` (`src/mt2_frame.h`) for the shared
  conditioning engine. Our own **MT2 Bluetooth reader is the worked satellite example**: it publishes
  `VoodooInputSupported` + its coordinate span, is bound by our multiplexer, and delivers each decoded
  frame via `messageClient(kIOMessageVoodooInputMessage, …)` with no special glue — the driver eats
  the interface it exposes to third parties, exercising it end-to-end on real hardware.
  https://github.com/acidanthera/VoodooInput
- **mac-precision-touchpad** (imbushuo) & **MagicTrackpad2ForWindows** (vitoplantamura) — open-source
  Windows MT2 drivers; sources for the MT2 surface geometry, finger-report layout, and haptic
  actuation format. https://github.com/imbushuo/mac-precision-touchpad ·
  https://github.com/vitoplantamura/MagicTrackpad2ForWindows
- **OpenMultitouchSupport** (Kyome22), **FingerMgmt**, **TrackMagic**, **tongseng**, and the apps
  **BetterTouchTool** / **MagicPrefs** — MultitouchSupport ecosystem references.

## Specs, research & tools

- **SkySafe — CVE-2024-0230** — documents Apple's USB out-of-band Bluetooth pairing mechanism.
  https://github.com/skysafe/reblog/blob/main/cve-2024-0230/README.md
- **LKML — MT1 control/feature codes (2010)** — https://lkml.org/lkml/2010/8/31/324
- **Apple Support** — Magic Trackpad requires OS X 10.6.4 (fixes the supported-OS floor).
  https://support.apple.com/en-us/106534
- **PacketLogger** (Apple, in Xcode's Additional Tools) — Bluetooth HID sniffing.
- **Magic Utilities** (magicutilities.net) — its device-config docs confirmed that Magic devices store
  their Bluetooth name *on the device* and that it's host-writable (rename writes the device + restarts;
  a factory reset restores the default name) — the reference that the on-device name is a real, settable
  field. https://magicutilities.net/magic-mouse/help/device-config
- **orlp/ed25519** (Orson Peters, public domain / zlib) — the ed25519 implementation Sparkle vendors
  for its EdDSA update signatures. We fetch + build the same library so our native `mt2_sign_update` /
  `mt2_generate_keys` produce byte-identical signatures the Sparkle framework accepts, buildable on the
  10.9 toolchain (no Swift). https://github.com/orlp/ed25519

---

*Capture policy: when a source materially helps the project, add it here so credit travels with
the code. (Full technical notes live in `docs/mt-stack/`.)*
