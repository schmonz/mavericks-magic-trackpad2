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

Record per-row PASS/FAIL + any new transient into `docs/mt-stack/decisions.md` (durable) — the gitignored
`docs/superpowers/prefpane-transport-matrix.md` holds the raw clean-room event log.
