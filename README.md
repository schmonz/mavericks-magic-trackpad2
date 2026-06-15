# mt2d — Magic Trackpad 2 on OS X Mavericks

Makes an Apple Magic Trackpad 2 usable on OS X 10.9, which has no native MT2
support. It drives Apple's **real** multitouch gesture engine, so you get a
proper cursor, two-finger scroll, and swipes — not synthesized mouse events.

> Status: full gestures (cursor, two-finger scroll, swipes) work and survive
> reboot unattended. Physical click and tap-to-click are **not wired up yet**
> (see the work list / open issues).

## How it works

Three pieces turn raw trackpad frames into real Apple gestures:

1. **`MT2Claim`** (input kext) out-bids the generic kernel HID driver for the
   trackpad's multitouch USB interface and leaves it open for userspace.
2. **`mt2_gesture_feed`** (userspace daemon) reads the raw multitouch frames,
   decodes them, re-encodes each as a Magic Trackpad 1 (`0x28`) report, and
   pushes it into the `MT2Gesture` kext's user client.
3. **`MT2Gesture`** (output kext) constructs an in-kernel `AppleMultitouchDevice`
   and feeds it those frames, so Apple's own gesture engine (in `hidd`) drives
   the cursor and gestures — the same path a native trackpad uses.

The feeder self-heals: it waits for the device at boot and reconnects after an
unplug/replug. A boot wrapper (`mt2d-run`, run by a LaunchDaemon) loads both
kexts at startup from a loose path and re-enumerates the device if `MT2Claim`
didn't win the match. The kexts are unsigned, so they are **never** placed in
`/Library/Extensions` or the boot caches (only `kextload`ed from
`/usr/local/lib/mt2d`) — the kernel always boots clean. A small sentinel
(`/var/db/mt2d-boot.state`) guards against a kext-load panic boot-looping: if a
boot starts full mode but never confirms healthy, the next boot skips loading
`MT2Gesture` until you run `sudo /usr/local/sbin/mt2d-run --reset`.

## Requirements

- OS X 10.9 with unsigned kexts allowed:
  `sudo nvram boot-args="kext-dev-mode=1"` then reboot.
- Command Line Tools (clang, make).

## Build & install

    make pkg
    sudo installer -pkg build/mt2d-1.0.0.pkg -target /

Installs the kexts under `/usr/local/lib/mt2d`, binaries under
`/usr/local/sbin`, and a LaunchDaemon that loads everything at boot. No reboot
needed.

## Uninstall

    sudo launchctl unload /Library/LaunchDaemons/com.schmonz.mt2d.plist
    sudo kextunload -b com.schmonz.MT2Gesture
    sudo kextunload -b com.schmonz.MT2Claim
    sudo rm -rf /Library/LaunchDaemons/com.schmonz.mt2d.plist \
        /usr/local/sbin/mt2_gesture_feed /usr/local/sbin/mt2d-run \
        /usr/local/sbin/mt2_reenumerate /usr/local/lib/mt2d \
        /var/db/mt2d-boot.state

## Develop

    make                 # build the feeder + dev tools
    make kext kext-gesture   # build the two kernel extensions
    make test            # run unit tests

## Layout

- `src/` — frame decode/encode + USB reader (`mt2_reader`, `mt2_decode`,
  `mt1_encode`, `touch_model.h`). `mt2d_mt1`/`vhid_mt1` are a kextless research
  path kept for reference.
- `kext/` — `MT2Claim`, the input interface-claim kernel extension.
- `kext-gesture/` — `MT2Gesture`, the output kext that constructs the in-kernel
  `AppleMultitouchDevice` and drives Apple's gesture engine.
- `tools/` — `mt2_gesture_feed` (the feeder) plus dev/diagnostic helpers;
  `tools/spikes/` holds one-off probes.
- `tests/` — unit tests.
- `dist/` — LaunchDaemon plist, the `mt2d-run` boot wrapper, and installer scripts.
- `captures/` — recorded MT2 frames used as test fixtures.

GPL-2.0-or-later (the decode/encode layouts derive from Linux
`hid-magicmouse.c`).
