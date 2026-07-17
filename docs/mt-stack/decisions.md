# Decisions — big-ticket roads not taken

Why we *didn't* go certain ways, so we don't re-tread them. Two kinds:

- **Not a functioning choice** — a fact makes it impossible. Won't revisit.
- **Not a sufficiently desirable choice** — it works (or could), but lost on a constraint. The entry
  names the constraint, so it's clear what would reopen it.

For *open* unknowns (vs these resolved ones) see `open-questions.md`.

---

## Terminal design space — the three ways to present to 10.9 (framing + open experiment, 2026-07-15)

The stack has three OS-agnostic layers we've paid for once — the **VoodooInput inbound interface** (satellites
send `VoodooInputEvent`s), the **conditioning pipeline** (MT2 decode → MT1 `0x28` contact format,
`mt1_encode`), and the **terminal**: the node that *presents to the OS* so the pane/gestures/identity
consumers see a trackpad. The terminal is the only **per-OS-version** layer. Adding 10.8/10.7/10.6 = swap the
terminal, not re-architect. This section frames the three terminal mechanisms and the one experiment that
would pick between them; the individual "roads not taken" below are the evidence.

**The VoodooInput analogy, stated precisely.** VoodooInput and us share the *goal* ("make the native stack
see a supported trackpad") but conform at **different layers**. VoodooInput's `VoodooInputSimulatorDevice`
**fabricates a fake MT2 device** and lets IOKit *matching* bind Apple's own driver to it — device-layer
conformance, reusing Apple's driver+AMD+pane wholesale. It has **zero code to drive a real Magic Trackpad**
(that front-half is uniquely ours; see "Run VoodooInput on 10.9" below). Two 10.9 facts force us off their
exact mechanism: (a) our device (MT2) is *unsupported* on 10.9 — there's no native MT2 driver to bind an
emulated MT2; (b) the nearest *bindable* 10.9 driver path is panic-prone (getReport churn). So we conform at a
*lower, more brittle* layer than VoodooInput does.

**Why we're unusually willing to subclass/depend-on a private API here:** these OSes are **frozen**. Any
per-version RE (vtable order, consumed method surface, enable handshake) is a **one-time cost that never
rots**. That converts "brittle" into "pay once per version, permanently" — a bargain that would be reckless on
macOS-current.

### The three terminals

| Terminal | Mechanism | Reuses Apple's… | Where you pay | Status |
|---|---|---|---|---|
| **① Fabricate-AMD** (current) | our nub allocs its **own** `AppleMultitouchDevice`, feed it `handleTouchFrame` (0x28) | recognizer + hidd, **from the AMD down** | identity above the AMD is lost → per-consumer osax swizzles (pane view, icon, battery, the **bezel HUD in a process the osax can't reach**) | **SHIPPED.** Input works; pane/identity is whack-a-mole |
| **② Subclass-the-class** | our reader `: public AppleUSBMultitouchDriver` / `BNBTrackpadDevice`; inherit **identity**, override **behavior** with our synthetic pipeline | class *identity* (all consumers match via metaCast); not Apple's driver *code* | **per-version RE**: reconstruct the undocumented class vtable + enumerate the **consumed method surface** so no call falls through to un-started `super` (= panic). USB tractable (already matches `IOUSBInterface`); BT hard (`BNBTrackpadDevice` born inside Apple's BT HID stack) | **PARTLY CLOSED (2026-07-16).** The *bare* subclass panics: `registerService()` on a bare `IOHIDDevice`-lineage node null-derefs in `_publishDeviceNotificationHandler` (all RE validated — size 1672; lineage `AppleUSBMultitouchDriver : IOUSBHIDDriver : IOHIDDevice`; `kxld -n` "loadable"). But a **REIMPLEMENTED name-wearing subclass is VIABLE** (`OSDefineMetaClassAndStructors(AppleUSBMultitouchDriver, IOHIDDevice)` + full lifecycle — iphone2g&3gfan) = the **owned-USB** terminal, portable + no vtable offsets; caveat = metaclass-name collision with Apple's co-resident kext. See the CORRECTION in the closure entry below. |
| **③ Device-emulate transport-native** (VoodooInput-faithful) | publish a *started* virtual device with a **natively-supported** identity — **MT1 over BT**, **onboard/built-in trackpad over USB** — let Apple's driver **own** the AMD; our reader drives the real MT2 into it | Apple's **driver + AMD + recognizer + pane**, the most of all; also **decouples Apple's driver from the real-hardware churn** → dodges the getReport panic that motivated the USB pivot | reproducing the device + **enable handshake** + passing **built-in gating** (USB multitouch is built-in-only gated; BT external is not — which is exactly why the transport split is MT1-BT / onboard-USB) | **Partially tried, NOT closed.** Genuine-BT (Apple owns AMD) **shipped and worked**; the pure-vhid USB spike got **adoption but no dispatch** with the failure **un-isolated** |

### Evidence already on record (don't re-tread)

- **③ on BT worked and shipped:** genuine `BNBTrackpadDevice` let Apple spawn+own the AMD; retired by the
  full-synthetic pivot (SP2) for single-ownership/clean-teardown/generalization — *not* because it failed
  (see "REPLACE" below, `explanation.md` transport table).
- **③ on USB, the decisive spike (2026-07-13):** kextless `IOHIDUserDevice` (`src/vhid_mt1.c`) presenting the
  MT1 descriptor + posting `0x28` → `AppleMultitouchHIDEventDriver` **adopted** it and made an `IOHIDPointing`
  shell, but **no `AppleMultitouchDevice` was spawned and the cursor didn't move.** Concluded "not a drop-in
  win" — **but the residual was explicitly NOT isolated:** the bare vhid used a *generic* MT1 identity and
  **lacked the enable handshake + built-in context** the real device has. See "Post HID input reports" below.
- **② hazard is real:** publishing a **bare un-started** `IOHIDDevice` of the matched class panics
  (`_publishDeviceNotificationHandler` null-deref) — this is the completeness obligation ②/③ both carry
  (a started device / full override avoids it; genuine-BNB worked because it's a *real started* IOHIDDevice).
- The clean "publish-a-nub-let-IOKit-match" question is **already logged open** (see "Run VoodooInput on 10.9"),
  with three named teeth: catalogue residue, built-in gating, the un-started-IOHIDDevice panic.

### The one experiment that discriminates ② vs ③ (and would retire the fabricate-AMD whack-a-mole)

**On USB, close the 2026-07-13 residual:** publish a **started** IOHIDDevice with the **onboard/built-in**
identity (`MT Built-In=true` + the onboard interface match keys), drive the **full MT1 enable handshake**,
feed `0x28`, and observe whether Apple now **spawns the AMD and dispatches** (the step the bare vhid skipped).
- **If it dispatches:** ③ is the cleanest terminal (pane + all identity consumers native, driver decoupled
  from hardware churn); ② becomes unnecessary; the fabricate-AMD swizzles retire.
- **If it still won't dispatch:** we've finally isolated *recognizer-needs-a-real-AMD* vs *didn't-pass-the-
  built-in-enable-gate*, and ② (subclass) or ① (keep fabricating) stand — decided on evidence, not guess.

**Paired USB ② prototype (run only if ③ doesn't dispatch):** flip `MT2USBReader`'s base
`IOService`→`AppleUSBMultitouchDriver`, override `start()` to run our synthetic pipeline (no `super::start`),
RE+override the consumed surface, load, and measure: does the pane light **natively**, do icon/bezel/battery
consumers resolve natively, and **how large was the override surface** (that number sizes the per-version BT
cost). Both experiments are USB-first by design (BT is the expensive transport for both ② and ③) — validate
the generalizable claim on the cheap transport before buying the multi-version roadmap. **Prerequisite for
either:** the in-pane `usb=0` detection anomaly (`open-questions.md`) is moot for ③ (native match) but must be
understood if we keep any osax-driven detection — the staged `service_present` DIAG log resolves it on one load.

---

### ② "subclass-the-class" for USB — NOT FUNCTIONING (kernel panic, on-device 2026-07-16); DECISION: USB=genuine, BT=synthetic

**What we tried.** A binary subclass of the private `AppleUSBMultitouchDriver` to hold its class identity
(so the pane's `loadMainView` matches it) while running **none** of Apple's driver code — "wear the class,
override the behavior." Minimal proof: `com_schmonz_AumdShimTest : AppleUSBMultitouchDriver`, matched on
`IOResources`, `start()` = a size/identity safety gate then `registerService()`. Artifacts (now removed;
git history + this entry are the trail): `kext-gesture/apple_usb_mt_shim.h`, `examples/AumdShimTest/`.

**How far it got — all the RE was correct.** The reconstruction fully validated OFF-device:
- instance size `0x688` (1672) — compile-time `static_assert` passed against Apple's metaclass-ctor literal;
- **true lineage RE'd via `kxld` + vtable relocations: `AppleUSBMultitouchDriver : IOUSBHIDDriver : IOHIDDevice
  : IOService`** — NOT `IOHIDEventService` (that's the sibling `AppleUSBMultitouchHIDEventDriver`, the
  `IOHIDInterface` personality). The whole base chain has on-box headers, so the vtable is correct **by
  construction** (`#include <IOKit/usb/IOUSBHIDDriver.h>`, needs `-DKERNEL=1`);
- `kxld -n` dry-run: **"appears to be loadable"** — vtable, size, and `OSBundleLibraries` (via `kextlibs`:
  `AppleUSBMultitouch` + `IOHIDFamily` + `IOUSBHIDDriver`) all resolved against the running kernel.
- **`kxld -n` is a genuine off-device oracle** for vtable/link correctness — reusable technique; it caught
  every structural error (wrong size → "malformed"; wrong base → "super vtable out of date") before any load.

**How it failed.** `kextload` → **instant kernel panic** (lost SSH/VNC). Null-pointer page fault at IOHIDFamily
offset `0x3c02` = **`IOHIDDevice::_publishDeviceNotificationHandler`**, triggered by our `start()`'s
`registerService()`. Machine recovered on reboot (the kext was in `/tmp`, never installed to `/S/L/E`). The
`start()` safety gate *passed* (the RE was right) — the fault is **inside** `registerService`'s IOHIDDevice
publish machinery, downstream of anything our gate can guard.

**Why — the load-bearing conclusion.** `AppleUSBMultitouchDriver` **is an `IOHIDDevice`**. Registering an
`IOHIDDevice` fires `_publishDeviceNotificationHandler`, which dereferences state that only `IOHIDDevice::start`
initializes. So **a registered `IOHIDDevice` must be a properly *started* one** — `registerService()` on a
bare/un-started node is a guaranteed panic. This is EXACTLY the pre-existing "Own / bare `IOHIDDevice`" entry
below (and the "② hazard is real" note in the terminal-design-space section above) — both predicted it; the
load should not have happened without heeding them. Therefore **"identity-only, run none of Apple's code" is
impossible for any `IOHIDDevice`-lineage class.** To hold the `AppleUSBMultitouchDriver` identity you must
`start` it (Apple's HID machinery), i.e. **② collapses into genuine for USB.** (Same lineage on BT:
`BNBTrackpadDevice : IOHIDDevice` — which is exactly why genuine-BNB worked by *manually starting a real
started IOHIDDevice*, per the entry below.) This also retires my earlier mis-claim that USB was the "clean
synchronous `IOHIDEventService`" case — both transports are the hard `IOHIDDevice` lineage.

**CORRECTION 2026-07-16 (iphone2g&3gfan, MacRumors) — "② collapses into genuine" was TOO STRONG.** What our
`AumdShimTest` proved is narrower: a **bare, un-started** subclass panics. It does NOT follow that ② collapses
into genuine — only that the started `IOHIDDevice` requirement must be *met*. It can be met by **reimplementing
the class** instead of starting Apple's. iphone2g&3gfan (VoodooInput Wellspring backend) does exactly this:
`OSDefineMetaClassAndStructors(AppleUSBMultitouchDriver, IOHIDDevice)` — a **from-scratch reimplementation that
wears Apple's class name** (so `IOObjectConformsTo(service,"AppleUSBMultitouchDriver")` — the same recognition
gate we RE'd at `open-questions.md` `_MTDeviceCreateFromService`/`IOObjectConformsTo`→type 1 — returns true for
his subclass), then `VoodooInputWellspringSimulator : AppleUSBMultitouchDriver`. Because he implements the full
`IOHIDDevice` lifecycle, `registerService()` is safe — no panic. Our shim panicked because it inherited Apple's
real class but ran *none* of its code; his *is* the code. So the accurate closure is: **bare-subclass = CLOSED
(panic); reimplemented name-wearing subclass = VIABLE — it is the OWNED-USB terminal** (portable, no per-version
vtable offsets, no manual-start/interpose UAF, no monolithic-driver taxes; the USB analog of owned-BT). Open
caveat on 10.9: his trick redefines the `AppleUSBMultitouchDriver` metaclass, which **collides** with Apple's
co-resident `AppleUSBMultitouch.kext` (loaded for MT1s; our genuine path `allocClassWithName`s from it) — a
same-name metaclass conflict he sidesteps by *replacing* Apple's driver in the Wellspring-internal context.
Resolving that co-residence is the prerequisite to adopting owned-USB here. **QUEUED (user, 2026-07-16): return
to owned-USB via this route once owned-BT is where we want it** — see `usb-decisions.md` §4. His reusable
"plumb frames upward + name-wearing simulator" is the portable OS-publish backend the merge-map predicted
([[genuine-vs-owned-device-reeval]]); he is posting it on GitHub. His device is fed by VoodooInput satellites,
so a real MT2 needs *our* front-half bus driver — complementary, not drop-in.

**DECISION (2026-07-16, user):** **USB = genuine** (Apple's real `AppleUSBMultitouchDriver` started on the MT2
+ interpose/translate — recover the retired genuine-USB path, as synthetic-BT was recovered); **BT = synthetic**
(current fabricated-AMD + direct L2CAP listener — works, clean teardown). A per-transport terminal split.
- **Roads eliminated for USB:** ② subclass (this panic); ③ device-emulate (futile — the AMD is spawned only by
  the `IOUSBInterface` transport driver, not the HID path; see the terminal-design-space section).
- **Generalization preserved (the mission constraint holds):** both are *publish-backends* behind one shared
  device-read/conditioning seam — a future device plugs into the read seam regardless of transport. Genuine-USB
  is a *generic* backend ("make Apple's driver bind+drive it, translate its reports"), not a per-device special
  case. The implementation-layer inconsistency (genuine USB / synthetic BT) mirrors Apple's own 10.9 stack
  asymmetry (USB `IOUSBHIDDriver`/`IOHIDDevice` monolith vs BT `BNBTrackpadDevice`/L2CAP clean seams); the
  consistency that matters is at the interface layer. Re-accepts the genuine-USB tax (the getReport churn
  panic — already HARDENED `22bbf1a`; ledger in [[genuine-vs-owned-device-reeval]] / the REPLACE entry).

**IMPLEMENTED + ON-DEVICE VALIDATED (2026-07-16).** The genuine-USB terminal was recovered from `9573611^`
(last genuine `start()`) and reconciled onto HEAD: OUR `com_schmonz_MT2USBReader` is the sole matcher of the
MT2's `IOUSBInterface` (PID 613 / 0x265 is not in Apple's `idProductArray`), then manual-starts Apple's genuine
`AppleUSBMultitouchDriver` and instance-scoped-clones its `handleReport` (vtable byte offset `0x8b8` = slot 279,
**re-verified this session** against the live `/S/L/E` driver via `re vtable`; button `handleButton` @ `0xb28`).
Each MT2 `0x02` report → `mt2_usb_decode` → shared session (`mt2_policy_default`) → `kUsbSink` MT1-encodes +
checksums + chains Apple's original `handleReport`; Apple's recognizer does the gesture work. Reconcile deltas
vs `9573611^`: the shared `gh_default_adapter` (only USB used it) was dropped and its seven generic manual-start
ops inlined as file-static in `MT2USBReader.cpp`; interpose migrated to the declarative `mt2_splice_kext` row
(`kUsbHandleReportRow`). Validated live on USB: gestures/clicks good; **Trackpad prefpane shows the full native
UI** (Point&Click / Scroll&Zoom / More Gestures / About, battery 100%) — the payoff synthetic-USB could not
deliver; **teardown clean** (unload → both our reader AND the manual-started AMD go to count 0, `MT2USBReader:
stopped`, no orphaned AMD, no panic); reload re-establishes cleanly (1 reader + 1 AMD each cycle, still
responsive, no re-enumerate needed). BT reader unchanged (still synthetic fabricated-AMD). Commits `6ce9e37`
(genuine_host core + test) + `ea82e2a` (genuine reader) on `main`. The recovered code is byte-for-byte the
previously-validated genuine-USB path (interfaces did not drift); the only novelty is the reconcile, covered by
the 32/32 host suite (incl. `test_genuine_host`) + this on-device run.

### Own / bare `IOHIDDevice` for the prefpane — *not functioning*
To light the prefpane we tried publishing our own `IOHIDDevice` of the matched class. Publishing an
**un-started** `IOHIDDevice` null-derefs in `_publishDeviceNotificationHandler` → kernel panic. You
can't `registerService()` a bare nub of that class. (Led to: manually start a *genuine*
`BNBTrackpadDevice` instead, which is a real started IOHIDDevice.) **CONFIRMED again on-device 2026-07-16**
by the ② subclass attempt above — same fault, same handler.

### REPLACE — drop genuine BNB, own the device outright — *not desirable*
A clean reimplementation that owns the BT channels + input was viable, but the user pinned **stock
Apple prefpane = mandatory**, and the pane matches `BNBTrackpadDevice`. Dropping genuine BNB loses the
pane. **Reopening criterion:** if "stock pane mandatory" were relaxed (e.g. an own-pane workstream
shipped), REPLACE becomes viable again.

**Accumulating genuine-reuse tax on USB (ammunition for REPLACE — don't relitigate these items
individually; weigh them here).** BT hands us *clean seams*: `BNBTrackpadDevice` delegates through a
`BluetoothMultitouchTransport` we can interpose, and we observe input frames independently of the AMD —
so BT gets condition-waits and clean geometry answers. USB's `AppleUSBMultitouchDriver` is **monolithic**:
it does its own USB `DeviceRequest`s through private, stripped, non-virtual helpers
(`_getFeatureReportInfo`/`_deviceGetReport`/`configureDataMode`) and owns the interrupt pipe. No seam.
That forces two cruder bridges, each a settled decision:

1. **USB enable settle (`MT2_USB_ENABLE_SETTLE_MS = 50`) — KEEP, proven necessary (2026-07-04).** After
   the multitouch-enable (which ACKs immediately but switches mode *async*), we must wait before starting
   the AMD, or its `configureDataMode` storms a still-mouse-mode device with failing GET_REPORTs
   (`0xe000404f`) — the substrate of the `!pageList phys_addr` panic. On-device evidence: reorder-only
   (no settle) *reproduces* the storm (0x28 mouse packets + a 0xc8 retry loop, 24 vs 17 stalls); reorder
   + 50 ms is clean. The *principled* fix is a condition-wait — exactly what BT reconnect-v2 does
   (`retry-until-first-frame`, `gSteadyConn == gConnId`) — but on USB the first `0x31` frame arrives
   *through* the pipe the AMD owns, so watching for it pre-AMD means claiming the interrupt pipe
   ourselves first: more code + owning the pipe, for a 50 ms win. Not worth it under genuine-reuse;
   *trivial* under REPLACE (we'd own the enable→configure→first-frame sequence natively). **Don't
   re-propose deleting the settle.**

2. **USB geometry-probe residual — LIVE WITH IT; interpose (Option B) declined (2026-07-04).** On USB the
   AMD probes the device for geometry reports (`0xd0/d1/d3/d9/a1`) via `_getFeatureReportInfo`; the device
   lacks them (`MaxFeatureReportSize = 1`) so they fail `0xe000404f` — harmless (we seed geometry via
   `usb_build_init_props`, one source shared with BT's `mt2_fill_geometry_report`). BT interposes these
   away on its transport; USB has no transport to interpose, so "answering" them would need
   binary-patching a private fn or hooking the shared `IOUSBInterface` — more code + risk, no safety win.
   Under REPLACE they'd simply never be issued.

**The pattern (the real REPLACE ammunition):** every USB bridge is cruder than its BT twin because we
reuse Apple's *monolithic* USB driver instead of a delegating one. The geometry *data* is already unified
in `src/mt2_geometry.c`; the *injection/timing* can't be, because Apple's two drivers differ. Each new
such tax (the settle, the geometry residual, and whatever comes next) is a data point that an OWNED USB
driver would erase — weigh the ledger here, not the individual items.

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

### Yield BNB's PSM-19 delegate; don't double-own it — *the Path-A delegate-conflict panic* (RE 2026-06-20)
Why the shipped architecture lets genuine BNB own the interrupt channel and we only INTERPOSE its delegate
(overwrite `channel+0x110`, keep `+0x118` target), rather than `listenAt`-ing PSM 19 ourselves:
`IOBluetoothL2CAPChannel::listenAt` has a **single-owner check on `channel+0x118`** — a second owner gets
`0xe00002bc` ("owner does not match new owner"). If OUR reader holds that delegate slot, BNB's
`prepInterruptChannelWL → listenAt` fails → `closeDownServices` → the **5s restart loop** re-enters
`handleStart` → page-fault in `createCommandGate+0x17` on a now-null provider → **panic** (a distinct vector
from the bare-un-started-nub panic above). So: let BNB win PSM 19, then steal its delegate fn-ptr after the
channel is up. (Teardown restores `+0x110`/`+0x118` before releasing — see `reference.md` ordered teardown.)

### Trackpad pref-pane art/animation can't tell MT2 from MT1 — a swap would need a userspace file-swapper (RE 2026-06-20)
Distinct from the **Bluetooth**-pane identity work (name/picture, in `explanation.md`): the **Trackpad**
pref-pane's art path has **no ProductID/model/VID-PID compare**. `_pickMovie` (@0x4761) branches on a single
boolean `mMagicTrackpadFound` (@0x478b), fed only by IOServiceMatching *presence* booleans (BNBTrackpadDevice
@0x2335/0x38b6/0x4ce1, AppleUSBMultitouchDriver @0x23ac, `com.apple.AppleMultitouchTrackpad` @0x23db). ⇒ an
"MT1-vs-MT2 art" conditional cannot live in the pane OR our kext; it would have to be a **userspace LaunchAgent
that swaps on-disk asset files by name** (10.9 has no SIP, so the files are writable). Asset→state map:
no-device → `NoTrackpad.nib` / `TrackpadPicture.png` (the half-MT1/half-MT2 target image); onboarding →
`MTTrackpadController.nib` / `trackpad_remove_cover.png` / `trackpad_install_battery.png`; gesture demo →
`BTTrackpad.mov` + `BTTrackpad.xml`. Animation contract: `.mov` + an `.xml` plist of `{name, startTime:CMTime}`,
**23.976 fps, 27 shots, bound by CHAPTER NAME** (gesture rows carry `moviechaptername`; times read from the
`.xml`, not hardcoded) → shipping our own `.mov`/`.xml` that keeps the referenced shot names (Shots
3/4/6/7/8/15/16/17) needs no change to the existing gesture XMLs. **Not pursued** (cosmetic; deferred) —
recorded so the RE isn't lost.
**UPDATE 2026-06-30:** the "would have to be a LaunchAgent that swaps on-disk files" conclusion predates
our osax (shipped 2026-06-29). The osax already injects into System Preferences and swizzles the Trackpad
pane, so a MT2-accurate art swap can be done IN-PROCESS there (swizzle `setAVAsset:` / the image getter to
return our `NSImage`/`AVAsset`) — no on-disk file mutation, reversible, per-process. Preferred over the
file-swapper for the *Trackpad* pane (the osax loads only in System Prefs, which is where that pane lives).
Contrast the *Bluetooth*-pane picture (`explanation.md`), which renders in THREE processes (System Prefs +
`BluetoothUIServer` + menu extra), so the osax alone is only a partial fix there.

### Prefpane BT<->USB "video blink" — root cause + fix (RE 2026-06-30, commit 0f68531)
The remaining SECONDARY flicker (a blink in the gesture-demo video area on a BT<->USB switch) was NOT a
cross-fade or a render glitch — it's loadMainView reloading the movie. Findings (tools/re disasm of
`/System/Library/PreferencePanes/Trackpad.prefPane`, + an instrumented osax spike):
- **`-[Trackpad loadMainView]` (@0x225d) ALWAYS rebuilds**: detect (synchronous IOServiceGetMatchingService
  for BNBTrackpadDevice/AppleUSBMultitouchDriver + an IOPropertyMatch on com.apple.AppleMultitouchTrackpad) →
  `_controlerForNIBName:` → `_loadControler:` → `setMainView:`. It has NO "view unchanged" guard.
- **BT and USB BOTH use the `MTTrackpadController` view** (BT just adds the battery UI). So a BT<->USB change
  rebuilds the SAME view type, and the new view's gesture movie re-initializes → the blink.
- `_loadControler:` (@0x2b90) does a cross-fade (`NSPrefCrossFadeView` + `setFrame:display:animate:YES`, gated
  on the old view having a window) AND its own `setMainView:`, and loadMainView calls `setMainView:` again at
  its tail — so each loadMainView = TWO installs. **Both the cross-fade and the double-install were RULED OUT
  as the blink** (suppressing the fade did nothing; de-duping to one install didn't help). The blink is the
  single rebuild's movie reload.
- **The pane's controller updates in place on its own:** `-[BaseTrackPadController _magicTrackpadAction:
  deviceConnected:]` (@0x4c57) does `setHidden:` (battery), `setGesturesArray:`, and **`setAVAsset:`** (the
  movie) — independent of loadMainView. It reacts to BT-DISCONNECT in ~200ms (hides the battery). So driving
  loadMainView on a BT<->USB change was pure waste (rebuild + movie reload) on top of the controller's clean
  in-place update.
- **FIX (the guard):** skip loadMainView when a device is present AND the trackpad view is already up; full
  rebuild only on a real view-type change (<->NoTrackpad). "Trackpad view up" = current controller's
  `nibFileName != "NoTrackpad"` — the controller is a generic `InputDeviceNibController` (class can't tell
  views apart), `nibFileName` returns nil for the trackpad nib and "NoTrackpad" only for the no-device view.
  On-device: BT<->USB now blink-free, battery/UI still correct.

### USB enumeration timing — measured (2026-06-30); the removal window can't be shortened
Measured on a BT->USB handoff (cmake-build/usb_appear_timer poller, t=0 at BT-drop): AppleUSBMultitouchDriver
matchable at **~958ms**, raw IOUSBDevice (VID 1452/PID 613) at **~978ms** — the raw device is NOT faster than
the driver (the earlier guess that it would be is FALSE). So the ~1s is a genuine hardware enumeration floor
(extends the prior "IOUSBDevice busy 1107ms"). The prefpane removal-settle window (REMOVE_CHECK_MS=1300ms)
can't be meaningfully shortened: at removal time the incoming USB device isn't yet enumerable, so no fast
check can tell a genuine BT power-off from a BT->USB handoff. **Open cosmetic:** a genuine BT power-off
briefly shows Apple's battery-hidden "looks like USB" in-place state (the controller hides the battery ~200ms
after BT drops) until the 1.3s window elapses → NoTrackpad. The only fix would be to suppress that in-place
battery-hide DURING the removal window and re-apply it if it resolves to USB (invasive; risks reintroducing
the blink) — deferred. Apple's own pane has no live USB observer at all, so this is already past Apple.

### Prefpane power-off "linger" — RE the in-place battery-hide + re-apply path (2026-06-30, Phase 0; GO)
RE'ing the deferred fix above (suppress Apple's in-place battery-hide during the removal window, re-apply on a
handoff). Read-only disasm of `/System/Library/PreferencePanes/Trackpad.prefPane/Contents/MacOS/Trackpad` via
`tools/re`. **GO/NO-GO gate: GO** — all three questions resolve cleanly.
- **Trigger + signature (0.1).** `-[BaseTrackPadController _magicTrackpadAction:deviceConnected:]` (@0x4c57) is
  the ONLY thing that hides the battery *control* (the "USB look"). Signature confirmed `(id self, SEL _cmd,
  id arg1, signed char connected)`: `connected` arrives in `%rcx`/`%r15b` (sign-extended `movsbl` at +0x3c =
  BOOL/`signed char`); `arg1` in `%rdx`/`%r14` (receives `armIterators`). At 0x4d13 it does `setHidden:` with
  `al = sete(connected==0)` on `mBTBatteryControl`/`mBTBatteryControlLabel`/`mBTBatteryLabel` (+ `mChangeBattery
  Button`), then the full in-place rebuild (`setAVAsset:nil`, `setGesturesArray:nil`, `_buildGestureArrayAndSel
  ectGestureWithName:`, two availability notifications). So suppressing the whole method holds battery + gestures
  + movie together. **Subtlety:** on `connected:NO` it hides the battery ONLY if no `BNBTrackpadDevice` still
  matches in IORegistry (+0x6d..+0x9c); if one is still present it just calls `_checkBatteryTimer:` and returns.
  `calls 0x4c57` shows NO static call site → it's `objc_msgSend`-dispatched from the controller's OWN IOService
  observer (re-arms via `armIterators` at the top), independent of our recompute — matches the on-device finding.
  Implementing class is `BaseTrackPadController` (runtime `self` is the `MTTrackpadController` subclass via its
  ivars) → swizzle `BaseTrackPadController`; `class_getInstanceMethod` finds the inherited IMP.
- **Re-apply path = REPLAY the suppressed call (0.2; the crux).** Save `(self, arg1, connected)` from the
  no-op'd call; at handoff resolution re-invoke the original IMP with them. At that point BT is gone, so the
  `connected:NO` path finds no `BNBTrackpadDevice` → falls to 0x4d13 → `setHidden:YES` (battery gone = correct
  USB look). This is byte-for-byte today's working handoff, just deferred — no extra RE, candidate #1 holds.
- **No second hide path (0.3).** `_checkBatteryTimer:` (@0x4988) never hides the battery *control* — only
  `setFloatValue:`/`setStringValue:` (battery value) + `setHidden:` on `mChangeBatteryButton`; it early-returns
  if `mMagicTrackpadFound==0`, and otherwise no-ops when `IOBluetoothDevice pairedDevices` has no connected
  classMinor==0x25 device (true while BT is off). `_updateBTDevice:` (@0x4918) only schedules the repeating
  `mBatteryCheckTimer` that fires that same harmless `_checkBatteryTimer:`. So gating `_magicTrackpadAction`
  alone fully holds the view; the battery timers can't disturb it mid-window.

**How we'd know if this RE is wrong:** Phase 2 on-device — if power-off still flashes the USB look, the
suppression missed a hide path (re-instrument); if the handoff loses battery-correctness or re-blinks, the
replay is wrong. Falsification attempted: disassembled both battery-timer paths to rule out a second hide;
residual risk = a hide path reachable only at runtime (e.g. a KVO/notification observer not visible in static
disasm) — Phase 2 is the oracle.

### Prefpane MTTrackpadController handle — find_mt_controller (RE 2026-06-30, on-device PROBE)
For the transport-state-machine refactor the osax must call `_magicTrackpadAction` on the live
`MTTrackpadController` (to set battery deterministically), but `mCurrentController` is the generic
`InputDeviceNibController` (PROBE: `isMT=0`) and does NOT respond to `_allControllers`. Two deterministic
accessors confirmed live (PROBE on USB):
- `mCurrentController.mController` ivar = the `MTTrackpadController` (the nib controller retains its content
  controller). This is the CURRENT view's controller — preferred (no singleton-staleness risk).
- `+[MTTrackpadController sharedController]` returns the `MTTrackpadController` singleton (isMT=1). Good
  fallback. (`+[BaseTrackPadController sharedController]` does NOT respond — the accessor is on the subclass.)
`find_mt_controller()` = read `mCurrentController.mController` (verify isKindOfClass:MTTrackpadController),
fall back to `[MTTrackpadController sharedController]`. Other nib-controller ivars seen: `mContentView`(NSView),
`mSetupBTButton`(NSButton), `mSetupBTBackButton`(nil).

### Let PackageKit version-gate our pkg bundle components — *not functioning* (2026-07-10)
`pkgbuild` defaults every bundle component to `BundleIsVersionChecked=true`, so at install time
PackageKit compares the pkg's bundle `CFBundleVersion` against what's on disk and **SKIPS the component
if the installed one is >= the pkg's**. Our kext was hardcoded `CFBundleVersion 1.0.0` for its whole
life, and `1.0.0` sorts **above** every `0.4.x`, so the installer skipped the kext on *every* install —
the driver itself never updated for anyone who already had it (we only missed it because dev
hand-reloads the kext). Proven in `/var/log/install.log` during the 0.4.4 dogfood:
`PackageKit: Skipping component "com.schmonz.MT2Gesture" (0.4.4-…) because the version 1.0.0-… is
already installed`. Even after stamping the real version (`0b9dc5f`), a numeric regression (1.0.0 → 0.4.x)
still skips.

**Decision: force-install every component, never let PackageKit version-gate them.** The pkg build runs
`pkgbuild --analyze` → flips `BundleIsVersionChecked=false` on all bundles (`cmake/pkg_no_version_check.sh`)
→ `pkgbuild --component-plist`. An unsigned, loose-path (`/usr/local/lib/mt2d`) driver package should
always place its own build, not defer to a stale on-disk copy. Self-bootstrapping: the first release
carrying the fix (v0.4.5) force-installs the kext even over the legacy 1.0.0, unsticking every existing
install. Validated on-device 2026-07-10: 0.4.4→0.4.5 self-update actually moved `kextstat` to 0.4.5.
**Reopening criterion:** only reconsider if we ever sign the kext and move it to `/Library/Extensions`
(which has its own versioning/staging semantics) — then re-evaluate whether the OS should manage upgrades.

### Show update UI from the scheduled background check — *not desirable* (2026-07-10)
Sparkle 1.x `checkForUpdatesInBackground` presents its update dialog when an update EXISTS. On the daily
LaunchAgent (pane closed, nobody watching) that leaves a blocking dialog up forever — the "updater stuck
in the Dock" reports. **Decision: background is a SILENT probe** (`checkForUpdateInformation`) that calls
the delegates (so we record the About-tab "update available" hint) but shows NO UI ever, then exits; a
120 s watchdog backstops a pre-delegate hang. **Principle: background is silent, the pane hint informs,
all update dialogs are foreground/manual only.** (`4631997`, `159c36e`.) Reopening criterion: none
foreseen — a scheduled task should never block on unattended UI.

### Pref-dict Stage 2 — no accidental seed gaps; the divergences are genuine (2026-07-10)
Stage 1 (`24efb39`) shared the identical seeds + named the genuine per-transport values (parser-options
0x2F/0x27), byte-identical (proven on both transports via `ioreg` before/after). Stage 2
(diagnose-then-reconcile) diagnosed every USB-only seed on-device:
- `Driver is Ready` / `HIDDefaultBehavior="Trackpad"` — genuine USB-manual-start needs; BT's BNB provides
  equivalents; BT works fully without them.
- `MultitouchPreferences` / `TrackpadUserPreferences` — USB seeds them (no system pre-population at
  manual-start init); BT gets system-populated ones; both transports fully functional.
- `TrackpadThreeFingerDrag` — the USB node DOES publish it; the toggle shows on a fresh USB launch. Its
  live-USB drop is a query-timing race (`open-questions.md`), NOT a seed gap.
⇒ **NO accidental seed gaps to reconcile — the divergences are genuine/structural** (the genuine-reuse-tax
ledger above). Stage 2's honest outcome was documentation, not code. Reopening: only if a future device or
transport reveals a real capability gap the seeds should close.

### Presence-SM unification is faithful — verified line-by-line + full matrix (2026-07-10)
`46e8f09` was committed "validated on a BT→USB→BT happy path"; the full state-change matrix was NOT
re-walked at the time. Re-walked 2026-07-10: the shared `mt2_presence_observer` (`obs_sm_event` /
`obs_dev_changed` / `presence_observer_reconcile` / `presence_observer_create`) is a verbatim,
behaviour-identical lift of the removed inline `sm_event`/`dev_changed`/`sm_reconcile`/`arm_observer` (same
HOLD/`gen` supersession, same 4 observer specs, same synchronous perform path; `perform` untouched). The
unification is NOT the cause of the stale-video symptom. **Lesson (re-affirmed): the render/suppression seam
is NOT host-testable — walk the FULL transport matrix (`prefpane-test-runthrough.md`) after ANY change to
the observer / perform / capture path, not a happy-path glance.**

### Capture-race stale video — eager-capture the (self,arg) pair, don't re-add the blink (2026-07-10, e730175)
Root cause + fix detail in `prefpane-test-runthrough.md` "Full re-walk (2026-07-10)". Decision: rather than
re-add the forced `loadMainView` that `6231b53` removed (it blinks), read `(self,arg)` straight from the
pane's ivars just-in-time (`eager_capture_magic`: `find_mt_controller` + `mMagicTrackpadServiceObserver`) so
the faithful in-place replay is always armed before the first transition — no blink, no rebuild. Guarded by
`respondsToSelector:armIterators` (a wrong arg was the Task-5 doesNotRecognizeSelector regression).
Validated across the full matrix (0 skips every row).

### MT2 → synthetic terminal — deliberate re-open point (queued 2026-07-12)
Sub-project 2 revived the synthetic terminal consumer (fabricated `AppleMultitouchDevice` + `MT2HIDShell`,
`kext-gesture/mt2_synth_amd.*`) as the delivery target for VoodooInput satellites, exposed as a swappable
`kSynthSink` registered via `MT2Gesture::beginSyntheticTerminal`. Because the readers-engine unification
already made the terminal a `mt2_transport_sink_t`, routing the **MT2 itself** through this synthetic
terminal is now a one-line, per-reader flip: register `&kSynthSink` in a transport's
`connectionEstablished` instead of the genuine sink. **Decision (deferred, not now):** once the synthetic
terminal is on-device-proven (the sub-project-2 Phase-D oracle: hidd adopts the fabricated AMD + a fed
frame drives the cursor), evaluate the flip as a cheap A/B — synthetic vs genuine on cursor/gesture/battery
fidelity + cross-version portability. Retire a transport's genuine interpose ONLY if synthetic wins without
losing conformance; else keep genuine. This is the concrete next step of the `genuine-vs-owned-device-reeval`
insight (genuine wins on 10.9 for reuse/conformance; owned/synthetic is more portable below 10.9). Do NOT
flip a working, shipped transport before the terminal is proven (make-it-cheap-to-be-wrong).

**BUILT — the A/B knob is in (2026-07-13, `main` `f44d6d4`+`c9573c3`).** Not the flip itself: a
*flag-gated* synthetic terminal on the **BT** path so the A/B is measurable on real hardware.
`sysctl debug.mt2_bt_synth` (int, default `0`=genuine → **shipped behaviour byte-unchanged**; `1`=synthetic),
registered alongside `debug.mt2_log`/`debug.mt2_batt` (`kext-gesture/mt2_log.*`, extern `gMT2BtSynth`).
When set, `MT2BTReader` builds its OWN fabricated AMD (`mt2_synth_amd_build` under the `MT2Gesture` nub)
and registers `kBtSynthSink` (parallel to `kBtSink`; same `mt1_encode`/`handleTouchFrame`) at
`connectionEstablished` instead of BNB's genuine AMD; build-fail falls back to genuine so the MT2 is never
left dead. Teardown mirrors the genuine ordering EXACTLY: NULL `gBtSynthCtx` before `quiesceDelivery()`
(so an in-flight watchdog flush sees no AMD) and `mt2_synth_amd_teardown` only AFTER the drain (a review
caught a would-be free-before-drain UAF specific to synth mode). BNB stays the BT transport (terminal-only;
does NOT touch the reconnect-enable-fail, which needs owning the wire). Host suite 31/31, default goldens
unchanged. **PENDING (gated, user away from desk):** the on-device A/B checklist in BOTH modes — cursor ·
tap · scroll · right-click · 3/4-finger · **prefpane recognition** · **battery** · edges — recorded as the
divergence table that decides retire-genuine-or-keep-the-split. Spec/plan under `docs/superpowers/`
(transient). Do the A/B, THEN update this section with the result.

### ROOT CAUSE — genuine-path orphaned-AMD kernel panic (2026-07-15), and why it forces full-synthetic
**Panic:** `Kernel_2026-07-15-082259.panic` — NX fault at shutdown in `AppleMultitouchDevice::removeFramesClient`
(named from AppleMultitouchDriver disasm: slot 0x688=`getWorkLoop`, 0x938=`_removeFramesClient`, faulting
`callq *0x1a0(%r10)` = a workloop dispatch), process `hidd`, RIP = a stale pointer landing in our reloaded
kext's `__DATA`. So Apple's stack ran teardown on an **`AppleMultitouchDevice` whose memory was already
freed/corrupted** — `getWorkLoop()` returned garbage. Live evidence: **`AppleMultitouchDevice`=2 with
`BNBTrackpadDevice`=0** — two GENUINE AMDs orphaned with no transport. No synthetic objects existed this
session (`MT2HIDShell`=0, `VoodooInput`=0, zero `mt2_synth_amd` log lines); the session-2 synth commits are
no-ops in genuine mode. So this is the GENUINE path, not synthetic, not the A/B code.

**Failure model (the mechanism):** the genuine BT path manual-starts Apple's `BNBTrackpadDevice`
(`gh_start` → alloc/init/attach/**start outside IOKit's matching flow**, `MT2BTReader::manualStartGenuineBnb`).
Apple's `AppleMultitouchDriver` matches the BNB and spawns an `AppleMultitouchDevice` as its **client**.
Manual-starting outside matching leaves the BNB with an **unbalanced busy count** (already documented at
`MT2BTReader.cpp` stop(): *"the BNB never balances its start-time busy so a wait can never succeed"*). On
disconnect the L2CAP channel (the reader's provider) terminates → reader `stop()` → `gh_stop` → BNB
`terminate()`+`release()` with **NO waitQuiet** — but `terminate()` **can never finalize** because busy≠0, so
the BNB's stop/detach/free stalls, and **its client AMD's termination stalls with it → the AMD orphans**
(half-torn-down, still retained by Apple). Each connect/disconnect cycle repeats → orphans **accumulate**
(2 after this session's wedged-BT connect churn). At shutdown Apple runs `removeFramesClient` on an orphan →
`getWorkLoop()` on freed memory → NX fault. **Fatal amplifier:** the hot `make reload` swapped a very
different binary (deployed Jul-9 kext → Jul-15 build, +24KB `__TEXT`) into the same address range, so the
stale pointer hit non-exec data. Same-binary reloads (normal dev) mask it — which is why we'd "never seen
this": it needs orphaned AMDs AND a big-binary-delta reload, both novel this session.

**Why this forces full-synthetic (single ownership):** the defect is **split ownership** — Apple spawns the
AMD off a BNB we manual-started but can't cleanly terminate. It is INHERENT to reuse-Apple-BNB, and it does
not generalize (unknown devices have no BNB). Full-synthetic allocs OUR OWN `AppleMultitouchDevice` under OUR
nub (no manual-started BNB, no unbalanced-busy provider) so WE own start/stop/terminate → a clean teardown is
achievable. Caveat: synthetic still creates a real `AppleMultitouchDevice`, so its teardown must ALSO
finalize cleanly (deregister frames-client/workloop before free) — that is SP1's teardown-hardening. See
`docs/superpowers/specs/` (MT2→full-synthetic program: SP1 map+oracle+harden, SP2 BT own-the-wire, SP3 USB,
SP4 retire genuine).

### SP1 busy question resolved OFF-DEVICE by RE (2026-07-15) — fabricated AMD is terminatable
The open question for the hardened synthetic teardown (does `terminate()` finalize on our manually-started
fabricated `AppleMultitouchDevice`, or stall on unbalanced busy like the genuine BNB?) is answered by
disassembling `AppleMultitouchDevice` in the `AppleMultitouchDriver` binary: **`::start` is a plain
`IOService` doing SYNCHRONOUS setup** (IORecursiveLock allocs, OSArray/OSDictionary, `addMatchingNotification`
to observe event drivers) — **no `adjustBusy`, no `registerService`, no `IOCreateThread`/`thread_call`, no
async HID handshake.** `::willTerminate` and `::stop` only `IOLog` (no `waitQuiet`/blocking). Contrast the
BNB: its imbalance came from `IOHIDDevice`'s ASYNC bring-up holding busy until a callback we never get in
manual context. **Conclusion: the fabricated AMD does NOT inherit the un-terminatable-busy defect;
`terminate(kIOServiceSynchronous)` (SP1's hardened teardown) will finalize.** The RE also confirms the
VoodooInput divergence (terminate, not direct `stop()`): the AMD holds its OWN matching-notification/frames-
client/workloop state, so its `free()` (reached only via the terminate handshake) is what releases them —
the old direct `stop()` bypassed that. Residual (why on-device T6 still confirms, now expected-PASS): static
RE can't fully model the live client graph under real hidd adoption. This de-risks deferring T4/T6 while the
user is away.

### Post HID input reports (let Apple own the AMD) — NOT viable on 10.9 (spike 2026-07-13)
Re-tested the "most-Apple-reuse" synthetic model prompted by a MacRumors thread question: publish an
`IOHIDDevice` presenting the MT1 descriptor and POST `0x28` multitouch input reports, letting Apple's
stack spawn + own the `AppleMultitouchDevice` and run recognition — no fabricated AMD. Reused the
existing `src/vhid_mt1.c` (kextless `IOHIDUserDevice`) + a `/tmp/vhid_gesture` runner that `mt1_encode`s
a circling contact and `vhid_send`s it. **Result: `AppleMultitouchHIDEventDriver` attaches to the vhid
and creates an `IOHIDPointing` shell, but NO `AppleMultitouchDevice` is spawned under it and the cursor
does NOT move** — the posted `0x28` data is not consumed into pointer/gesture output (18s of frames, 6
cursor samples, zero movement). So the pure input-report path gets *adoption* but not *dispatch* on 10.9.
This matches why the shipped synthetic path fabricates a real `AppleMultitouchDevice` and feeds it
`handleTouchFrame` (0x28) directly — that is the seam the recognizer actually reads (`explanation.md`
"(0x28) Apple understands, let Apple's AppleMultitouchDevice + the MultitouchHID recognizer"). **Decision:
the fabricated-AMD + handleTouchFrame terminal (SP2/SP3) is the right synthetic model on 10.9; the
input-report/"Apple owns the AMD" variant is not a drop-in win and is NOT pursued.** Residual (not
isolated, low priority): the exact gap — device-enable handshake vs. recognizer-needs-the-AMD — wasn't
teased apart (the descriptor faithfully omits 0x28, as the real MT1 does; the real device streams after
its enable + in a context the bare vhid lacks). Consequence for the MT2→synthetic A/B: A/B genuine
against the fabricated-AMD terminal, NOT an input-report variant.
