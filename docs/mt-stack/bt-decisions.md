# BT terminal — the decision, the path we took to it, and what would change it

Scoped companion to `decisions.md`, the mirror of `usb-decisions.md`. Same criteria: what the current
**Bluetooth transport terminal** is, the meandering path that produced it, exactly what would reopen an
alternative, and the pros/cons of settling here. The one-line contrast to hold onto: **USB settled on
*genuine* and BT settled on *owned*, and each was a necessity, not a preference** — the constraints pushed
the two transports to opposite terminals. Read `decisions.md` → "Terminal design space" for the ①/②/③ framing.

---

## 1. What the current BT terminal IS (as of `89cad00`/`e1c94f6`, 2026-07-15)

**Mechanism: owned / synthetic — we bind the real L2CAP channels ourselves and drive our *own* fabricated
`AppleMultitouchDevice`.** This is terminal **① fabricate-AMD**. No Apple `BNBTrackpadDevice` is manual-started;
single ownership top to bottom.

1. **Match.** `com_schmonz_MT2BTReader` matches the MT2's BT-SIG identity (VID 76 / PID 613 / source 1) as an
   IOKit personality. (We cannot make Apple's own driver bind — the matcher reads the real DID from the
   controller-side store, not from a personality; `decisions.md` "VID/PID match path — not functioning". So a
   genuine BT terminal could only ever be reached by *manual-start*, §3.)
2. **Two L2CAP channels.**
   - **Control PSM 17:** send the `0xF1` multitouch-enable (`SET_REPORT {0x53,0xF1,0x02,0x01}`), retried until
     the first real frame arrives (`gSteadyConn == gConnId`, reconnect-v2 `c1a23a6`); then poll `GET_REPORT
     0x90` battery every 30 s and publish `BatteryPercent` on our fabricated AMD node (`e1c94f6`).
   - **Interrupt PSM 19:** `listenAt` → `incomingData` → strip the `0xA1` HID transport byte → `mt2_bt_decode`
     → `submitFrame` → shared session (`mt2_policy_default`) → `kBtSink` → our fabricated AMD.
3. **The AMD is ours.** `mt2_synth_amd` builds one `AppleMultitouchDevice` under our nub, transport-tagged
   Bluetooth (`d12557e`), SP1-hardened so `terminate()` is **synchronous and balanced** — no un-terminatable
   busy, no orphan.
4. **Teardown.** `listenAt(NULL)` deregisters the L2CAP delegate (else UAF, `a65cfc7`); the fabricated AMD
   tears down synchronously. Single owner → single clean termination path.

USB is the opposite (genuine manual-start; see `usb-decisions.md`). The asymmetry is deliberate; consistency
lives at the shared read/conditioning seam, not the terminal.

---

## 2. The meandering path (why we ended up here)

Every SHA verified (`git show -s`, author date).

