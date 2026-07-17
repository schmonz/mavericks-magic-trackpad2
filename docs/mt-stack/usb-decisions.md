# USB terminal — the decision, the path we took to it, and what would change it

Scoped companion to `decisions.md`. That file records *roads not taken* across the whole stack; this one
tells the **USB transport terminal's** story end-to-end: what the current mechanism is, the meandering path
that produced it, exactly what would have to change for us to change it, and the pros/cons of having settled
here. Read `decisions.md` → "Terminal design space" first for the ①/②/③ framing this doc leans on.

---

## 1. What the current USB terminal IS (as of `ea82e2a`, 2026-07-16)

**Mechanism: manual-start Apple's *real* driver on the *real* interface, then interpose its input path.**
Not our own fabricated device (①), not a subclass (②), not an IOKit-matched virtual device (③) — a fourth
thing: we run Apple's genuine `AppleUSBMultitouchDriver` ourselves and reframe the bytes flowing through it.

1. **Sole match.** `com_schmonz_MT2USBReader` matches the MT2's `IOUSBInterface`. Apple's own
   `AppleUSBMultitouchDriver` does **not** match it — the MT2's `idProduct` 613 (`0x265`) is absent from
   Apple's `idProductArray` — so we are the *only* matcher. (⚠️ Do **not** "fix" this by injecting 613 into
   Apple's match: Apple then wins the interface at score 99000, our reader gets 0 instances, the trackpad
   goes dead. Documented dead end — see `decisions.md` and [[mt2-genuine-usb-catalogue-residue-trap]].)
2. **Enable, then settle, then start** (order is load-bearing, `22bbf1a`). Send the MT2 USB multitouch-enable
   control transfer + `IOSleep(50 ms)` so the device leaves mouse-mode **before** Apple's driver probes it.
   Skip this and Apple's driver storms a still-in-mouse-mode device with feature-report requests
   (`0xe000404f`) → the `!pageList phys_addr` getReport panic ([[mt2-usb-bringup-getreport-panic]]).
3. **Manual-start.** `allocClassWithName("AppleUSBMultitouchDriver")` → `init` (with `usb_build_init_props`)
   → `attach` → `start` on our interface, via the host-tested `genuine_host` lifecycle core. Apple's real
   driver spawns the AMD, owns the interrupt pipe, wires the recognizer, and **lights the prefpane natively**.
4. **Interpose `handleReport`.** Instance-scoped vtable clone (declarative row `kUsbHandleReportRow`, slot
   byte-offset `0x8b8` = index 279, re-verified against the live driver 2026-07-16) repoints `handleReport`
   to `mt2_usb_handle_report`: each raw MT2 `0x02` report → `mt2_usb_decode` → shared session
   (`mt2_policy_default`) → `kUsbSink` → MT1-encode + checksum → **chain Apple's captured original**
   `handleReport`. Apple's recognizer sees a well-formed MT1 report and does *all* gesture/identity/pane work.
5. **Teardown.** `gh_stop`: `vtc_restore` (restore the vtable **first**) → `terminate` → `release` the
   manual-started driver. Validated clean this session (unload → our reader AND the AMD both drop to count 0,
   no orphan, no panic; reload re-establishes 1+1 each cycle).

BT is unchanged and **synthetic** (① fabricated-AMD + direct L2CAP listener). The transports are asymmetric
by design (§5).

---

## 2. The meandering path (why we ended up here)

Every SHA below is verified (date = author date, `git show -s`).

