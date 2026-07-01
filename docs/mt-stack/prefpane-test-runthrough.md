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