| Phase | Date | Commit(s) | What happened | Why |
|---|---|---|---|---|
| **Owned born** | 2026-06-16 | `79d0cdc`,`f24766e`,`a65cfc7` | First `MT2BTReader`: bind PSM 19, `listenAt` → decode MT2 `0x31` → feed our own nub. Live-verified; `listenAt(NULL)` on stop (UAF fix). | In-kernel BT transport; single owner from day one. |
| **Genuine-BNB introduced** | 2026-06-20 | `094917a`,`2177a24`,`108565a` | Manual-start Apple's real `BNBTrackpadDevice` on control PSM 17 (bypassing IOKit match); **yield** the PSM 19 delegate so BNB's own `listenAt` wins; interpose BNB's interrupt delegate slot (`0x110`) to translate MT2→MT1. | Reuse Apple's driver + AMD + prefpane. The pane lit natively ("FORM" milestone). |
| **Genuine-BNB shipped** | 2026-06-20 | `1f4bf79` (+ `b224cd8`) | User-verified: native prefpane controls (tap/secondary/3-finger-drag) work live, via `+0x1b0` poke + `MultitouchPreferences` seed. Cursor+gestures work. | This *was* the keeper — max Apple reuse, native pane. |
| **Original synthetic deleted** | 2026-06-25 | `9334273` | Delete the synthetic listener / `0xF1` / tee — genuine interpose only. | Genuine-BNB believed to be the answer; the owned path was dead code. (Later the recovery source for `53b3f6d`.) |
| **Teardown dead-end found** | 2026-06-23 | `7168593` | `waitQuiet` synchronous-teardown attempt **removed**: on-device probe proved the manual `BNBTrackpadDevice` is `busyState=1` for its *entire life* and never settles (its genuine connect lifecycle never reaches `deviceReady`; the 5 s watchdog cycles), so the start-time busy is **never balanced**. | First hard evidence that manual-start-outside-matching cannot cleanly terminate. |
| **Battery + reconnect on genuine** | 2026-07-01 / 07-04 | `50d7172`,`c1a23a6` | Real battery on the BNB node; reconnect retry-until-first-frame. | Feature-complete the genuine hybrid. |
| **A/B knob (soft exit)** | 2026-07-13 | `f44d6d4`,`c9573c3` | `debug.mt2_bt_synth` sysctl (default 0=genuine) + a fabricated-AMD terminal under `=1`. | Stage a reversible exit; prove owned-BT works side-by-side before committing. |
| **The orphaned-AMD panic** | 2026-07-15 | `44261f4` | Root cause: `Kernel_2026-07-15.panic` = NX fault in `AppleMultitouchDevice::removeFramesClient` at shutdown, calling `getWorkLoop()` on a **freed** AMD. Live: `AppleMultitouchDevice=2, BNBTrackpadDevice=0` — two orphaned AMDs. The un-balanced BNB busy (from `7168593`) means `terminate()` can't finalize → the BNB's child AMD orphans → accumulates per connect/disconnect → shutdown UAF. | The genuine-BNB teardown flaw is **inherent to split ownership**, not a tuning bug. Forces owned. |
| **Full pivot to owned** | 2026-07-15 | `53b3f6d`,`89cad00`,`e1c94f6` | `MT2BTReader` back to direct PSM 19 listener → SP1-hardened fabricated AMD (recover `9334273^`); delete **all** genuine-BNB machinery (`manualStartGenuineBnb`, `seedBnbIdentity`, interrupt-interpose shim, geometry vtable-clone, the A/B flag, `gh_start`/`gh_stop`); battery re-homed onto the fabricated AMD node. | Single ownership → synchronous, balanced teardown → the orphan-panic source is gone. |

The arc: **owned (06-16) → genuine-BNB shipped (06-20) → teardown proven un-terminatable (06-23) → shutdown
orphan panic (07-15) → back to owned, permanently (07-15).** Where USB's whipsaw ended on *genuine* because
that was the only clean path to a native pane, BT's ended on *owned* because genuine-BNB **cannot be cleanly
torn down at all**.

---

## 3. Where this sits in the terminal design space

`decisions.md` frames **① fabricate-AMD**, **② subclass-the-class** (CLOSED, panics), **③ device-emulate
transport-native**. Current BT is squarely **①**: our nub owns a fabricated AMD.

