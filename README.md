# Magic Trackpad 2 on Mac OS X Mavericks

Native gestures with Apple's Magic Trackpad 2 on Mac OS X 10.9.

## How it works

A kernel extension translates input to look like it came from an original Magic Trackpad, which 10.9's `hidd` understands:

1. `MT2USBReader` and `MT2BTReader` out-bid `IOUSBHIDDriver` and
   `IOBluetoothHIDDriver`, enable multitouch, and `mt2_decode` raw
   frames. `mt2_session` and `mt2_pipeline` merge USB frames or
   Bluetooth events into a unified stream to `mt1_encode`.
2. `MT2Gesture` receives Magic Trackpad 1 events and passes them
   to a constructed `AppleMultitouchDevice`.

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
- `kext-gesture/` — `MT2Gesture`, the one shipped kext: the `IOResources` nub
  that builds the in-kernel MT1 HID interface (`MT2HIDShell`) which Apple's
  `AppleMultitouchDevice`/event driver binds onto, plus the `MT2USBReader` and
  `MT2BTReader` transport reader personalities.
- `kext-bnbinject/`, `kext-startmt/` — research/experiment kexts (earlier,
  abandoned approaches) kept for reference; not built or shipped.
- `tools/` — dev/diagnostic helpers; only `mt2_reenumerate` ships, the rest are
  reverse-engineering probes. `tools/spikes/` holds one-off probes.
- `tests/` — unit tests.
- `dist/` — LaunchDaemon plist, the `mt2d-run` boot wrapper, and installer scripts.
- `captures/` — recorded MT2 frames used as test fixtures.
- `docs/` — design specs and reverse-engineering findings.
- `reference/` — `hid-magicmouse.c` from Linux, the basis for the decode/encode
  layouts (see the license note below).

GPL-2.0-or-later (the decode/encode layouts derive from Linux
`hid-magicmouse.c`).
