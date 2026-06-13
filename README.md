# mt2d — Magic Trackpad 2 on OS X Mavericks

Makes an Apple Magic Trackpad 2 usable on OS X 10.9 (which has no native MT2
support): cursor, two-finger scroll, click, right-click, and tap-to-click.

## How it works

A tiny kernel extension (`MT2Claim`) out-bids the generic kernel HID driver for
the trackpad's USB interface and leaves it open; a userspace daemon (`mt2d`)
reads the raw multitouch frames, synthesizes mouse events, and injects them
through a kextless virtual HID device the system already understands.

## Requirements

- OS X 10.9 with unsigned kexts allowed:
  `sudo nvram boot-args="kext-dev-mode=1"` then reboot.
- Command Line Tools (clang, make).

## Build & install

    make pkg
    sudo installer -pkg build/mt2d-1.0.0.pkg -target /

Installs the kext under `/usr/local/lib/mt2d`, binaries under `/usr/local/sbin`,
and a LaunchDaemon that loads everything at boot. No reboot needed.

## Uninstall

    sudo launchctl unload /Library/LaunchDaemons/com.schmonz.mt2d.plist
    sudo kextunload -b com.schmonz.MT2Claim
    sudo rm -rf /Library/LaunchDaemons/com.schmonz.mt2d.plist \
        /usr/local/sbin/mt2d /usr/local/sbin/mt2d-run \
        /usr/local/sbin/mt2_reenumerate /usr/local/lib/mt2d

## Develop

    make        # build daemon + tools
    make test   # run unit tests

## Layout

- `src/` — daemon and the decode / gesture / virtual-device modules
- `kext/` — the `MT2Claim` interface-claim kernel extension
- `tools/` — dev & install helpers; `tools/spikes/` holds one-off probes
- `tests/` — unit tests
- `dist/` — LaunchDaemon plist + installer scripts
- `captures/` — recorded MT2 frames used as test fixtures

GPL-2.0-or-later (the decode/encode layouts derive from Linux
`hid-magicmouse.c`).
