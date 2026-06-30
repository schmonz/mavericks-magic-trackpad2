# Decisions — big-ticket roads not taken

Why we *didn't* go certain ways, so we don't re-tread them. Two kinds:

- **Not a functioning choice** — a fact makes it impossible. Won't revisit.
- **Not a sufficiently desirable choice** — it works (or could), but lost on a constraint. The entry
  names the constraint, so it's clear what would reopen it.

For *open* unknowns (vs these resolved ones) see `open-questions.md`.

---

### Own / bare `IOHIDDevice` for the prefpane — *not functioning*
To light the prefpane we tried publishing our own `IOHIDDevice` of the matched class. Publishing an
**un-started** `IOHIDDevice` null-derefs in `_publishDeviceNotificationHandler` → kernel panic. You
can't `registerService()` a bare nub of that class. (Led to: manually start a *genuine*
`BNBTrackpadDevice` instead, which is a real started IOHIDDevice.)

### REPLACE — drop genuine BNB, own the device outright — *not desirable*
A clean reimplementation that owns the BT channels + input was viable, but the user pinned **stock
Apple prefpane = mandatory**, and the pane matches `BNBTrackpadDevice`. Dropping genuine BNB loses the
pane. **Reopening criterion:** if "stock pane mandatory" were relaxed (e.g. an own-pane workstream
shipped), REPLACE becomes viable again.

### VID/PID match path (route-2 matching) — *not functioning*
We hoped to get matched onto the device by advertising the MT2's IDs. The matcher reads the **real
DID from the controller-side store**, not from our personality, so we can't win matching that way.
Manual-start is the only route.

### Seam C — wire injection (intercept HIDP GET_REPORT on PSM 17) — *not functioning / not desirable*
Premise was that geometry crosses the L2CAP wire and we could fabricate the response. It doesn't —
the geometry query terminates at in-kernel transport stubs, never hitting the wire. Even if it could
work, the in-kernel vtable override is simpler.

### Single-slot geometry override (`0xcc8` only) — *not functioning as designed*
Overriding only `getMultitouchReport` (`0xcc8`) never fired: `_deviceGetReportWithLookUp` probes the
**LENGTH via `getMultitouchReportInfo` (`0xcd8`) FIRST**, which hit the stub and short-circuited
before the data fetch. Proven on-device (override live, `MATCH`, but never called). **Fix that
shipped:** override *both* `0xcd8` and `0xcc8`. Don't re-try single-slot.

### Late / passive geometry publish — *not functioning*
Installing a geometry handler *after* the AMD started, or late `setProperty` of the geometry keys,
had no effect: userspace `MultitouchSupport` caches geometry at **first attach**, and the AMD is born
(empty) before it's reachable at `transport+0x1b0`. The publish must be correct *before* `AMD::start`
— hence the vtable override installed before `bnb->start()`.

### `IOMatchCategory` to coexist with `IOBluetoothHIDDriver` — *not functioning*
Trying to coexist (instead of exclude) so PSM 19 would open reliably: it opened, but the HID driver
then grabbed the interrupt-channel `listenAt` delegate (single-owner) → our listener rejected → no
frames, and the HID driver still didn't drive a cursor. Worst of both; reverted.

### `waitQuiet` on manual-BNB teardown — *not a functioning choice* (removed 2026-06-23)
Added 2026-06-21 as a "synchronous teardown" to drain BNB before `release()` (hoping to fix the §S2.9
unload wedge / §S2.14 reconnect flap). On-device probe (`getBusyState` before/after, 8s bound):
the manual `BNBTrackpadDevice` is `busyState=1` *before* `terminate()` and **still `busy=1` after a
full 8001 ms** wait; its AMD child is `busy=0`. So the busy is BNB's own — its genuine connect
lifecycle never completes in our hybrid flow (`deviceReady` never reached; the 5s "Forcing MT restart"
watchdog cycles), so the start-time busy is never balanced and `waitQuiet` can **never** succeed; it
only stalled every disconnect for the full bound. Removed → plain `terminate()` + `release()`. Unload
safety rests on the in-gate delegate/vtable restores, not on quiescence (no panic; reconnects work).
**Reopening criterion:** if the full `waitForChannelState(OPEN)` handshake fix lands and BNB reaches
`deviceReady` genuinely, it should settle to `busy=0` — a real synchronous teardown becomes possible
then; re-check.