| Phase | Date | Commit(s) | What happened | Why |
|---|---|---|---|---|
| **Synthetic born** | 2026-06-17 | `684d23f` | First `MT2USBReader`: opens the interrupt-IN pipe itself, `armRead`/`readComplete` → decode → feed a fabricated AMD. No Apple driver. | Replace the userland feeder with an in-kernel transport. |
| **Genuine introduced** | 2026-06-24 | `8aa5832` | `startGenuine()`: manual-start Apple's `AppleUSBMultitouchDriver` + interpose `handleReport` (CompactV4 reframe). **Flag-gated OFF.** | Trial genuine-vs-synthetic side by side without committing. |
| **Genuine wins** | 2026-06-25 | `a3a699e` | Delete the synthetic read loop; `start()` is genuine-only. | Genuine tested good on-device; the pipe loop was dead code. Native pane + max reuse. |
| **Panic hardening** | 2026-07-04 | `22bbf1a` | Enable+50 ms-settle **before** manual-start. | The getReport storm on a mouse-mode device caused a kernel panic; sequencing fixed it. |
| **The full-synthetic pivot** | 2026-07-15 | `9573611` → `292a08d` (USB); `53b3f6d`→`89cad00` (BT); `c182fc1` (delete `genuine_host`) | Both transports ripped back to synthetic; all genuine manual-start machinery deleted. | A **different** panic: manual-started Apple drivers left **orphaned AMDs** (un-terminatable busy imbalance → use-after-free in `removeFramesClient`). Single-ownership synthetic has no such surface. |
| **Synthetic unified** | 2026-07-12→15 | `1c4e925`,`202dde5`,`138819b`,`f29a004`,`d12557e` | One fabricated-AMD backend (`mt2_synth_amd`), on-demand lifecycle, transport-aware identity. | Single upstreamable backend; per-instance ownership; USB stops falsely claiming Bluetooth. |
| **Declarative splice** | 2026-07-08 | `mt2_splice`/`mt2_splice_kext` + rows | Interposition becomes host-tested declarative rows (save-before-write, idempotent, restore-only-if-ours). | Retire raw pointer arithmetic; one audited engine for every seam. |
| **② subclass — CLOSED** | 2026-07-16 | `e5e67c7` | On-device test of "wear `AppleUSBMultitouchDriver`, run none of its code." **Kernel panic**: `registerService()` on a bare `IOHIDDevice`-lineage node null-derefs in `_publishDeviceNotificationHandler`. | Identity is inseparable from *starting* an `IOHIDDevice`. ② collapses into genuine for USB. |
| **Genuine recovered** | 2026-07-16 | `6ce9e37`,`ea82e2a`,`17a5a7b` | Recover the genuine-USB terminal from `9573611^`, reconcile onto HEAD (inline the adapter, declarative interpose). **USB=genuine, BT=synthetic.** | ② is impossible; ③ un-proven; ① leaves the pane dark ("No Trackpad"). Genuine-USB's orphan risk was **BT-specific**; USB teardown was empirically clean. |

The whipsaw in one line: **genuine (06-25) → synthetic (07-15, for the orphaned-AMD panic) → genuine again
(07-16, once ② was ruled out and the orphan panic was understood to be BT-only).** The full-synthetic pivot's
own cost proved the point — deleting the genuine nodes broke the prefpane exactly as
[[genuine-vs-owned-device-reeval]] predicted (pane matches `AppleUSBMultitouchDriver`; count 0 → "No Trackpad").

---

## 3. Where this sits in the terminal design space

`decisions.md` frames three terminals: **① fabricate-AMD** (own nub feeds a fabricated AMD — SHIPPED, but
identity above the AMD is lost → per-consumer osax swizzle whack-a-mole), **② subclass-the-class** (CLOSED,
panics), **③ device-emulate transport-native** (publish a *started* natively-identified virtual device, let
IOKit match Apple's driver — partially tried, **not closed**; the 2026-07-13 USB vhid spike got *adoption but
no dispatch*, gap un-isolated).

The current USB terminal is **none of those three** — call it the **genuine manual-start + interpose**
terminal. It captures ③'s payoff (Apple's driver + AMD + recognizer + pane, all native) **without** ③'s open
blocker (built-in gating / virtual-device dispatch), because it uses the **real** device, interface, and
driver rather than a fabricated stand-in. The price is the manual-start lifecycle + the interpose seam, which
③ (if it dispatched) would not need. Genuine-BT was the same mechanism on the other transport; it shipped and
worked, and was retired only by the full-synthetic pivot — not because it failed.

