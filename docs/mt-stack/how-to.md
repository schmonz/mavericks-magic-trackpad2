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
  burned us): `strings cmake-build/MT2Gesture.kext/Contents/MacOS/MT2Gesture | grep '<your string>'`
- **`dmesg` accumulates across reloads** — mark the line count (`sudo dmesg | grep -c CONNTRACE`)
  before a run so you read only new lines.
- If a load fails with **kextload error 71**, it's usually an unresolved symbol / missing
  `OSBundleLibraries` entry (`sudo kextutil -n -t /tmp/MT2Gesture.kext` shows which).

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

3. **Does `mt2_set_btname` work from root@launchd at boot?** First clear the override:
   `sudo /usr/libexec/PlistBuddy -c "Delete :DeviceCache:<addr>:displayName" /Library/Preferences/com.apple.Bluetooth.plist`
   (or rename the device to junk), reboot, then check the Bluetooth pane shows "Magic Trackpad 2" and
   `defaults read /Library/Preferences/com.apple.Bluetooth DeviceCache | grep -iA22 <addr> | grep displayName`.
   If it did NOT get set, IOBluetooth `setDisplayName:` needs a user session → move the invocation from
   `dist/mt2d-run` (root LaunchDaemon) to a per-user LaunchAgent. (Running the tool by hand in a user
   shell is known to work + live-update the pane.)
