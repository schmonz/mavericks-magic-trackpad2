# Magic Trackpad 2 on Mac OS X Mavericks

Native gestures with Apple's Magic Trackpad 2 on Mac OS X 10.9.

## How it works

A kernel extension reuses Apple's own genuine multitouch drivers and feeds them a translated stream, so 10.9's existing recognizer does all the gesture/cursor/click work:

1. `MT2USBReader` and `MT2BTReader` out-bid `IOUSBHIDDriver` and
   `IOBluetoothHIDDriver` to win the Magic Trackpad 2, then **manually
   start Apple's genuine driver** on the same transport —
   `AppleUSBMultitouchDriver` over USB, `BNBTrackpadDevice` over
   Bluetooth — and **interpose its input seam** (`handleReport` over
   USB; the L2CAP delegate callback over BT).
2. Each raw MT2 frame is decoded (`mt2_decode` plus the
   `mt2_usb_decode`/`mt2_bt_decode` transport wrappers), conditioned by
   the shared `mt2_session` (settle gate, contact lifecycle, liftoff),
   and translated to the report Apple's driver expects (`mt1_encode` to
   the MT1 `0x28` report over BT; a CompactV4 reframe over USB). We also
   inject the two things Apple's path can't read off this device's wire
   — sensor **geometry** and the **device-button gate** — so the genuine
   `AppleMultitouchDevice` drives the cursor, gestures, and the Trackpad
   preference pane. See `docs/mt-stack/` for the full architecture.

Mavericks will load an unsigned kext as long as it's _not_ in
`/Library/Extensions` and therefore not present at early boot. Once
loaded, we reenumerate the device if needed and attach.

## Requirements

- Mac OS X 10.9
- Command Line Tools (clang, make).

## Build & install

    make pkg
    sudo installer -pkg build/mt2d-1.0.0.pkg -target /

Installs the kext under `/usr/local/lib/mt2d`, userland helpers
under `/usr/local/sbin`, and a LaunchDaemon for when you reboot
(but you don't need to).

## Uninstall

    sudo launchctl unload /Library/LaunchDaemons/com.schmonz.mt2d.plist
    sudo kextunload -b com.schmonz.MT2Gesture
    sudo rm -rf /Library/LaunchDaemons/com.schmonz.mt2d.plist \
        /usr/local/sbin/mt2d-run /usr/local/sbin/mt2_reenumerate \
        /usr/local/lib/mt2d /var/db/mt2d-boot.state /var/log/mt2d.log

## Develop

    make                 # build the dev/diagnostic tools
    make kext-gesture    # build the kernel extension
    make test            # run unit tests

## Layout

- `src/` — the shared frame decode/encode + session core, compiled into the kext
  and exercised by the userspace unit tests: `mt2_decode` (shared core) with the
  thin `mt2_usb_decode`/`mt2_bt_decode` transport wrappers, `mt1_encode`,
  `mt2_pipeline`/`mt2_session` (settle / lift-drop / decel / click logic), and
  `touch_model.h`. `vhid_mt1` is a kextless research path kept for reference.
- `kext-gesture/` — `MT2Gesture`, the one shipped kext: the `MT2USBReader` and
  `MT2BTReader` transport readers that manual-start + interpose Apple's genuine
  drivers, and the `MT2Gesture` nub that hosts the shared session and feeds the
  conditioned stream to Apple's spawned `AppleMultitouchDevice`.
- `tools/` — dev/diagnostic helpers; only `mt2_reenumerate` ships, the rest are
  reverse-engineering probes (`tools/re` is the RE toolkit). `tools/spikes/` holds one-off probes.
- `tests/` — unit tests.
- `dist/` — LaunchDaemon plist, the `mt2d-run` boot wrapper, and installer scripts.
- `captures/` — recorded MT2 frames used as test fixtures.
- `docs/` — design specs and reverse-engineering findings.
- `reference/` — `hid-magicmouse.c` from Linux, the basis for the decode/encode
  layouts (see the license note below).

GPL-2.0-or-later (the decode/encode layouts derive from Linux
`hid-magicmouse.c`).
