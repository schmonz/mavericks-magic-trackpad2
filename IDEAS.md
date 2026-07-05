Can we avoid needing a dummy click to connect BT when the way we got on BT is to unplug USB?

Can we replicate modern-macOS USB out-of-band BT pairing? (plug in once -> auto-paired -> unplug to go wireless.) CONFIRMED real: Mac generates a random link key, reads the device BT addr + sends link key & host BT addr to the device over USB HID (SkySafe CVE-2024-0230). Feasibility for us: USB read/write side is easy (MT2USBReader already does control transfers; BT MAC also in USB serial string); exact link-key report IDs/bytes are undocumented RE work (capture a real pairing or find the PoC); HARD part = injecting the link key into Mavericks's BT stack (com.apple.Bluetooth.plist LinkKeys + blued, or IOBluetooth). This is CORE onboarding for a public driver aiming for Apple-level UX: every new user pairs first, and "plug in once, done" is the Apple experience vs. "manually pair over BT then dummy-click". (Not low-value — I mis-framed that from the dev's already-paired machine.) Cheap first step: just read the device BT addr over USB (zero-risk, proves access).

Can we replace the prefpane MT1 assets at runtime?

For each of our userland tools, is it necessary for our purposes or is it belt-and-suspenders we can stop needing?

NAME (decided 2026-06-17): **Mavericks Trackpad 2** — the "Magic"->"Mavericks" substitution on the real product name; descriptive (conveys what+which-OS) with a subtle pun, no "tragic"=broken downside, sidesteps the "Magic" trademark. Tagline: "your Magic Trackpad 2, fully native on OS X Mavericks (USB + Bluetooth)." Slugs: repo `mavericks-trackpad-2`, `MavericksTrackpad2.kext`, bundle id `com.schmonz.MavericksTrackpad2`. FOLLOW-UP (on-console, deploy-affecting, NOT yet done): migrate the `mt2d` artifacts — `com.schmonz.mt2d` launchd label, `mt2d-run`, `/usr/local/lib/mt2d`, `/var/db/mt2d-boot.state`, `MT2Gesture` kext id, README, pkg id — to the new name.

Is the README short and sweet?

Can we build a .pkg on GitHub Actions from a modern macOS that produces byte-identical (or close-enough) binaries to local ones?

Are our functions well named and factored so I, a very naive reader in this domain, can understand how it all works?

Are our well-named and -factored functions all under exhaustive test, so their behavior can only be changed on purpose?

Is there anything else in our repo that we no longer need, now that we have trustworthy running code and a good understanding of it?

[NEXT PROJECT — DECIDED 2026-07-02] Extract the reverse-engineering LAB to its own sibling repo (e.g. `mt2-re` or a general `macos-re-lab`). `tools/` currently fuses ~6 SHIPPED helpers (mt2_reenumerate, mt2_set_btname, mt2_pane_watch, MT2PaneRefresh payloads, mt2_pane_arm) with a large dev-only RE lab: the `re` toolkit + ~20 probes/spikes (click_monitor, feed_probe, mt_contacts/cursorpos/transport, kc_carve, kc_lzss, macho_rebase, mt2_*_probe, …) + `tests/test_re.bats`. The lab is target-specific (needs 10.9 frameworks + a live device), never shipped, and un-CI-able on modern macOS — it's what forced the `test_re_bats` exclusion in the macos-26 release workflow. Coupling to the product is DOCUMENTATION only (~37 `re/` references across docs/mt-stack as the canonical fact-re-derivation method for src/mt2_stack.h), not build/runtime. Plan: own brainstorm→plan pass; git-move the lab, and rework the docs/mt-stack references so fact re-derivation still resolves (sibling repo the knowledge base points at, or a submodule). Do it AFTER the macos26-crossbuild pipeline lands. Relates to the "well organized?" and "tools necessary or belt-and-suspenders?" items above.

Are the remaining contents of the repo well organized?

Are we using CMake, my default choice for C projects, even though we might not need much from it for this project?

Are our tests hooked into CMake?

Are there any bits of code left that we're no longer sure are needed? Or that we're sure are no longer needed?