---

## 4. What would have to change for our approach to change

Each door names the exact evidence that reopens it. None are open today.

- **③ (device-emulate) becomes the terminal** — *if* the discriminating experiment in `decisions.md` §"one
  experiment" succeeds: publish a **started** `IOHIDDevice` with the **built-in/onboard** identity
  (`MT Built-In=true` + onboard match keys), drive the **full MT1 enable handshake**, feed `0x28`, and observe
  Apple **spawn the AMD and dispatch** (the step the 2026-07-13 bare vhid skipped). If it dispatches, ③ is
  strictly cleaner (no manual-start, no interpose UAF surface, Apple's driver decoupled from hardware churn) →
  genuine-manual-start and the ① swizzles both retire. If it still won't dispatch, we've isolated
  *recognizer-needs-a-real-AMD* vs *didn't-pass-the-built-in-gate*, and the current terminal stands on evidence.

- **Porting below 10.9** — re-open **genuine-vs-owned-vs-hybrid** deliberately, judged on
  portability/conformance/reuse (NOT prefpane recognition — a solved, swizzle-shaped problem either way,
  [[genuine-vs-owned-device-reeval]]). Genuine re-derives Apple internals **per OS version** (the `0x8b8`
  slot, the init-props dict, the enable handshake, the "magic interpose offsets"). An **owned** device is
  version-stable. iphone2g&3gfan's **hybrid** (subclass the genuine driver: genuine base for
  conformance+reuse, owned subclass for a stable seam) is the middle path to study — it's the OS-publish seam
  we'd otherwise reinvent per version ([[mt2-supported-os-range]]).