Retired genuine-BNB was the *same* "manual-start Apple's real driver + interpose" mechanism that current
genuine-USB uses (the "fourth thing" in `usb-decisions.md` §3) — on BT it manual-started `BNBTrackpadDevice`
and interposed its delegate. **So the two transports are mirror images: USB kept the manual-start-genuine
terminal; BT abandoned it for ①.** The deciding difference is not the seam quality (BT's seams are actually
*cleaner* — `BNBTrackpadDevice` delegates through a `BluetoothMultitouchTransport` we can interpose, and we
observe frames independently of the AMD; USB's `AppleUSBMultitouchDriver` is monolithic) — it is **teardown
ownership**: Apple's USB driver, manual-started, terminates cleanly; a manual-started `BNBTrackpadDevice` never
balances its busy and orphans its AMD. Clean seams didn't save genuine-BT; clean *ownership* is why owned-BT wins.

---

## 4. What would have to change for our approach to change

Owned-BT is the intended production terminal, for 10.9 and the sub-10.9 roadmap. State the reopening bar
precisely so we don't hedge: **reconsidering genuine-BT requires a CONJUNCTION of two things, not either
alone** —

- **(a) The teardown panic must first be *solved*.** Genuine-BT is disqualified *today* by the un-terminatable
  busy: a manual-started `BNBTrackpadDevice` sits at `busyState=1` for its whole life (`deviceReady` never
  reached; watchdog cycles) → `terminate()` never finalizes → orphaned AMD → shutdown UAF (`removeFramesClient`),
  proven on-device (`7168593`, `44261f4`). The only known path out is landing the full
  `waitForChannelState(OPEN)` control-channel handshake so BNB reaches `deviceReady` and settles to `busy=0`
  (`decisions.md` `waitQuiet` reopening criterion). Until that is built, genuine-BT is off the table no matter
  what else is true — reverting today would just trade a bug for a shutdown panic. And note that *even solving
  it* would not make us *want* genuine-BT: it re-adds split ownership and the hybrid `+0x1b0` poke whose teardown
  drove the `ultimate-hat.panic`. The handshake fix only makes genuine-BT *possible* to reconsider, not desirable.
- **(b) AND a must-have capability must prove genuinely impossible in owned-BT** by any means.

Both, together. That is a remote conjunction, which is why "we plan owned, full stop" is a decision, not a hedge.

**The login-screen reconnect is NOT that forcing function — it is an owned-BT regression to fix.** Earlier
genuine-BT reliably reconnected on click at the login screen; owned-BT does not (`open-questions.md`). That is a
capability the pivot *lost*: Apple's `IOBluetoothHIDDriver` gave paired-device host-initiated reconnection for
free, and by listening on L2CAP directly we stopped going through it. It fails **both** bars above — reverting
doesn't clear (a), and reconnecting a paired HID device is BT-stack registration / page-scan behavior, not a
genuine-vs-fabricated-AMD choice, so it doesn't meet (b). The fix is to **recover that reconnection inside
owned-BT** (register the paired device for reconnect / keep page-scan armed / re-arm on click). And the clincher:
even if we ever proved we needed Apple's reconnection machinery specifically, the answer is a *narrow* reuse of
that one hook — never the manual-start-BNB terminal, which re-imports the panic. So genuine-BT-the-terminal stays
disqualified either way.

- **Porting below 10.9** — owned-BT is *already* the portable choice (the reeval's whole point: owned carries
  forward, genuine re-derives Apple internals per version). Sub-10.9 ports **keep** owned-BT; there is nothing
  to reopen. (Contrast USB, where genuine's per-version RE tax is the standing reason to re-open owned/hybrid.)

- **The old "REPLACE not desirable" verdict is already OVERTURNED.** `decisions.md` REPLACE once ruled owning
  the device "not desirable" for one reason only — "stock Apple prefpane mandatory, and the pane matches
  `BNBTrackpadDevice`." That reason dissolved: we now drive the pane via the shipped osax/SIMBL swizzle
  regardless of which device backs it (battery, icon, live-transport), exactly as [[genuine-vs-owned-device-reeval]]
  predicted. So owning the BT device no longer loses the pane — REPLACE *is* the shipped path, and this doc is
  its ratification.

- **Porting below 10.9** — owned-BT is *already* the portable choice (the reeval's whole point: owned carries
  forward, genuine re-derives Apple internals per version). Sub-10.9 ports **keep** owned-BT; there is nothing
  to reopen. (Contrast USB, where genuine's per-version RE tax is the standing reason to re-open owned/hybrid.)

---

## 5. Ramifications of settling here — pros and cons

**Pros**
- **Single ownership → clean, synchronous teardown.** The defining win: we build and destroy the AMD, so
  `terminate()` finalizes and no orphan survives to fault at shutdown (`removeFramesClient` UAF). This is the
  whole reason for the pivot.
- **We own the enable→configure→first-frame sequence natively.** BT gets the *principled* condition-wait
  (`retry-0xF1-until-first-frame`) that USB can only approximate with a blind 50 ms settle — because we own the
  channel and observe frames independently of the AMD (`decisions.md` REPLACE ammunition).
- **Portable / mission-aligned.** Owned is version-stable (no per-release RE of Apple internals) and is the
  interface-over-driver direction ([[mt2-mission-interface-over-driver]], [[genuine-vs-owned-device-reeval]]).
- **No fragile split-ownership dependencies** — no manual-start, no `+0x1b0` poke, no un-balanced busy, no
  geometry vtable-clone UAF surface; all of that machinery was deleted (`89cad00`).

**Cons**
- **Pane/identity not native → osax swizzle machinery.** With no `BNBTrackpadDevice`, the pane matches nothing
  by default, so identity (battery, icon, live-transport, the presence the pane observes) is delivered by the
  shipped osax/SIMBL swizzles rather than for free. It works and is shipped, but it is standing machinery to
  maintain, and the **BT pane device *picture* is a known unfixable 10.9 limitation** (CoD-driven vault).
- **We own the whole BT link lifecycle — including reconnection Apple used to give us for free.** PSM 17/19
  bind ordering, the `0xF1` enable, and reconnect are ours to get right: the surface of the **reconnect
  enable-fails** bug (rapid power-cycle) and the **login-screen reconnect regression** — earlier genuine-BT
  reconnected on click at the login screen because Apple's `IOBluetoothHIDDriver` provides host-initiated
  reconnection of a paired device; listening on L2CAP directly, owned-BT lost it and must recover it (register
  the paired device for reconnect / keep page-scan armed / re-arm on click). A cost of owning the terminal, but
  a *feature to add to owned-BT*, not a reason to reconsider genuine (§4).
- **Transport asymmetry with USB** (owned BT / genuine USB) — an internal inconsistency, accepted for the same
  reason as in `usb-decisions.md`: it mirrors Apple's own 10.9 stack (BT clean seams, USB monolith), and the
  consistency that matters is at the interface layer (both terminals are publish-backends behind one shared
  read/conditioning seam).

**Net.** Owned-BT is the right and intended terminal: it is the only one that tears down cleanly, it is the
portable/mission-aligned choice, and the single reason it was ever called "not desirable" (losing the stock
pane) no longer holds now that we swizzle the pane regardless. The remaining costs are (a) the osax identity
machinery — shipped and maintained, and (b) owning the BT link handshake — which is exactly the reliability
work now queued (login-screen connect, reconnect enable-retry, CoD-match). Bringing owned-BT "all the way to
production" is finishing (b), not reconsidering the terminal.

---

## Cross-references
- `usb-decisions.md` — the mirror-image transport; the "fourth thing" (manual-start genuine) that BT retired
  and USB kept; the genuine-reuse tax ledger.
- `decisions.md` — ①/②/③ framing; REPLACE (now overturned); the `waitQuiet` teardown dead-end; the hybrid
  `+0x1b0` poke panic; VID/PID-match / IOMatchCategory / geometry-override roads not taken.
- `explanation.md` — L2CAP channel binding, the interpose-delegate mechanics, genuine-BNB internals (preserved
  for the RE record).
- `open-questions.md` — login-screen no-connect, PSM-19 / control-channel handshake ordering, reconnect
  enable-fails, CoD exact-match bug.
- Memories: [[genuine-vs-owned-device-reeval]], [[bt-reconnect-enable-fails]],
  [[bt-genuine-kext-unloaded-no-drive]], [[mt2-single-transport-at-a-time]], [[mt2-supported-os-range]].