Any bits of code that if we changed them in any way, something would break (or crash, or be at risk of crashing) _and_ we don't have a test that would turn red first?

How can we be sure now that our code will never leak memory, use after free, or otherwise crash or cause instability? How can we adjust our build and release process so that it stays this way?

Can we (maybe not in this repo) implement the cool accessibility feature where wiggling the cursor real fast makes it bigger so I can find it? (Or does Mavericks have this already?)

[PRIORITY — DESIGN DONE: 2026-06-17-battery-design.md] Battery reporting: read MT2 battery + surface it. Mavericks shows battery for recognized Apple BT input devices in the BT menu-bar extra + the Mouse/Trackpad pref pane. Battery is a STANDARD HID battery usage (Linux fixes up the vendor page `06 00 ff / 09 0b` -> `05 01 / 09 02` at rdesc[46]==0x84 & rdesc[58]==0x85; report id is descriptor-defined). Needs a one-time on-console descriptor dump for the report id; then expose via the standard property. Rides with the native-identity/BNBDevice-equivalence work. See 2026-06-17-battery-haptics-findings.md.

[FAR DOWN] Haptics: drive the Taptic actuator so SYNTHETIC clicks (tap-to-click, two-finger right-click) get tactile feel (Mavericks predates Taptic/Force-Touch, no OS support). USB actuation format (Windows fork): SET_REPORT bRequest 9, wIndex=interface 2, wValue 0x0322 click / 0x0323 release, payload {0x22/0x23,01,00,78,02,00,24,30...}, strength in bytes [2][5][10]. Linux has none. Only worth it if the device doesn't self-actuate physical clicks (verify on-console). See findings doc.

[SCOPE — oldest supported OS] HARD FLOOR = OS X 10.6.4 Snow Leopard: that's where Apple's external-trackpad gesture engine (AppleMultitouchDriver + MultitouchHID.plugin) — the thing we hijack by feeding AppleMultitouchDevice — first shipped (orig Magic Trackpad launch update, 2010). Below it: no host gesture stack to feed (reimplementing gestures = out of scope). Useful range = 10.6.4 -> 10.10 Yosemite (10.11+ has NATIVE MT2, driver unneeded). 10.9 is the lead platform. CATCH: each older version is a SEPARATE PORT — MT2 USB/BT decode is OS-independent (ports free), but the hijack (IsFake bypass / allocClassWithName / property names / createMultitouchHandler config / MTSlideGesture offsets) is 10.9.5-tuned + the kext needs a per-version KPI build. The technique's real floor could be >10.6.4 if the hijack points don't exist in older AppleMultitouchDriver builds — confirm per-version via RE.

[FLOOR — lower it?] The 2026-07-02 release-pipeline design sets a HARD install floor of **10.9.5** (see docs/superpowers/specs/2026-07-02-macos26-crossbuild-pipeline-design.md §2a). It's a SAFETY gate, not a KPI limit: the kernel headers we compile against are byte-identical between XNU 10.9.0 (`xnu-2422.1.72`) and 10.9.5 (`xnu-2422.115.4`), so KPI-wise the kext could load on any 10.9.x — but the hijack (AppleMultitouchDriver `193.8`, MultitouchHID.plugin `MTSlideGesture` offsets, createMultitouchHandler config) is 10.9.5-tuned and those are CLOSED source (not in XNU), so we can't confirm 10.9.0–10.9.4 without hardware. TODO: **RE (or just test on) a real 10.9.0 system and see if we just work there** — compare its AppleMultitouchDriver / MultitouchHID.plugin offsets to 10.9.5; if the hijack points match, drop the KPI declaration to `13.0.0` + lower the pkg `allowed-os-versions` min to `10.9.0` and widen the floor. Relates to the per-version-port CATCH in the [SCOPE — oldest supported OS] item above.

