# Hands-on test runthrough — Trackpad prefpane live transport-refresh

A repeatable on-device procedure for validating the prefpane live USB↔BT refresh through the standalone-osax
loader (watcher + osax; no SIMBL). The agent drives tools + reads logs; a **human drives the physical transport
actions** (plug/unplug USB, power the MT2 on/off) and reports the on-screen UI. Score PASS/FAIL per row.

## Preconditions
- Loader installed + live: `launchctl list | grep mt2panewatch` shows a pid. If not:
  `cmake --build cmake-build --target prefpane-refresh-install` then `... --target prefpane-watch-install`.
- A **backup pointer** (mouse) — powering the MT2 off while it's the only pointer strands you.
- SIMBL may stay installed for unrelated plugins (we coexist); just don't also install our dev SIMBL plugin
  (osax + our SIMBL plugin together = double-swizzle).
- This is **userspace only — no kext, no panic risk**.

## The oracle (run per step)
```sh
# 1. mark the syslog position BEFORE the physical action:
LB=$(wc -l < /var/log/system.log)
# 2. <human performs the physical action + reports the screen>
# 3. read what our payload did:
sudo sed -n "$((LB+1)),\$p" /var/log/system.log | grep -aE "MT2PaneRefresh|mt2panewatch"
# 4. device truth (what the pane SHOULD show):
tools/re ioreg-class BNBTrackpadDevice AppleUSBMultitouchDriver   # count: 1/0 per class
```

### Marker glossary (syslog `[MT2PaneRefresh]` / `[mt2panewatch]`)
- `injected pid N (try K)` — watcher sent `MT2x`/`load` on a System Prefs launch.
- `image loaded` → `swizzled -[NSPreferencePane didSelect]` → `inject handler invoked` — osax loaded.
- `captured Trackpad pane` — got the pane instance (via didSelect navigation, or the proactive direct-open path).
- `armed live observers (USB + BT, first-match + terminated)` — our both-transport IOKit watch is up.
- `device change: BT-/USB- -> removal-check in 1300ms` — a transport went away (coalesced removal).
- `device change: BT+/USB+ -> show in 250ms` — a transport appeared (coalesced).
- `recompute: loadMainView (coalesced)` — the pane redraw. **Expect ~ONE per device event** (coalescing); a
  USB-appear supersedes a pending BT-removal so a handoff = a single redraw, no NoTrackpad.

### UI tells (score OK vs STALE — the human reports these)
- **BT UI** = battery level + "Change Batteries" + wireless-trackpad video.
- **USB UI** = built-in-laptop-trackpad video, NO battery UI.
- **NoTrackpad** = "No Trackpad" placeholder.
- STALE = wrong-transport UI vs device truth (e.g. BT battery UI while device truth says USB).

### Gotchas
- **Clean reset = fully QUIT System Prefs (Cmd-Q)**, not just close the window — the process (and our IOKit
  notification ports) persist otherwise.
- **Capture** only happens once the Trackpad pane is the current pane: click Trackpad after launch, OR (direct-
  open case) it reopens on Trackpad and the proactive path captures it. Opening on the grid = no capture yet
  (correct — waits for navigation).
- **Hard floor:** MT2 USB enumeration ~1.1s; removal settle is 1300ms — handoff vs. removal can't be
  distinguished faster (device limit, not a tunable).

## The matrix (run in order; pane behavior is history-dependent)
First: open System Preferences, click **Trackpad** → expect `captured Trackpad pane` + `armed live observers`.

