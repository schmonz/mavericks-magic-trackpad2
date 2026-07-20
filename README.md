# VoodooInputMavericks

Native gestures with Apple's Magic Trackpad 2 on Mac OS X 10.9 — built as a **VoodooInput-shaped
trackpad framework** with a small device-specific satellite. The Magic Trackpad 2 driver is ~200
lines; the reusable machinery is named and structured to merge back into
[acidanthera/VoodooInput](https://github.com/acidanthera/VoodooInput).

## How it works

The Magic Trackpad 2 speaks a wire format 10.9 doesn't understand. Instead of reusing Apple's genuine
drivers, we present the device to the OS through the **VoodooInput** interface, then let Apple's own
recognizer do the gesture work against a device we fabricate:

1. **Satellite readers** (`mt2_*`, the ~3% that is device-specific) — `MT2USBReader` / `MT2BTReader`
   out-bid `IOUSBHIDDriver` / `IOBluetoothHIDDriver` to win the trackpad, open its interrupt pipe /
   L2CAP channels, enable multitouch, and decode each raw frame (`mt2_decode` + the
   `mt2_usb_decode`/`mt2_bt_decode` transport wrappers) into a `MavericksTouchFrame`. Each satellite
   advertises `VoodooInputSupported` + its coordinate span and `registerService()`s — the whole
   satellite contract.
2. **The mux** (`MavericksVoodooInput`, our reimplementation of upstream's `VoodooInput`) attaches to
   each satellite, receives `VoodooInputEvent`s, and conditions the stream — settle gate, contact
   lifecycle, liftoff, click logic (`mavericks_session` / `mavericks_pipeline` / `mavericks_lifecycle`).
3. **The terminal** (`MavericksAMDTerminal`) fabricates its **own** `AppleMultitouchDevice`, seeds it
   with genuine sensor geometry and the device-button gate, and feeds it MT1 `0x28` frames
   (`mavericks_amd_construct_report`). Apple's recognizer, `hidd`, and gesture engine then drive the
   cursor / gestures / clicks against our fabricated device — **no Apple driver is hosted or interposed.**

Two 10.9 quirks we handle, both RE'd (`docs/mt-stack/`):
- `hidd` only opens the multitouch **frames client** when the device appears while it is freshly
  enumerating, so the boot wrapper kicks `hidd` once when the USB reader binds (the BT reconnect bounce
  covers Bluetooth).
- Apple's Trackpad **preference pane** only shows the trackpad view for a genuine driver *class*, so a
  userland companion forces the trackpad controller for our fabricated device and keeps it honest as the
  device connects / disconnects / switches transport.

Mavericks loads an unsigned kext as long as it's *not* in `/Library/Extensions` (so it's absent at early
boot); the `voodooinputmavericks-run` LaunchDaemon kextloads it from `/usr/local/lib`, with a boot
brick-guard so a load panic can't loop.

## Requirements

- Mac OS X 10.9
- Command Line Tools (clang) + CMake (≥3.10).
- [mavericks-shared-cmake](https://github.com/schmonz/mavericks-shared-cmake) installed once — the build
  finds it via `find_package`:

      git clone https://github.com/schmonz/mavericks-shared-cmake
      cmake -S mavericks-shared-cmake -B mavericks-shared-cmake/build
      cmake --install mavericks-shared-cmake/build --prefix "$HOME/.local"

  The install self-registers in CMake's per-user package registry, so no `CMAKE_PREFIX_PATH` is needed.

## Build & install

    cmake --preset native
    cmake --build build-native --target pkg
    sudo installer -pkg build-native/voodooinputmavericks-<version>.pkg -target /

Or, to install exactly what a release ships (same installer scripts, both prefpane loaders):

    cmake --build build-native --target install-pkg

## Uninstall

    sudo launchctl unload /Library/LaunchDaemons/com.schmonz.voodooinputmavericks.plist
    launchctl unload /Library/LaunchAgents/com.schmonz.voodooinputmavericks.panewatch.plist
    launchctl unload /Library/LaunchAgents/com.schmonz.voodooinputmavericks.updatecheck.plist
    sudo kextunload -b com.schmonz.VoodooInputMavericks
    sudo rm -rf /Library/LaunchDaemons/com.schmonz.voodooinputmavericks*.plist \
        /Library/LaunchAgents/com.schmonz.voodooinputmavericks*.plist \
        /Library/ScriptingAdditions/VoodooInputMavericksPane.osax \
        /usr/local/libexec/voodooinputmavericks_pane_watch \
        /usr/local/sbin/voodooinputmavericks-run /usr/local/sbin/mt2_reenumerate \
        /usr/local/lib/voodooinputmavericks /usr/local/{var,share}/voodooinputmavericks \
        /var/db/voodooinputmavericks-boot.state

(If you installed the prefpane companion as a SIMBL plugin rather than the standalone osax, also remove
`~/Library/Application Support/SIMBL/Plugins/VoodooInputMavericksPane.bundle`.)

## Develop

    cmake --preset native                                  # configure (once; use `cross` to cross-build)
    cmake --build build-native                             # tools + kext
    cmake --build build-native --target kext               # just the kernel extension
    ctest --test-dir build-native --output-on-failure      # unit + shell + bats tests
    cmake --build build-native --target reload             # hot-reload the running kext

## Layout

Four namespaces, by provenance: **`mt2_*`** = the device satellite · **`mavericks_*` / `Mavericks*`** =
our reusable framework · **`VoodooInput*`** = the vendored upstream ABI · **`VoodooInputMavericks`** =
the shipping product.

- `src/` — the framework core + the device decode, compiled into the kext and exercised by the host unit
  tests. Device (`mt2_*`): `mt2_decode` with the `mt2_usb_decode`/`mt2_bt_decode` wrappers, `mt2_geometry`
  + `mt2_usb_bytes`, `mt2_battery`, `mt2_coord_range`. Framework (`mavericks_*`): `MavericksTouchFrame`,
  `mavericks_voodoo_translate` (frame ↔ `VoodooInputEvent`), `mavericks_session`/`_pipeline`/`_lifecycle`
  (conditioning), `mavericks_amd_construct_report` (the terminal's report builder), `mavericks_presence`/
  `_connect_sm`/`_coordinator` (transport SMs). `mavericks_vhid_mt1` is a kextless research path kept for
  reference.
- `kext-gesture/` — the one shipped kext (`VoodooInputMavericks.kext`): the `MT2USBReader` / `MT2BTReader`
  satellites, the `MavericksVoodooInput` mux, the `MavericksAMDTerminal` + `MavericksHIDShell` terminal,
  and the `MavericksVoodooInputHost` nub (an always-present `IOResources` sentinel for the boot brick-guard).
- `third_party/VoodooInput/` — the vendored acidanthera VoodooInput ABI headers (verbatim); the interface
  our framework is built against and meant to merge into.
- `tools/` — `mt2_reenumerate` ships, as does the Trackpad-pane companion (`voodooinputmavericks_prefpane/`,
  delivered as a SIMBL plugin or a standalone `.osax` + launch-watcher). `multitouch_*` are generic
  MultitouchSupport probes; `tools/re` is the RE toolkit; `tools/spikes/` holds one-off probes.
- `tests/` — host unit + shell tests.
- `dist/` — the LaunchDaemon/Agent plists, the `voodooinputmavericks-run` boot wrapper, installer scripts.
- `captures/` — recorded MT2 frames used as test fixtures.
- `docs/` — design specs and reverse-engineering findings (`docs/mt-stack/` is the durable knowledge base).
- `reference/` — `hid-magicmouse.c` from Linux, the basis for the decode/encode layouts.

GPL-2.0-or-later (the decode/encode layouts derive from Linux `hid-magicmouse.c`; the vendored VoodooInput
ABI is GPLv2, compatible).