[UX papercut + clean-fix lead, from MacRumors RE comment] Right-edge ~2cm dead zone: MT1 AND MT2 are less responsive in a ~2cm right-edge strip (the Notification-Center-swipe reserve); 10.9 has a BUG where it persists even with NC gesture OFF. It's a conditional in `MTSlideGesture::isBlocked` (jump at 0xf8a4 in MultitouchHID.plugin on 10.9.5) in a basic block reached ONLY for 1-finger pointer events under BLUETOOTH transport. Since we synthesize the AppleMultitouchDevice and set its Transport, reporting NON-BT transport may avoid the buggy block entirely — no system-file patch. VERIFY in the BNBDevice-equivalence RE spike (does non-BT transport avoid it without regressing other gestures?). Binary-patching the plugin is the fallback (invasive, version-specific offset — avoid for public release). NOTE: the commenter offered polished multitouch-stack exploration notes (MTSlideGesture etc.) — WORTH GETTING; gold for BNBDevice-equivalence + tap-to-click.

[MAYBE] Simulate Force Touch: 10.9 has no Force-Touch OS support, so we can't invoke the native 10.11+ behaviors. But we HAVE the pressure field (0..300) + the haptic actuation report. Could detect a deep-press (2nd pressure threshold, like real FT's 2nd detent) -> fire a haptic bump -> map to a legacy-supported action (best: synthesize 3-finger-tap = pre-FT "Look Up"/data-detectors; or a configurable shortcut). Combines pressure+haptics+gesture-synthesis. Niche; the triggerable action is the limiter.

[RESEARCH/DESIGN — for the 97% interface] Did Apple ship a similar "describe-a-multitouch-device" interface in 10.11+ (when MT2 became native)? If there's an analogous Apple-blessed interface/driver-personality shape, decide whether our engine should be MODELED ON it, or even ABI/source-compatible with it (so a device config or driver written for ours could move to/from Apple's, or vice versa). No thoughts yet — just capture: investigate 10.11+ AppleMultitouch* / HID multitouch interfaces and compare to our engine's device-config + feed-seam shape. Relates to [[mt2-mission-interface-over-driver]] and the engine carve-out [[mt2-refactor-to-explainability]].

[POLISH — once genuine-USB works] The genuine-USB reframe needs its OWN per-finger lifecycle state-tracking (prev-ids + last-position, to synthesize MakeTouch/Touching/BreakTouch the recognizer needs). The BT path has the same in `mt2_lifecycle`. Once everything works, try to NOT need any state-tracking of our own — find a way to feed the recognizer the lifecycle without us remembering prior frames (e.g. derive it from the device's own signal, or reuse one shared lifecycle component instead of two). Liking-the-code goal, not a correctness one. Relates to [[mt2-refactor-to-explainability]].

[POLISH] Architecture and flow diagrams (the unified two-transport diagram drafted in chat 2026-06-17 — save to docs/, plus a per-feature flow).

[POLISH] Demo videos/animations (show gestures working over USB + BT; before/after; tap-to-click; battery in the menu).

[RESEARCH] Get the MacRumors commenter (f54da)'s polished multitouch-stack exploration notes (MTSlideGesture, gesture internals) — they offered; gold for BNBDevice-equivalence + tap-to-click.

[RESEARCH — IN PROGRESS] Mine EVERY post/code snippet/reference from the MacRumors thread https://forums.macrumors.com/threads/magic-trackpad-2-on-10-9-or-lower.2332288/ to make this as good as it can be. Findings -> 2026-06-17-macrumors-thread-findings.md.

[RESEARCH — thread page 2, 2026-07-03] iphone2g&3gfan is building a VoodooInput-backed Wellspring trackpad driver: subclasses AppleUSBMultitouchDriver (not IOHIDDevice) to pass Apple's conformance checks; handles the 0x28 packet format + the 0x74 Wellspring format. Says "the logic that plumbs wellspring frames upwards should be reusable and could be used to get MT2 even lower than Mavericks or Snow Leopard 10.6.4" — DIRECTLY relevant to our supported-OS-range goal (10.6.4 floor / per-version ports). Offered to post on GitHub when cleaned up -> GET IT (same as the f54da notes lead). Their handleTouchFrame / wellspring-frame plumbing overlaps our genuine-host seam. Also f54da (OP) still believes our approach "breaks the native system preferences pane" — STALE: we've since made the native pane work (device-tied icon, battery on both transports, live transport switching) via the osax/SIMBL injection. CORRECTION OPPORTUNITY (outward-facing, user's call): a thread reply / README note so the project isn't undersold.