| Row | Physical action | Expected markers | Expected UI | Status (2026-06-29, via watcher) |
|-----|-----------------|------------------|-------------|-----------------------------------|
| 1 | On **BT**, pane open, **cable USB in** | `BT- removal-check` superseded by `USB+ show` → ONE `loadMainView` | settles **USB**, no NoTrackpad (one cosmetic USB flash OK) | ✅ PASS |
| 2 | On **USB**, pane open, **unplug USB** | `USB- removal-check` → `loadMainView`; then tap to wake → `BT+ show` → `loadMainView` | brief NoTrackpad (BT idle) → **BT** | ✅ PASS |
| 1b | Repeat BT↔USB **switches in the same session** | still **1** `loadMainView` per event (after 5+ this session) | clean each time; flicker NOT worsening | ✅ PASS (no leak) |
| 3a | On **BT**, **power MT2 OFF** | `BT- removal-check` → `loadMainView` (no USB event from us) | → **NoTrackpad** (cosmetic: Apple's loadMainView transits the USB nib en route — not our logic) | ✅ PASS |
| 3b | **Power MT2 ON** | `BT+ show` → `loadMainView` | back to **BT** | ✅ PASS |
| 4a | On **USB cabled**, **power MT2 OFF** (cable still in) | `USB- removal-check` → `loadMainView` (AppleUSBMultitouchDriver terminates on power-off) | → **NoTrackpad** (NOT stale USB — corrects the old inference; no policy decision needed) | ✅ PASS |
| — | Capture-race: open System Prefs **directly on Trackpad** (e.g. `osascript ... reveal pane id "com.apple.preference.trackpad"` after a full Cmd-Q) | `proactive capture via currentPrefPaneInstance (already on Trackpad)` → `captured Trackpad pane` | refresh still works | ✅ PASS (proactive path) |

Record per-row PASS/FAIL + any new transient into `docs/mt-stack/decisions.md` (durable). The full
post-fix acceptance is in `decisions.md` → "Task C.2 FULL matrix"; the pre-fix baseline is below.

## SM-build re-validation (2026-07-01) — the matrix caught a real regression
Re-ran the entire matrix on the **state-machine** osax (the `mt2_pane_sm`-driven adapter, commit `555e62e`).
It exposed a regression the SM *unit* tests can't see — they cover the pure `psm_*` logic, which was correct;
the bug was in the adapter's render call. The replay invoked `_magicTrackpadAction` with the **controller as
the `arg`**, but the body does `[arg armIterators]`, and `armIterators` is owned by the **`IOServiceObserver`**,
not the controller (disasm `-[BaseTrackPadController _magicTrackpadAction:deviceConnected:]`@0x4c57: prologue
`r14=rdx=arg`; @0x4caf `mov %r14,%rdi; callq [%rdi armIterators]`). So the replay threw
`-[MTTrackpadController armIterators]: unrecognized selector` → an AppKit-swallowed NSException at capture and
a no-render ("zilch") on every transport change. **Fix** (commit `6231b53`): capture the real `(self, arg)`
pair in `my_magicAction` and replay faithfully — `gOrigMagicAction(gMagicCtrl, gMagicArg, connected)`, exactly
macOS's own call, so it can't throw; reverted the stopgap forced `loadMainView` (the faithful replay does the
in-place switch, no blink). After the fix **all rows pass**, log line `captured magic
(self=MTTrackpadController, arg=IOServiceObserver armIterators=1)`. **Lesson:** the render/suppression seam is
NOT host-testable — the on-device matrix is the oracle for it. (Also noticed + left as native Apple behavior:
the BT UI exposes a three-finger-drag toggle the USB UI doesn't — the faithful replay reproduces the full
per-transport render, so the difference is Apple's, not ours.)

## Full re-walk after the presence-SM unification (2026-07-10) — caught a real capture-race
The presence-SM unification (`46e8f09`) extracted the inline `sm_event`/`dev_changed`/`sm_reconcile`/
`arm_observer` into the shared `mt2_presence_observer`; it was committed "on-device validated" on a
BT→USB→BT happy path but the FULL matrix was never re-walked. Walking it exposed a **stale-video
capture-race** (NOT introduced by the unification — the observer is a verbatim, line-by-line-faithful
lift; latent since `6231b53` removed the forced-loadMainView fallback):

- **Root cause.** `perform(ON_BT/ON_USB)` does an in-place `_magicTrackpadAction` replay and calls
  `loadMainView` only on a view-TYPE change. The replay needs the `(self,arg)` pair, captured LAZILY by
  `my_magicAction` when the pane's own `_magicTrackpadAction` eventually fires. On a same-view BT↔USB
  switch (no NoTrackpad in between), `perform` fires on the presence edge BEFORE that capture lands →
  `skip replay` → the video/art stays on the outgoing transport. It rendered right only when the switch
  happened to pass through NoTrackpad (forcing `loadMainView`) or the capture happened to win — accidental.
- **Fix (`e730175`).** `eager_capture_magic()` reads the pair straight from the pane's ivars the moment a
  replay needs it (self = live `MTTrackpadController` via `find_mt_controller`; arg = its
  `mMagicTrackpadServiceObserver`), guarded by `respondsToSelector:armIterators`. No render, no
  `loadMainView` → no blink. Log at open: `eager-captured magic (self=MTTrackpadController,
  arg=IOServiceObserver)`.
- **Full matrix re-validated post-fix (all rows 0 skips):** fresh launch (no blink), BT↔USB both ways
  (immediate, no stale), 3a/3b/4a NoTrackpad, capture-race direct-open. The decisive repro — unplug USB
  with BT waking *inside* the 1300ms HOLD window (same-view, fresh pane) — now renders BT immediately.

**CORRECTION to the SM-build note above:** the "three-finger-drag toggle is on BT / off USB → the
difference is Apple's, not ours" reading is **WRONG**. 3FD shows on a FRESH launch on USB (the USB node
DOES publish `TrackpadThreeFingerDrag=true`) and drops only on a LIVE USB appearance — it's **our** race
(`_isServiceAvailable:@"TrackpadThreeFingerDrag"` runs before the just-appeared USB node is queryable),
tracked in `open-questions.md` "3FD-on-live-USB availability race". Two other pre-existing transition
cosmetics also surfaced (both Apple-level-UX, both in `open-questions.md`): the USB→BT NoTrackpad flash
(BT wake vs the HOLD window) and the BT power-on tap-to-stream latency.

**Dev-loop gotcha (cost us ~20 min):** the dev box loads the **SIMBL plugin copy** of the payload
(`/Library/Application Support/SIMBL/Plugins/MT2PaneRefresh.bundle`), which wins the single-load over the
`.osax`. After an osax change, reinstall **both** (`prefpane-refresh-install` + `prefpane-refresh-simbl-install`)
or the stale SIMBL copy keeps injecting. Also: cross-machine "mtime in the future" defeats make's rebuild
check — force a recompile (delete the target's `.o`, or reconfigure) or you reinstall a stale binary.

## Clean-room baseline (pre-fix, 2026-06-28) — what the BUG looked like
Characterized physically (clean-room: Cmd-Q System Prefs between every trial to clear accumulated process
state; device-truth via `re ioreg-class` alongside the user-reported pane). This is the "before" the fix
was measured against — keep it to prove future changes don't regress. Pane states: OK / NOTFOUND
("No trackpad found") / STALE (wrong-transport UI).

| Start | Action | device-truth | OPEN pane (the bug) | FRESH-opened pane |
|-------|--------|--------------|---------------------|-------------------|
| BT | cable USB in | USB present | **NOTFOUND** ⚠️ (device IS present → display bug) | OK (USB) — fresh query works |
| USB | unplug USB → BT | BT present | **OK** (live-updates to BT) — asymmetry vs above | n/a |
| BT | power OFF | absent | FLASHES USB UI briefly → NOTFOUND | — |
| USB | power OFF (cabled) | absent (driver) | STALE (USB UI persisted) — *note: post-fix this is now NoTrackpad* | — |

**Clean-room data summary (the decisive finding):** launch System Prefs **on USB** → the open pane tracks
BOTH transports live; launch **on BT** → a USB-appear shows USB UI for a MOMENT then reverts to NOTFOUND,
**every time, no "learning"** (an earlier "OK on 2nd plug" was a contamination artifact — the process stayed
alive across a window-reopen-on-USB which registered the USB observer). USB→BT always live-updates. A fresh
launch is always correct (one-shot presence check of both classes).

**Root cause (disasm-confirmed):** the pane's only LIVE observer is on `BNBTrackpadDevice` (BT, @0x232e/0x38af);
USB is a one-shot `IOServiceGetMatchingService("AppleUSBMultitouchDriver")` presence check (@0x23be), so a USB
change fires no callback — the brief flash is the BT-terminate path (cabling USB drops BT) transiently
re-detecting USB before forcing NoTrackpad. Full disasm + fix-target in `open-questions.md` → "Trackpad
prefpane live-update misses our manually-started driver". Our osax adds the missing USB observer + coalesced
recompute; the post-fix matrix above passes every row.

**Confounds to control when re-testing:** (C1) "close + reopen window" does NOT reset state — the System Prefs
*process* persists, so IOKit notification ports persist; clean reset = full Cmd-Q. (C2) which transport the
device is on at process-launch decides whether the USB observer ever registered (root of the bug). (C3)
having both transports physically present at once changes behavior (USB cabled underneath while on BT).

---

# Satellite/Voodoo validation pass (2026-07-20) — pane FORCE + battery + naming

**Context.** We returned to full-synthetic: a fabricated `AppleMultitouchDevice` under the VoodooInput mux, no
genuine Apple driver. Apple's `-[Trackpad loadMainView]` detect is CLASS-based (`BNBTrackpadDevice`/
`AppleUSBMultitouchDriver`) so it never sees our synthetic AMD → the pane showed NoTrackpad. The osax now
**forces** the trackpad view by swizzling `-[Trackpad _controlerForNIBName:]` to substitute
`"MTTrackpadController"` when our reader is present (`feat(osax): force the trackpad pane view…`). This pass
re-validates the whole matrix under that force. **The device-truth oracle CHANGED** — `BNBTrackpadDevice`/
`AppleUSBMultitouchDriver` no longer exist; truth = OUR reader classes + the fabricated AMD.

## Updated oracle (run per step; mark `LB=$(wc -l < /var/log/system.log)` BEFORE the physical action)
```sh
tools/re ioreg-class MT2BTReader MT2USBReader   # 1 = that transport bound (= truth)
tools/re ioreg-class AppleMultitouchDevice                             # our fabricated AMD (1 = present)
sudo sed -n "$((LB+1)),\$p" /var/log/system.log | grep -aE "MT2PaneRefresh|MT2Presence"
```
### New markers (satellite era)
- `reconcile bt=0 usb=1 -> action` / `device change: USB+/BT+` — presence observer saw the transport.
- `perform: ON_USB` / `perform: ON_BT` / `perform: ABSENT (NoTrackpad)` — the SM render action.
- `controlerForNIB: device present -> forcing MTTrackpadController` — **the new force**. Expect it EXACTLY when a
  device is present and the pane would otherwise pick NoTrackpad. It must NOT appear when no device is present.

## A. Pane-view matrix (the force)
| Start / action | device truth | expected pane | validated |
|----------------|--------------|---------------|-----------|
| USB present, fresh open | USB reader=1, AMD=1 | MTTrackpad view (Point&Click / Scroll&Zoom / More Gestures / About), demo video, battery "100% charged"; interactive | ✅ 2026-07-20 live |
| BT present, fresh open | BT reader=1, AMD=1 | MTTrackpad view; battery = live % | ⬜ needs device |
| present → **power OFF** | reader=0, AMD gone | HOLD ~1.3s (view kept) → **NoTrackpad** | ⬜ force MUST decline once `gObs==NONE` |
| **USB → BT** (unplug USB) | USB=0 then BT=1 | ONE coalesced redraw, stays MTTrackpad, content flips to BT | ⬜ promptness + correctness |
| **BT → USB** (cable USB) | BT=0 then USB=1 | ONE coalesced redraw, stays MTTrackpad, USB content | ⬜ |
| **genuinely no device** (none paired/plugged) | reader=0, AMD=0 | **NoTrackpad** (force declines — no `controlerForNIB` marker) | ⬜ **THE key regression guard** |

Correctness note: the observer sets `o->state` BEFORE the callback, so `perform(ABSENT)` runs with `gObs==NONE`
→ the force declines → NoTrackpad. The rows above verify that holds live, not just in theory.

## B. Battery
- USB: "battery level 100% (charged)" — charging inferred from `PRESENCE_USB`. ✅ 2026-07-20 live.
- BT: `BatteryPercent` off the mux's AMD node (kext `GET_REPORT 0x90` → `gBtMux->synthCtx()`). ⬜ needs device;
  confirm a number renders and the battery row shows.
- ⚠️ **always-100 caveat is PRE-Voodoo** (`battery-reporting.md`). If BT reads 100 at a known-drained level,
  that's the OLD unresolved field question, NOT a Voodoo regression — don't re-chase it here.

## C. Naming (BT only — USB `MaxFeatureReportSize==1`, no room; USB showing no name report is EXPECTED)
The write (`mt2_name_write_onboard`) is `IOHIDManager` → `SET_REPORT(Feature, 0x55)` straight to the physical BT
MT2 — pipeline-independent (never touched our AMD). **The at-risk precondition:** the satellite BT path excludes
Apple's `IOBluetoothHIDDriver`, which historically exposed the BT MT2 as the `IOHIDDevice` the write needs.
1. **Visibility first** (on BT): `tools/re mt2-name` → should list the MT2 + its `0x55` name. **Empty/absent =
   the risk is real** (no BT HID node under the satellite → the write can't find a target).
2. **Rename**: BT pane → right-click the MT2 → Rename → a new name. Expect a `name-mirror: SET_REPORT 0x55`
   SUCCESS line (not `error 0x…`).
3. **Persistence**: `tools/re mt2-name` shows the new name; ideally power-cycle / re-pair to confirm it FOLLOWED
   the device (the whole point of the on-device name).
- **FAIL mode + fix:** if `IOHIDManagerCopyDevices` returns nothing (no BT HID node), the `0x55` write silently
  no-ops → rename doesn't follow the device. Fix would be to expose the MT2 as an `IOHIDDevice` again, or route
  the `0x55` write through our BT reader's control channel instead of `IOHIDManager`.

**Backup pointer reminder:** powering the MT2 off (rows in A) while it's the only pointer strands the session —
keep a mouse handy. This whole pass is userspace-only (osax + reads); no kext load, no panic risk.