- **▶ QUEUED (user, 2026-07-16): move USB to OWNED, via a name-wearing reimplemented driver.** The most
  actionable reopener, to pursue *once owned-BT is where we want it*. iphone2g&3gfan (VoodooInput Wellspring
  backend) proved the mechanism our ② closure wrongly dismissed: `OSDefineMetaClassAndStructors(
  AppleUSBMultitouchDriver, IOHIDDevice)` — a **from-scratch reimplementation wearing Apple's class name** so it
  passes the recognition gate (`IOObjectConformsTo(service,"AppleUSBMultitouchDriver")` — the same gate we RE'd
  at `open-questions.md` `_MTDeviceCreateFromService`), then a simulator subclass fed frames. It implements the
  full `IOHIDDevice` lifecycle, so `registerService()` is safe (our `AumdShimTest` panicked only because it was
  *bare*). This is the **owned-USB terminal**: it erases every con in §5 — no `0x8b8` vtable dependency
  (portable down toward Leopard), no manual-start/interpose UAF, no monolithic-driver taxes (settle,
  geometry residual), and no fragile "Apple must not match the interface" assumption. **Open caveat on 10.9:**
  redefining the `AppleUSBMultitouchDriver` metaclass collides with Apple's co-resident `AppleUSBMultitouch.kext`
  — resolving that co-residence (or not loading Apple's) is the prerequisite. Complementary, not drop-in: his
  simulator is fed by VoodooInput satellites, so a real MT2 needs *our* front-half reader. He is posting the code
  on GitHub. Full analysis + the ② closure correction: `decisions.md` ("CORRECTION 2026-07-16"); merge-map
  [[genuine-vs-owned-device-reeval]].

- **The USB manual-start teardown proves untenable** (an orphaned-AMD panic on USB like the BT one) — fall
  back to ① synthetic-USB (still live for BT/VoodooInput, so no capability is lost), or pursue the "intercept
  Apple's match / claim the interface" direction (`60c2c51`, `open-questions.md`). Not observed: USB teardown
  has been clean across ~12+ historical reloads plus this session.

- **BT rejoins genuine (symmetry restored)** — *if* the `BNBTrackpadDevice` un-terminatable busy imbalance
  (`removeFramesClient` UAF) that retired genuine-BT is solved. Until then BT stays synthetic; the asymmetry
  is deliberate, not an oversight.

---

## 5. Ramifications of settling here — pros and cons

**Pros**
- **Native everything (the payoff).** The prefpane shows the full trackpad UI on USB; icon, battery, and the
  bezel HUD resolve natively — no per-consumer osax swizzle whack-a-mole that ① requires. Validated on-device
  2026-07-16.
- **Maximum Apple-code reuse** (the mission, [[mt2-mission-interface-over-driver]]
  [[reuse-apple-code-construct-seam]]): recognizer + AMD + hidd + pane are all Apple's; we only reframe wire
  bytes.
- **Conformance-safe by construction.** Apple's real driver keeps passing Apple's own checks; there is no
  fabricated device that must be kept conformant as the OS evolves (it won't — 10.9 is frozen).
- **Correct geometry for free.** Genuine 30×22 comes from the real driver, so the half-resolution
  edge-dead-zone hazard of the synthetic seed cannot occur here ([[mt2-cursor-edge-clamp]]).
- **Sidesteps ③'s unsolved gate.** Using the real device+driver avoids the built-in-gating / no-dispatch
  problem that ③ has not yet cleared.

**Cons**
- **Interpose UAF surface.** The vtable clone is a use-after-free waiting to happen if teardown is sloppy;
  correctness rests on instance-scoped cloning + `vtc_restore`-before-release + idempotent `gh_stop`. Mitigated
  and host-tested, but it is real surface that ① and (a dispatching) ③ don't carry.
- **The monolithic-USB tax.** USB's `AppleUSBMultitouchDriver` has no delegating transport seam like BT's
  `BluetoothMultitouchTransport`, so bring-up needs the crude 50 ms enable-**settle** blind-time bridge
  (`22bbf1a`) instead of BT's clean condition-wait. Every USB bridge is cruder than its BT twin; the running
  ledger of this tax lives in `decisions.md` → the REPLACE entry.
- **Per-version portability tax.** Genuine depends on RE'd, version-specific internals; each OS below 10.9 is
  a fresh derivation. An owned device would erase this. This is the single biggest reason the door in §4 stays
  explicitly open for sub-10.9 ports.
- **Fragile-assumption dependency.** Correctness relies on Apple's driver *not* matching the interface (PID
  not in `idProductArray`). It holds for this device, but it is an assumption to state, not forget — and the
  tempting "fix" (inject the PID) inverts it and kills the trackpad.
- **Transport asymmetry.** USB=genuine / BT=synthetic is an internal inconsistency. Accepted because it
  mirrors Apple's own 10.9 stack asymmetry (USB monolith vs BT clean seams) and the consistency that matters
  lives at the **interface** layer — both terminals are publish-backends behind one shared
  device-read/conditioning seam, so a future device plugs in regardless of transport.

**Net.** On frozen 10.9, genuine-USB is the right call: it buys native identity and max reuse for a one-time,
never-rotting RE cost, and its teardown risk (the thing that scared us into full-synthetic) turned out to be
BT-specific. The costs that remain are (a) interpose discipline — contained and tested — and (b) portability,
which is precisely the axis we've committed to re-open, not resolve, when we go below 10.9.

---

## Cross-references
- `decisions.md` — ①/②/③ framing, the ② panic closure, the genuine-USB "IMPLEMENTED + validated" block, the
  discriminating ③-vs-② experiment, the REPLACE tax ledger.
- `explanation.md` — the two-injected-vtable mechanics, genuine-USB packet/seam detail, the synthetic path.
- `open-questions.md` — the genuine-USB packet residual, the "intercept Apple's match" pivot, in-pane detection.
- `reference.md` — vtable slots (`0x8b8` handleReport, `0xb28` handleButton), report formats.
- Memories: [[genuine-vs-owned-device-reeval]], [[mt2-usb-bringup-getreport-panic]],
  [[mt2-genuine-usb-recovery-task]], [[mt2-genuine-usb-catalogue-residue-trap]], [[mt2-supported-os-range]].
