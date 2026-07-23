# How-to — working on the 10.9 multitouch stack

Recipes. For the model see `explanation.md`; for facts see `reference.md`.

## Drive a new behavior end-to-end

1. **Decode** the relevant MT2 report field (extend `src/mt2_decode.c`; it's pure C, host-testable).
2. **Condition** it through the shared session if it's stream-shaped (`src/mt2_session.c` lifecycle),
   or handle it as an edge (like the click sink).
3. **Feed** Apple's AMD: translate to the MT1 `0x28` report (`src/mt1_encode.c`) → `handleTouchFrame`,
   or post an event (`handlePointerEventFromDevice`).
4. **Inject a side-channel** only if Apple's path can't obtain it for this device (geometry, the
   button gate are the two precedents — both done by making Apple's own query/bring-up succeed, not
   by reimplementing).
5. **Verify on-device** (host-green is necessary but not sufficient — the effect must be observed in
   the real system).

## Dev-loop (reload without reboot)

```sh
# Reloading while the pad is ON/streaming is fine — empirically clean across many reloads; teardown
# restores the cloned vtable FIRST (no power-off dance needed; the old "pad OFF first" rule was dropped).
cmake --build cmake-build --target kext-unload   # drop the running/boot copy
cmake --build cmake-build --target kext-load     # build + stage + load
# Or the one-shot dev loop (unload -> drain -> load -> bounce present transport):
cmake --build cmake-build --target reload
```

- **`kext-load` (and `reload`) also `kextload`s `AppleUSBMultitouch.kext` + `AppleBluetoothMultitouch.kext`** first
  (the genuine paths `allocClassWithName` `AppleUSBMultitouchDriver` / `BNBTrackpadDevice` from those —
  NULL class otherwise). The booted deploy does the same in `dist/mt2d-run`.
- **Always verify the loaded binary contains your edit** before trusting a test (stale object has
  burned us): `strings cmake-build/MavericksVoodooInputHost.kext/Contents/MacOS/MavericksVoodooInputHost | grep '<your string>'`
- **`dmesg` accumulates across reloads** — mark the line count (`sudo dmesg | grep -c CONNTRACE`)
  before a run so you read only new lines.
- If a load fails with **kextload error 71**, it's usually an unresolved symbol / missing
  `OSBundleLibraries` entry (`sudo kextutil -n -t /tmp/MavericksVoodooInputHost.kext` shows which).

## Trackpad prefpane live-refresh (standalone osax loader — no SIMBL)

The Trackpad pref pane gets a live USB↔BT transport-refresh from a pure-C GC-neutral Scripting Addition
(`tools/mt2_prefpane_refresh/`) injected into System Preferences. Delivery is a **standalone `.osax` +
our own launch-watcher** — NOT SIMBL. How it loads (RE'd in `decisions.md` "Scripting Addition loading on
10.9"): additions load on demand only, so the watcher sends our own `MT2x`/`load` Apple event to System
Prefs on launch; the osax (in `/Library/ScriptingAdditions`) loads and its `__attribute__((constructor))`
does the work. `ascr`/`gdut` does NOT load ours (it only eagerly loads terminology-bearing additions).

```sh
# Build + install the loader (osax + watcher binary + per-user LaunchAgent), then it auto-injects on every
# System Preferences launch:
cmake --build cmake-build --target prefpane-refresh        # build osax (+ dylib + arm) ...
cmake --build cmake-build --target prefpane-refresh-install # ... osax -> /Library/ScriptingAdditions
cmake --build cmake-build --target prefpane-watch-install   # watcher -> /usr/local/libexec + LaunchAgent (loads it)

# Full teardown (osax + watcher + LaunchAgent). Leaves SIMBL alone:
cmake --build cmake-build --target prefpane-uninstall
```

- **Coexists with SIMBL.** Users keep SIMBL for unrelated plugins; we ship NO SIMBL plugin and the
  installer/uninstaller NEVER touch SIMBL. Installing our own dev SIMBL plugin AND the osax together
  double-loads (didSelect swizzled twice) — so don't; pick one loader.
- **Verify a load:** open System Preferences, then
  `grep -a "mt2panewatch\|MT2PaneRefresh\]" /var/log/system.log | tail` — expect
  `[mt2panewatch] injected pid N` then `[MT2PaneRefresh] image loaded -> swizzled didSelect -> inject handler invoked`.
- The pkg ships all three; its postinstall loads the agent for the console user (next login otherwise).
- **Validate the live refresh** with the repeatable matrix in `prefpane-test-runthrough.md` (human switches
  transports, agent reads the markers + device truth).

## Watch the live stack (runtime diagnostics)

```sh
sudo sysctl -w debug.mt2_log=1   # milestones + CONNTRACE
sudo sysctl -w debug.mt2_log=2   # verbose: per-report geometry GET/INFO, per-edge clicks
sudo sysctl -w debug.mt2_log=0   # silence (default)
```

Connect-flap verdict (after a connect cycle / reboot):
```sh
sudo dmesg | tools/re conn-trace      # per-connection timeline + STEADY/FAIL
```
A `FAIL` that never reaches `INTERRUPT_BOUND` ⇒ PSM 19 didn't open ⇒ the targeted fix is
`waitForChannelState(OPEN)` on PSM 17 (then a real repro exists to verify against).

## Fix the connect flap (when cold-boot/sleep-wake measurement shows one)

The defer-0xF1 fix already in the tree handles warm reconnect. If the CONNTRACE oracle shows a
`FAIL` that stalls before `INTERRUPT_BOUND` (PSM 19 never opened), reproduce the genuine
control-channel acceptance (see `reference.md` → BT connect handshake; constants in `src/mt2_stack.h`):

1. In `MT2BTReader::start`, after winning the control channel: `listenAt(control, cb)` then
   `waitForChannelState(OPEN)` (`MT2_L2CAP_VT_waitForChannelState`, arg `MT2_L2CAP_STATE_OPEN`)
   **before** anything else.
2. Keep deferring `0xF1`; move SET_PROTOCOL (`MT2_HIDP_SET_PROTOCOL | bit`, inverted for 05AC:0309)
   + enable to a deviceReady-equivalent — only after **both** channels are OPEN.
3. Treat the PSM-19 channel with the wait/accept ordering, not identically to control.
4. Do **not** open PSM 19 ourselves — it's device-initiated.

Verify with the same oracle: re-run the failing scenario, expect the timeline to reach `STEADY`.
If reproducing the order does **not** fix it, take the one justified live capture (see
`open-questions.md`).

## Safety before any kext load

- **Assess panic risk and get go/no-go first** — passwordless sudo is not authorization to proceed.
- **Pad off before unload** (use-after-free if the channel is still streaming into our shim).
- Reversible changes to shared Apple state must be undone in `stop()` (vtable restore, delegate
  restore, listenAt(NULL)) **in-gate, before release**.

## Add or verify a fact

Each constant in `src/mt2_stack.h` carries its `re/` command. To check one after a point release,
run it and compare; fix the header if it moved. Example:
```sh
tools/re vtable AppleBluetoothMultitouch BNBTrackpadDevice 0xcd8   # expect getMultitouchReportInfo
```
Do RE only through the in-tree `re/` wrappers (readable, allowlist-friendly), never raw otool/nm/ioreg
ad hoc. A future `re/verify-facts` could read the header and check every constant at once.

## Verify the post-merge polish (on-device, pending as of 2026-06-25)

Three deploy/identity changes shipped to `main` need on-device confirmation. Preconditions: a USB
backup pointer is live; the updated package is deployed (`cmake --build cmake-build --target pkg` +
`sudo installer -pkg cmake-build/mt2d-1.0.0.pkg -target /`, already done once).

1. **Two-boot boot-load test** (the genuine paths need Apple's kexts loaded at boot — `0109c9d`).
   First `sudo /usr/local/sbin/mt2d-run --reset` (sentinel → `ok`).
   - **USB boot:** cable the MT2, reboot. After boot (no manual `kextload`): cursor / 2-finger scroll /
     4-finger swipe / tap / physical click / 2-finger physical+tap right-click / Trackpad prefpane all
     work. Bonus: unplug → it should fall to BT and work (hot-swap).
   - **BT boot:** uncable (BT), reboot. Same checklist over BT. Glance at `/var/log/mt2d.log` for
     `full-gesture mode confirmed healthy` vs a `recover_full` cycle (BT boot match-race backstop).
   - Pass ⇒ the boot-load + sentinel fixes (`0109c9d`,`5bd650b`) are confirmed.

2. **Does the `Product` seed populate the node?** (`484e229`). Reload the kext, reconnect the MT2, then:
   `tools/re ioreg-props BNBTrackpadDevice "Product"` (BT) / `... AppleUSBMultitouchDriver "Product"`
   (USB) — expect `"Magic Trackpad 2"`. If still empty, the genuine driver overrides `Product` from the
   device and init-dict seeding doesn't stick for that key (revert the two seeds).

3. **Does a Bluetooth-pane Rename land on the device?** Right-click the MT2 row → Rename → type a
   distinctive name → Enter. The injected osax mirrors it onto the device: watch
   `grep name-mirror /var/log/system.log` for `writing onboard (0x55)` + `pushed … + cleared alias`,
   then confirm with `tools/re mt2-name` (report `0x55` = the typed name) and the Bluetooth pane showing
   it. It persists across power-cycles and follows the device to other Macs. Nothing writes the name
   unrequested — there is no boot-time or install-time name write. (See docs/mt-stack/explanation.md
   "Rename routing + the mirror".)

## Cross-build against 10.9 from a modern host (SDK + kext toolchain)

For the queued macOS-26 → CI build work: build/target 10.9 from a modern toolchain while keeping the
native 10.9 build as the reference (the modern path must be ADDITIVE — never regress building on a 10.9
host; select the toolchain by host as a seam, default = today's 10.9-native behavior). Credit the sources
below in `CREDITS.md` if they materially help the build.

### Userland — standalone MacOSX10.9 SDK

`phracker/MacOSX-SDKs` is the canonical archive of extracted historical macOS SDKs.

```sh
# Release page: https://github.com/phracker/MacOSX-SDKs/releases/tag/11.3
# Direct:       https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.9.sdk.tar.xz
# Unpack it and point a current clang/Xcode at it:
clang -isysroot <path>/MacOSX10.9.sdk -mmacosx-version-min=10.9 ...   # standard old-target cross-build
```

The **userland** cross-build is already PROVEN via the shared-CMake `dimmit` adopter. An old SDK alone
does NOT settle the KEXT side (a kext links against kernel headers, must LOAD on 10.9). Note: this box
(the MacPro6,1) is the 10.9-native build host (`clang-600.0.57`, `darwin13.4.0`), so the modern-toolchain
spike CANNOT run here — it needs a macOS-26 host. Old `clang-600` does not grok the blog's
`-target x86_64-apple-macos10.9` triple (emits ELF); 10.9-era syntax is
`-arch x86_64 -mmacosx-version-min=10.9`.

### Kext — build a loadable 10.9 kext from a modern toolchain (leads, not yet attempted)

Open question: can a modern toolchain build a kext that LOADS on 10.9? The recipe pieces:
- **XNU headers matching the 10.9.x kernel** (kexts link against kernel headers, not the userland SDK).
- clang cross-target sketch:
  ```sh
  clang -target x86_64-apple-macos10.9 -mkernel -nostdinc \
        -I<xnu>/libkern -I<xnu>/iokit -I<xnu>/osfmk -I<xnu>/bsd ...   # the driver's include graph
  ```
- kernel include paths, **`-mkernel`**, correct **linker flags**, packaging as a **`.kext` bundle** (not a
  normal executable).
- **KEY INSIGHT:** a kext does NOT need the whole XNU source tree — only the include graph its driver
  actually pulls in (typically `libkern`, `iokit`, `osfmk`, and selected parts of `bsd`). Identify that
  graph and carve just those dirs.

**Sources:**
- Recipe blog (building XNU for 10.9 Mavericks):
  http://shantonu.blogspot.com/2013/10/building-xnu-for-os-x-109-mavericks.html
- XNU source matching 10.9.x (2422 = Mavericks):
  https://github.com/apple-oss-distributions/xnu/tree/xnu-2422.115.4
  (tarball: https://github.com/apple-oss-distributions/xnu/archive/xnu-2422.115.4.tar.gz)
- 10.9.0 XNU tarball: https://opensource.apple.com/tarballs/xnu/xnu-2422.1.72.tar.gz
- Apple open-source index: https://opensource.apple.com/releases/

**Dependency map (off-device de-risk, from our own code):**
- **Kernel-header dependency set is small + bounded.** Angle-bracket includes across `kext-gesture/`:
  IOKit core (IOService, IOLib, IOLocks, IOWorkLoop, IOCommandGate/Pool, IOTimerEventSource, IOUserClient,
  IOBufferMemoryDescriptor, IOCommand), libkern/c++ (OSObject/Dictionary/Number/String/Boolean/MetaClass),
  kern/clock.h, mach/{kmod,mach_types}.h, sys/sysctl.h, string.h — all from **xnu-2422.x**. PLUS two family
  headers NOT in xnu: `IOKit/hid/IOHIDDevice.h` (**IOHIDFamily**) and `IOKit/usb/IOUSBInterface.h`
  (**IOUSBFamily**).
- **Link deps** (Info.plist `OSBundleLibraries`, KPI ver 13.4 = 10.9.4): `com.apple.kpi.{iokit,libkern,mach,bsd}`
  + `IOHIDFamily`(1.5) + `IOUSBFamily`(1.8) + `IOBluetoothFamily`(2.0.0) + `AppleMultitouchDriver`(193.8).
- **Build-time headers NOT needed for IOBluetoothFamily or AppleMultitouchDriver** — the kext REDECLARES
  the minimal Apple classes it splices (`IOBluetoothL2CAPChannel`/`IOBluetoothObject`; AMD) and resolves
  the mangled symbols AT LOAD via `OSBundleLibraries`. So a modern build needs headers for **xnu +
  IOHIDFamily + IOUSBFamily ONLY**; Bluetooth/AMD are redeclare + link-resolve.
- **Simplest single header/stub source = the 10.9 Kernel Debug Kit (KDK)** — bundles xnu KPIs + IOHIDFamily
  + IOUSBFamily + IOBluetoothFamily + AppleMultitouchDriver headers AND the KPI symbol/export lists for the
  link step — likely cleaner than carving xnu + IOHIDFamily + IOUSBFamily repos separately.

**Remaining unknowns for the kext spike (need a macOS-26 host):** whether modern clang/ld64 can emit a
Mach-O kext that LOADS on 10.9 (KPI symbol versioning, codesigning/`-mkernel` ABI), and sourcing the 10.9
KDK (Apple KDKs for 10.9 are archived, findable). The header/dep graph above is settled.

## Test a rename-release update (e.g. 0.4.5 → 0.5.0)

0.5.0 renames the whole identity (`com.schmonz.mt2*` → `dev.modernmavericks.voodooinputmavericks*`,
loader `mt2d-run` → `voodooinputmavericks-run`, kext `com.schmonz.MT2Gesture` →
`dev.modernmavericks.VoodooInputMavericks`). Sparkle applies updates by **running the new `.pkg`**, so the
update = (fetch/accept) + (install new pkg over old). Verify each half.

**Fetch/accept (static, already verified):** v0.4.5 and HEAD ship byte-identical `SUFeedURL`
(`…/releases/latest/download/appcast.xml`, a stable "latest release" URL) and `SUPublicEDKey`
(`E8ZT…lGs=`). So a v0.4.5 updater will see the 0.5.0 GitHub release and accept a pkg signed with the
unchanged key. `git show v0.4.5:tools/mt2_updater/Info.plist.in` vs `tools/CMakeLists.txt` FEED_URL/ED_PUBKEY.

**Model A — the update does NOT hot-swap the kext.** A kext driving the live MT2 can't be unloaded in place
(retained synthetic `IOHIDDevice`; see decisions.md "Kexts can't be hot-swapped over a live driver"). So an
update STAGES the new kext, leaves the old one driving the trackpad, notifies "restart to finish," and the
new kext loads cleanly at the next boot. A FRESH install (no prior kext resident) loads immediately, no
reboot. Test both halves accordingly.

**Migration completeness (static):** completeness means the old LOADERS/plists/binaries/kext-files are
removed so nothing reloads the old identity at that boot — NOT that we `kextunload` a running driver (that
partial-teardown is what left a user dead-until-reboot). `tests/test_preinstall_migration.sh` asserts it
against the *genuine* v0.4.5 pkg manifest:
```sh
gh release download v0.4.5 -p '*.pkg' -D /tmp/mig            # genuine 0.4.5 pkg
pkgutil --expand /tmp/mig/mt2d-0.4.5.pkg /tmp/mig/x
lsbom /tmp/mig/x/mt2d-component.pkg/Bom | awk '{print $1}' | grep -iE 'LaunchDaemons|LaunchAgents|mt2d'
# every plist/loader/kext-file it lists must be rm'd in dist/scripts/preinstall; preinstall must NOT kextunload
```

**Migration (definitive, on-device — pkg-over-pkg, no signing/feed needed):** a THREE-step test (install old,
install new, REBOOT). The trackpad keeps working on the old version throughout; nothing breaks pre-reboot.
```sh
sudo installer -pkg /tmp/mig/mt2d-0.4.5.pkg -target /        # genuine 0.4.5 -> old identity loads + drives
kextstat | grep -i MT2Gesture ; launchctl list | grep com.schmonz.mt2 ; ls /usr/local/lib/mt2d  # OLD present

sudo installer -pkg build-native/voodooinputmavericks-0.5.0.pkg -target /   # UPDATE: stages, does NOT hot-swap
# EXPECT (this is correct Model-A behavior, not errors): a "restart to finish" notice appears; the OLD kext
# is STILL resident (kextstat unchanged) and still driving; the new kext files are on disk. So the oracle is
# NOT clean yet -- it should report the old kext still resident PRE-reboot:
sh tools/spikes/verify_migration.sh          # pre-reboot: expect residue/old-kext WARN, NOT exit 0

sudo reboot                                   # apply: nothing resident at boot -> new kext loads clean
# after login + a touch on the pad:
sh tools/spikes/verify_migration.sh          # NOW expect exit 0 == no old residue + new kext resident
```
For the FRESH-install path (no prior kext), the postinstall fires the WatchPaths trigger and the kext comes
up immediately — `verify_migration` is clean with no reboot.

**Full Sparkle path (optional, needs the private signing key):** sign the 0.5.0 pkg + `gen_appcast.sh`,
serve `appcast.xml`+pkg locally, `defaults write com.schmonz.MavericksTrackpad2Updater SUFeedURL
http://localhost:PORT/appcast.xml` on a genuine-0.4.5 box, trigger the updater, then run the oracle. Only
needed to exercise Sparkle's own download/verify UI; the pkg-over-pkg test already covers the migration.