### Hybrid `+0x1b0` poke — *not desirable (stability)*
The hybrid architecture routed prefpane settings by poking BNB's handler slot (`+0x1b0`) to our
fDevice. It worked (controls applied) but its teardown drove genuine HID teardown against a
half-restored object → the `ultimate-hat.panic`. Motivated the move to full-BNB, which deletes the
poke (BNB's node *is* the input *is* the pane target). **Note:** the *hybrid architecture itself*
remains a live alternative for a livability comparison; only this *mechanism* is graveyarded.

### `OSBundleLibraries` to force-load the Apple driver kexts — *not functioning for BT → uniform script-load*
The genuine paths manual-start `BNBTrackpadDevice` (BT) and `AppleUSBMultitouchDriver` (USB) via
`allocClassWithName`, which needs the owning Apple kext (`AppleBluetoothMultitouch.kext` /
`AppleUSBMultitouch.kext`) loaded so the class is registered. On a clean boot they are NOT loaded in
time → `allocClassWithName` returns NULL → the genuine path is dead with **no fallback** (proven
2026-06-24: BT had no cursor / no BT prefpane bits until `kextload AppleBluetoothMultitouch.kext` +
reconnect). The IOKit-idiomatic fix is to declare those kexts in our `OSBundleLibraries` so `kextload`
pulls them in as dependencies — which is exactly how we already force-load
`com.apple.driver.AppleMultitouchDriver`. **It does NOT work for BT:** `AppleBluetoothMultitouch.kext`
exports **no `OSBundleCompatibleVersion`**, and a bundle with no compatible version cannot be an
`OSBundleLibraries` dependency (nothing to link/version-match against). `AppleUSBMultitouch.kext` *does*
export `OSBundleCompatibleVersion=1.0.0`, so USB *alone* could use it — but we deliberately **don't**,
to avoid splitting mechanisms (Info.plist dep for USB, script for BT). Instead BOTH are loaded
best-effort by `kextload` in the boot wrapper (`dist/mt2d-run`) and dev `make load`, before our kext.
Two reasons beyond uniformity: (1) **soft** (`|| true`) — our kext still loads and the other transport
still works if an Apple kext is absent, whereas a hard `OSBundleLibraries` dep would block our *entire*
kext (losing BOTH transports) on a missing/incompatible Apple dep; (2) one mechanism is simpler.
**Reopening criterion:** move USB to `OSBundleLibraries` only if a deploy path loads our kext outside
the wrapper/Makefile (it has the compatible version); BT can't until Apple's kext gains one.

### Run VoodooInput on 10.9 / become a VoodooInput plugin — *not functioning / wrong direction*
(Researched 2026-06-24, web.) The obvious "why not just use the Hackintosh kext?" question, answered
so we don't re-tread it. **First, the landscape:** there is **no public Apple API for the valuable
half** (device → gesture engine) on *any* macOS version. The public device path is the HID **digitizer**
(modern: HIDDriverKit `IOUserHIDEventService::dispatchDigitizerTouchEvent`; usage page `0x0D`,
Contact Identifier `0x51`), but third parties report stock digitizer descriptors yield *no cursor / no
gestures* and provider-binding is **built-in-only** gated — the same wall as our [[builtIn]] lens. The
gesture engine (`MultitouchSupport`) stays private everywhere; the only public gesture API is
app-level `NSGestureRecognizer`. So there's nothing of Apple's to be ABI-compatible *with*.

**VoodooInput** (acidanthera) is the community analog and was this project's original *reference* — but
it is the **inverse** of us: `VoodooInputSimulatorDevice` **fabricates a fake MT2** so IOKit *matching*
binds Apple's native MT HID driver to a virtual nub; clients (VoodooPS2/I2C) translate *non-Apple*
hardware into MT2 frames. It has **zero code to drive a real Magic Trackpad** — the "read a real MT2
over USB/BT" front-half is uniquely ours. Three blockers kill "just run it on 10.9": (1) it's a
modern-toolchain kext — loading on the 10.9 kernel is the exact unsolved build problem; (2) it assumes
clean IOKit **matching** to a modern stack, the very path we had to replace with manual-start + interpose
+ built-in-gating dodges; (3) it's **redundant for a real MT2** (already an MT2 — we need 10.9 to *wire
up* the real device, which VoodooInput doesn't do). **Reopening / convergence criterion:** the right
collaboration is inverted — be the **10.9 back-port that speaks VoodooInput's contact interface**
(`VoodooInputEvent{contact_count,timestamp,transducers[]}` + `VoodooInputTransducer{type,id,fingerType,
isValid,isPhysicalButtonDown,supportsPressure,maxPressure, Touch{x,y,pressure,width} current/previous}`,
IOKit msg `kIOMessageVoodooInputMessage`=12345), so one device targets both eras. Separately worth a look
(open question, not decided): VoodooInput's normal **publish-a-nub-and-let-IOKit-match** is cleaner than
our manual-start — does it work on 10.9? (Has teeth: catalogue residue, built-in gating, the un-started
`IOHIDDevice` panic above.)

### Override the cached `ClassOfDevice` to fix the Bluetooth-pane picture — *not functioning*
The pane's device picture is chosen from the device's Class-of-Device via `IOBluetoothDeviceImageVault`
(see explanation.md "Bluetooth prefpane device identity"). We hoped to override the MT2's CoD in
`blued`'s cache so it maps to the trackpad vault entry. **Doesn't stick:** set `DeviceCache[<addr>]
.ClassOfDevice` `1428`→`9600`, restarted `blued`; on the MT2's reconnect `blued` **re-fetched the CoD
from the live device and overwrote it back to `1428`** (`LastNameUpdate` advanced; file reverted). So
CoD is live-sourced every connect, and there is no per-device image override key (unlike `displayName`
for the name). **Reopening criterion:** none via the cache — the only ways to change the picture are
hooking/patching `IOBluetoothUI` in the BT-UI processes (System Prefs / `BluetoothUIServer` / menu
extra) or a binary patch of the framework (feasible only because 10.9 has no SIP). Cosmetic; deferred.
Contrast: the **name** has the `displayName` user-override key → clean, persistent fix (do that).

### CMake-built MT2Gesture.kext validated on-device — parity with the Makefile build (2026-06-29)
Project A (CMake/ctest migration) replaced the Makefile build alongside the existing one in `cmake-build/`.
Before retiring the Makefiles we proved parity of the load-bearing artifact: the CMake kext binary has the
**same 594 symbols (identical type+name set)** as the Makefile kext (`nm` name+type diff empty; only
addresses differ from link order), both `MH_KEXTBUNDLE` x86_64 `NOUNDEFS`. Then the real gate: unloaded the
running kext, staged the CMake kext root-owned in `/tmp`, `kextload`ed it (resolved deps `<120 88 37 30 5 4
3 1>`, nub `com_schmonz_MT2Gesture` registered/matched/active/busy 0), and the user exercised the full
gesture set — **confirmed working on both USB and BT**. The CMake kext is behavior-equivalent to the
Makefile build. (The installed boot copy under `/usr/local/lib/mt2d` was untouched; this was a runtime swap.)

### Scripting Addition loading on 10.9 — how a .osax gets into System Preferences (Phase 0 research, 2026-06-29)
Research for the standalone-osax delivery (replace SIMBL). Authoritative sources: Apple **QA1070** "Loading
Scripting Additions in Mac OS X", Apple **TN1164**, and the EasySIMBL `SIMBLAgent.m` source.

**Finding 1 — additions load ON DEMAND, never automatically at launch.** On OS X each app has its OWN system
handler table (not shared); "for performance reasons, this isn't done automatically as a normal part of the
application initialization sequence — it is only done if an application uses AppleScript or initializes the
table itself" (QA1070/TN1164). The AppleScript scripting component, when it first comes into existence in a
process, loads every `.osax` from `/System/Library/ScriptingAdditions` + `/Library/...` + `~/Library/...`.
⇒ A plain osax install with **no trigger will NOT auto-load** into System Preferences (it doesn't use
AppleScript at launch). This is the Task 0.2-Step-2 expectation, now backed by Apple docs (still verify on-box).

**Finding 2 — the documented force-load event is `ascr`/`gdut`** (`kASAppleScriptSuite`/`kGetAEUT`,
"get dynamic user terminology"). QA1070 gives the exact recipe: `AEBuildAppleEvent(kASAppleScriptSuite,
kGetAEUT, …); AESend(&e,&r,kAEWaitReply,…)`. On 10.2+ (so 10.9) the system installs the receiving handler
automatically, so an EXTERNAL process can send `ascr`/`gdut` to a target pid to make it load ALL scripting
additions. This is the sanctioned, reliable trigger.

**Finding 3 — our payload does its work in a CONSTRUCTOR, so LOADING is sufficient.**
`tools/mt2_prefpane_refresh/mt2_prefpane_refresh.c:245` has `__attribute__((constructor))` (plus a belt-and-
suspenders `MT2InjectHandler` for `MT2x`/`load`). The SIMBL bundle already works precisely because its
constructor fires on `[bundle load]`. ⇒ Once the **.osax is loaded** (by `ascr`/`gdut`), the constructor runs
and does the work — we do NOT need our custom `MT2x`/`load` event to be received.

**Finding 4 — explains the prior spike failure.** The earlier note "the .osax/AppleEvent path does not
reliably load into Cocoa System Preferences" used `mt2_pane_arm` sending our CUSTOM `MT2x`/`load`. That is
NOT a force-load event — if the osax isn't loaded yet, System Prefs has no handler for it and (unlike
`ascr`/`gdut`) an arbitrary custom event is not guaranteed to instantiate the AppleScript component. So the
fix to try is: send **`ascr`/`gdut`** (force-load) instead of/before `MT2x`/`load`.

