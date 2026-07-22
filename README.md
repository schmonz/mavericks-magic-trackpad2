# VoodooInputMavericks

Magic Trackpad 2 driver for Mac OS X 10.9 Mavericks.

## How it works

10.9 knows how to recognize multitouch gestures from period-appropriate
laptop trackpads (USB) and the original Magic Trackpad (Bluetooth).
Our job:

1. Speak MT1 wire format
2. Present a sensible API

The programming interface is nearly verbatim from
[VoodooInput](https://github.com/acidanthera/VoodooInput):

1. Magic Trackpad 2 **satellite readers** (`mt2_*`)
   `MT2USBReader` / `MT2BTReader` out-bid `IOUSBHIDDriver` /
   `IOBluetoothHIDDriver`, open interrupt pipe / L2CAP channels,
   enable multitouch, decode each raw frame (`mt2_decode`) into a
   `MavericksTouchFrame`, and advertise `VoodooInputSupported`.
2. **Mux** (`VoodooInput`) attaches to each satellite and
   routes its `VoodooInputEvent`s to the terminal backend — upstream
   acidanthera's verbatim `VoodooInput.cpp`, with our terminal path
   selected in via the `MAVERICKS_TERMINAL` build macro.
3. **Terminal backend** (`MavericksTerminalBackend`) conditions the
   stream (`mavericks_session` / `mavericks_pipeline` /
   `mavericks_lifecycle`) and drives the **terminal**
   (`MavericksAMDTerminal`), which synthesizes an
   `AppleMultitouchDevice` and feeds it MT1 frames
   (`mavericks_amd_construct_report`).

Intent is for VoodooInput drivers to build and run unchanged, and
for upstream VoodooInput to have an easy way to add Mavericks
support, in case that's something they might want.

10.9's Trackpad prefpane needs a SIMBL plugin (included) to recognize
devices connected via our VoodooInput terminal.

## Develop

```sh
cmake --preset native                                  # configure (once; use `cross` to cross-build)
cmake --build build-native                             # tools + kext
cmake --build build-native --target kext               # just the kernel extension
ctest --test-dir build-native --output-on-failure      # unit + shell + bats tests
cmake --build build-native --target reload             # hot-reload the running kext
cmake --build build-native --target pkg
cmake --build build-native --target install-pkg
```

## Uninstall

```sh
sudo launchctl unload /Library/LaunchDaemons/dev.modernmavericks.voodooinputmavericks.plist
launchctl unload /Library/LaunchAgents/dev.modernmavericks.voodooinputmavericks.updatecheck.plist
sudo kextunload -b dev.modernmavericks.VoodooInputMavericks
sudo rm -rf /Library/LaunchDaemons/dev.modernmavericks.voodooinputmavericks*.plist \
    /Library/LaunchAgents/dev.modernmavericks.voodooinputmavericks*.plist \
    "/Library/Application Support/SIMBL/Plugins/VoodooInputMavericksPane.bundle" \
    "/Library/Application Support/ModernMavericks/Trackpad2Updater.app" \
    /usr/local/sbin/voodooinputmavericks-run /usr/local/sbin/mt2_reenumerate \
    /usr/local/libexec/mt2_linkstated \
    /usr/local/lib/voodooinputmavericks /usr/local/{var,share}/voodooinputmavericks \
    /var/db/voodooinputmavericks-boot.state
```

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
  satellites, the vendored upstream `VoodooInput` router mux, the `MavericksTerminalBackend` (conditioning + the
  `MavericksAMDTerminal` / `MavericksHIDShell` terminal), and the `MavericksVoodooInputHost` nub (an
  always-present `IOResources` sentinel for the boot brick-guard).
- `third_party/VoodooInput/` — the vendored acidanthera VoodooInput ABI headers and mux source (`VoodooInput.cpp/.hpp`, verbatim + SHA-pinned); the interface
  our framework is built against and meant to merge into.
- `tools/` — shipped: `mt2_linkstated` (the resident transport link-state coordinator daemon —
  reconnect / USB-handoff / yield / hidd-kick / shutdown-quiesce) and `mt2_reenumerate` (a boot-time
  re-enumerate to reclaim USB interface 1 when the reader loses it to `IOUSBHIDDriver`), plus the
  Trackpad-pane companion (`voodooinputmavericks_prefpane/`, a SIMBL plugin). `multitouch_*` are generic
  MultitouchSupport probes; `tools/re` is the RE toolkit; `tools/spikes/` holds one-off probes.
- `tests/` — host unit + shell tests.
- `dist/` — the LaunchDaemon/Agent plists, the `voodooinputmavericks-run` boot wrapper, installer scripts.
- `captures/` — recorded MT2 frames used as test fixtures.
- `docs/` — design specs and reverse-engineering findings (`docs/mt-stack/` is the durable knowledge base).
- `reference/` — `hid-magicmouse.c` from Linux, the basis for the decode/encode layouts.

GPL-2.0-or-later (the decode/encode layouts derive from Linux `hid-magicmouse.c`; the vendored VoodooInput
ABI is GPLv2, compatible).