**Finding 5 — timing.** EasySIMBL's `SIMBLAgent` injects on the app's **`isFinishedLaunching`** (KVO), not on
the raw `NSWorkspaceDidLaunchApplicationNotification`. Branch A's watcher should prefer `isFinishedLaunching`
(or retry) so it doesn't fire before the AE machinery is ready.

**⇒ Single most-likely agentless trigger to try in Task 0.2 Step 3:** send **`ascr`/`gdut`** to the running
System Preferences pid (one-shot: a tiny AESend sender, or adjust `mt2_pane_arm` to this event class/id) and
check whether `MT2PaneRefresh` maps + its constructor logs. NOTE even if this loads the osax, the event must
be re-sent on each System Prefs launch ⇒ a persistent watcher (Branch A) is still likely required for the
shipped product; true zero-trigger Branch B only wins if System Prefs can be made to load additions itself
(unlikely). Sources: developer.apple.com/library/archive/qa/qa1070, .../technotes/tn1164,
github.com/norio-nomura/EasySIMBL SIMBLAgent.m.

### Phase 0 ON-DEVICE result — the standalone osax mechanism WORKS (no SIMBL); Branch A (2026-06-29)
Ran Task 0.2/0.3 on the 10.9 box (SIMBLAgent stopped to remove it as a confound; our SIMBL plugin removed;
other users' SIMBL plugins + SIMBLAgent left for restore). Findings, each from live syslog + `lsof`:

- **Step 2 — agentless zero-trigger auto-load is DEAD (confirmed on-device).** Install osax, no agent/trigger,
  fresh System Prefs launch → our osax NOT mapped, no constructor log. Matches QA1070 (additions load on demand
  only).
- **`gdut` is a RED HERRING for us.** Sending `ascr`/`gdut` to System Prefs DID initialize the OSA machinery and
  eagerly loaded `/System/Library/ScriptingAdditions/SIMBL.osax` (which carries an `OSAScriptingDefinition`
  .sdef), but loaded NONE of the three `/Library/ScriptingAdditions` osaxen and NOT ours. `gdut` only eagerly
  loads *terminology-bearing* additions; ours declares only `OSAXHandlers` (event `MT2x`/`load`), so `gdut`
  never loads it.
- **Our OWN event `MT2x`/`load` loads it, reliably, ALONE (no gdut needed).** `mt2_pane_arm <pid>` → osax mapped
  + `[MT2PaneRefresh] image loaded → swizzled -[NSPreferencePane didSelect] → inject handler invoked`. The OSA
  dispatcher finds our `OSAXHandlers` registration and loads the osax to invoke `MT2InjectHandler`; the payload's
  constructor runs on load.
- **Location does NOT matter for event-dispatch loading: `/Library` works as well as `/System/Library`.** Proven
  by loading our osax from each (fresh process, `MT2x`/`load` only). Both ScriptingAdditions dirs are scanned for
  `OSAXHandlers` registration. ⇒ install to **`/Library/ScriptingAdditions`** (less invasive; don't touch
  Apple's `/System/Library`; clean uninstall). The plan's original `/Library` install location was RIGHT.
- **Explains the prior spike's "the .osax/AppleEvent path does not reliably load" note:** NOT a location problem
  (location is fine) and NOT a force-load-event problem (`gdut` unneeded). The residual variable is TIMING /
  targeting — sending `MT2x`/`load` to the running pid at the right moment (here: app already launched, sent by
  pid) works every time across 3 fresh launches. SIMBLAgent times injection on `isFinishedLaunching`.

**DECISION GATE (Task 0.3): AGENTLESS DEAD → Branch A** (a launch-watcher that sends `MT2x`/`load`). BUT the
trigger contract is now KNOWN EMPIRICALLY (`MT2x`/`load` to the System Prefs pid, osax in `/Library`), so
**Task 0.4 (RE SIMBLAgent for the trigger) is UNNECESSARY** — skip it. Branch A reduces to: a per-user
LaunchAgent that, on System Prefs launch (prefer `isFinishedLaunching`), sends `MT2x`/`load` to the pid — i.e.
`mt2_pane_arm` wrapped in a watcher. No SIMBL, no SIMBLAgent.

### osax ↔ SIMBL coexistence — both load → DOUBLE-swizzle (installer must make them exclusive) (2026-06-29)
Tested on-device whether the standalone osax still loads from its ship location (`/Library/ScriptingAdditions`)
while SIMBL is active. Result: **YES it loads** — SIMBL does not block or preempt the osax path. BUT with BOTH
the SIMBL plugin (`/Library/Application Support/SIMBL/Plugins/MT2PaneRefresh.bundle`) AND the osax installed,
the SAME payload loads TWICE in one System Prefs process: SIMBLAgent auto-injects the `.bundle` at launch
(`image loaded → swizzled didSelect`), then our `MT2x`/`load` loads the `.osax` (`image loaded → swizzled
didSelect → inject handler invoked`). The two are independent images, so `-[NSPreferencePane didSelect]` gets
**swizzled twice** + two transport observers — a real double-injection hazard.

⇒ The osax and the SIMBL plugin MUST be mutually exclusive. The installer's `postinstall` already removes the
SIMBL plugin when installing the osax (plan Task C.1 Step 3) — this finding is WHY that step is load-bearing,
not just tidy. (The dev box keeps SIMBL for now; the shipped pkg drops it.) No payload change needed; the
constructor isn't idempotent across two images and shouldn't have to be — exclusivity is the contract.

### CORRECTION — no transition; coexist with unrelated SIMBL; never ship/remove a SIMBL plugin (2026-06-29)
Reframing the prior "double-swizzle / installer removes SIMBL plugin" note (it assumed a SIMBL→osax
transition — wrong). Reality (user): nobody uses this yet, so there is NO transition; but **people run SIMBL
for OTHER, unrelated plugins**, and our osax+watcher must **coexist** with that and never disturb it.

The shipped product installs the **osax + watcher ONLY — it does NOT ship a SIMBL plugin** (`MT2PaneRefresh.bundle`
is a dev-only loader). So the double-load I saw earlier was a DEV artifact (dev box had both our SIMBL plugin
AND the osax). Proven on-device (shipped simulation: SIMBL active for the 7 other plugins, our osax in
`/Library`, NO MT2PaneRefresh SIMBL plugin): at System Prefs launch our payload does NOT load (0 markers —
SIMBL.osax maps but doesn't touch our osax, different event), and our `MT2x`/`load` then loads it **exactly
once** (single `swizzled didSelect`). The 7 other plugins + SIMBLAgent are untouched.

⇒ Installer rules: install osax + watcher; **do NOT install a SIMBL plugin; do NOT remove or modify SIMBL,
SIMBLAgent, or any other SIMBL plugin.** (Supersedes the earlier "postinstall removes the SIMBL plugin"
guidance for the SHIPPED pkg — that removal is only meaningful on a DEV box that installed our own dev SIMBL
plugin, and even then must be scoped to OUR `MT2PaneRefresh.bundle` and nothing else.)

### Task C.2 acceptance — live BT→USB refresh works through the standalone watcher loader (2026-06-29)
The headline case validated end-to-end through the NEW loader (watcher LaunchAgent + osax, no SIMBL), user
driving the physical transport switch + reporting the screen, agent reading syslog + device truth:

**Sequence:** MT2 on BT (unplugged USB; cursor + gestures confirmed) → opened System Preferences → Trackpad
(normal BT UI) → plugged USB. **Result (user-observed): the pane settles on USB, NO "No Trackpad"** (one extra
USB flash — the known cosmetic double-render, SECONDARY/non-blocking). Syslog corroborates the full chain:
`[mt2panewatch] injected pid 47119 (try 1)` → `captured Trackpad pane` → `armed live observers (USB + BT)` →
`device change: BT- → removal-check in 1300ms` → `device change: USB+ → show in 250ms` →
`recompute: loadMainView (coalesced)`. Device truth after: BNBTrackpadDevice 0 / AppleUSBMultitouchDriver 1
(USB), matching the settled pane. The USB-appear superseded the BT-removal → ONE coalesced loadMainView → no
NoTrackpad — exactly the designed behavior, now proven to load via our `MT2x`/`load` watcher instead of SIMBL.

⇒ The standalone-osax delivery (Phase 0 + Branch A + packaging) is functionally COMPLETE and accepted for the
headline case. Remaining: the cosmetic single-flash polish (SECONDARY, tracked in
[[mt2-prefpane-osax-injection-mechanism]]) and the proactive capture-race path (open directly on Trackpad) if
not yet re-confirmed under the watcher.

### Task C.2 FULL matrix — every row validated through the standalone watcher loader (2026-06-29)
Re-ran the whole prefpane transport matrix on-device through the watcher+osax loader (no SIMBL), human driving
the physical transport actions, agent corroborating via syslog markers + `tools/re ioreg-class` device truth.
Procedure captured durably in `docs/mt-stack/prefpane-test-runthrough.md`. All rows PASS:
- **Row 1 (BT→USB, launched-on-BT):** settles USB, no NoTrackpad. `BT- removal-check` superseded by `USB+
  show` → one coalesced `loadMainView`. (one cosmetic USB flash)
- **Row 2 (USB→BT):** `USB- → loadMainView` (brief NoTrackpad while BT idle) → tap → `BT+ → loadMainView` → BT.
- **Row 1b (2nd+ BT→USB same session):** still ONE coalesced `loadMainView` per event after 5+ this session —
  the feared observer-accumulation leak does NOT manifest (our coalescing dedups). Flicker not worsening.
- **Rows 3a/3b (BT power-cycle, previously only INFERRED):** OFF → `BT- → loadMainView` → NoTrackpad; ON →
  `BT+ → loadMainView` → BT. Our logic clean (no USB event from us). NEW: the OFF transition cosmetically
  transits the USB nib en route to NoTrackpad — Apple's `loadMainView` rebuild, not our observer (same family
  as the known SECONDARY flicker).
- **Row 4a (power-off while cabled USB) — CORRECTS the old inference:** the matrix guessed "stays USB (stale)";
  actual = `USB- → loadMainView` → **NoTrackpad**. The `AppleUSBMultitouchDriver` terminates on power-off even
  with the cable in, so our observer fires and the pane is correct. No policy decision needed.
- **Capture-race (open directly on Trackpad):** reproduced via `osascript reveal pane id
  com.apple.preference.trackpad` after a full Cmd-Q; the `didSelect` swizzle is missed but the **proactive
  `currentPrefPaneInstance` path captures it** (`proactive capture via currentPrefPaneInstance (already on
  Trackpad)`). Works through the watcher.

⇒ The standalone-osax prefpane delivery (Phase 0 + Branch A + packaging + acceptance) is COMPLETE and accepted
across the full matrix. Only-open item is the cosmetic single-flash on a redraw (SECONDARY; loadMainView's own
rebuild; tracked in [[mt2-prefpane-osax-injection-mechanism]]). The Project-A/CMake commits + this prefpane
work are all on `main` (unpushed).
