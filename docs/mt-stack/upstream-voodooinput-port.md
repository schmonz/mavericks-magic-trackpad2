# Porting to real upstream VoodooInput (forward-looking sizing, 2026-07-19)

> **✅ PIECE 3 SHIPPED 2026-07-22 — fork (B) chosen. Upstream's verbatim `VoodooInput.cpp` IS our mux now;
> `MavericksVoodooInput` retired. See "SHIPPED" section at the bottom for the as-built record. The sizing +
> the (A)/(B) analysis below are preserved as the reasoning that led there.**

**Context.** We currently *vendor* only the 3 VoodooInput ABI headers (verbatim, SHA-pinned) and run our
OWN reimplemented mux (`com_schmonz_VoodooInput`) + fabricated-AMD terminal. The end-state ambition: fork
**real** acidanthera/VoodooInput and contribute (1) the MT2 as a genuine satellite and (2) a **Mavericks
terminal backend**, so the ecosystem gains a 10.9 target and the MT2 driver becomes upstreamable. This note
is the grounded difficulty sizing from actually reading upstream `master` (2026-07-19).

## North-star — the reconstructable end state (2026-07-22)

The way we're working toward this is deliberately *reconstructable*. Target end state: a new agent can be
pointed at this repo, told to "review how we got here," and then either —
1. **Clean-room replay:** create a fresh repo containing only the *logical* steps that actually reach this
   end point (dropping the RE detours, dead ends, and back-and-forth), or
2. **Real upstream branch:** fork acidanthera/VoodooInput, branch, and reconstruct those logical steps *there*
   as the actual contribution — **with the MT2 driver kept OUT of the PR branch.**

Two implications that steer how we build, starting now:
- **Commit hygiene = the contribution.** Each VoodooInput-facing change should be a self-contained, logically
  ordered step (vendor pristine → additive guarded seam → …), so the sequence *is* the reconstructable
  history. The `unifdef -UMAVERICKS_TERMINAL == pristine` invariant means the guarded delta of each vendored
  file is exactly the diff a PR would carry.
- **Separability: the mux must not name the MT2 driver.** Today our seam hardcodes
  `#include "…/MavericksTerminalBackend.h"` + `OSTypeAlloc(MavericksTerminalBackend)` inside `VoodooInput.cpp`.
  That's MT2-driver code reaching *into* the mux — fine for OUR fork, but it blocks "MT2 stays out of the PR
  branch." The clean end state: the mux's `< ElCapitan` branch references a **generic pre-ElCapitan terminal
  interface** (mirroring how it references `VoodooInputSimulatorDevice`), and `MavericksTerminalBackend` is
  *our* concrete implementation of that interface, living out-of-branch in the MT2 driver. Introducing that
  abstraction is the "prep the PR" step — deferred, not yet done. The current version_major-gate work still
  hardcodes the backend (correct incremental step; the guarded block stays cleanly replaceable).

## What upstream VoodooInput is

README: *"opensource trackpad aggregator kernel extension providing **Magic Trackpad 2 software emulation**
for arbitrary input sources."* So upstream's terminal literally emulates an MT2 — the same target our
owned/genuine terminals wrestle with. Structure under `VoodooInput/`:
- `VoodooInput.cpp/.hpp` — the multiplexer (matches `VoodooInputSupported`, receives `VoodooInputEvent`)
- `VoodooInputMultitouch/` — the ABI headers we vendored + more
- `VoodooInputSimulator/` — the **terminal**: `VoodooInputSimulatorDevice`, `VoodooInputActuatorDevice`
- `Trackpoint/`, `Scripts/`, `Info.plist`, `VoodooInputIDs.hpp`

## Finding 1 — dependency graph is CLEAN (no Lilu)

`Info.plist` `OSBundleLibraries` = **only Apple KPIs**: `com.apple.iokit.IOHIDFamily 2.0`,
`com.apple.kpi.iokit 14`, `com.apple.kpi.libkern 14`, `com.apple.kpi.mach 13.0`. **No Lilu, no OpenCore
entanglement** — VoodooInput proper is a standalone IOKit kext. This was the biggest unknown; it resolves
the good way. The v14 KPIs are 10.10+; lowering them to 10.9's `13.4` is a plist edit (no v14-only API use
seen). Personality: `IOClass VoodooInput`, `IOProviderClass IOService`, matches `VoodooInputSupported`,
probe score 200 — **identical to our reimplemented mux**, so our satellites bind upstream unchanged.

## Finding 2 — the terminal is the ③ shape, full-lifecycle, hardcoded but OS-gated

`VoodooInputSimulatorDevice : public IOHIDDevice`, overriding the **full lifecycle**: `newReportDescriptor`,
`getReport`/`setReport`, all identity methods (`newVendorIDNumber`/`newProductIDNumber`/`newProductString`/
`newTransportString`/`newPrimaryUsageNumber`/…), `setPowerState`, `start`/`stop`. It presents an emulated
MT2 as a **published IOHIDDevice the OS multitouch stack binds** — i.e. terminal **③** (NOT our ① fabricated-
AMD-direct). Full lifecycle ⇒ it **dodges the U1b bare-node `registerService` panic** by construction.

`VoodooInput.cpp::start` **hardcodes** it: `simulator = OSTypeAlloc(VoodooInputSimulatorDevice)` then
init/attach/start. Event seam is one clean line: `case kIOMessageVoodooInputMessage: simulator->
constructReport(*(VoodooInputEvent*)argument)`. Not pluggable today — BUT the file already does
`version_major >= kVoodooInputVersionMonterey` gating, so **upstream already branches behavior by OS
version**. A Mavericks terminal selected by `version_major` fits their existing pattern (extends a seam they
have, not a new abstraction) — big for upstreamability.

## The (a)/(b) fork, precisely located

> **U2 IS ANSWERED — NO (see `open-questions.md`, "Prefpane shows No Trackpad on synthetic USB" §, ~L874).**
> 10.9's multitouch AMD **spawns only from the `IOUSBInterface` transport driver**, NOT from a bare virtual
> IOHIDDevice. Upstream's `VoodooInputSimulatorDevice` *is* exactly a bare virtual IOHIDDevice (no USB
> transport) → it cannot get an AMD dispatched on 10.9. So **fork (a) is foreclosed** and **fork (b) is the
> answer** — which we then SHIPPED (see the SHIPPED section at the bottom). The framing below is preserved as
> the reasoning; treat "U2 unresolved" here as historical.

Whether upstream's own terminal works on 10.9 hinges on the ONE question we deferred as **U2/③** (now
answered NO, above): *does 10.9's `AppleMultitouchDriver` bind a virtual IOHIDDevice and spawn + dispatch an AMD?*

- **(a) Port their simulator as-is** — mechanical (KPI versions, 10.9 IOHIDDevice lifecycle compat, C++11 —
  our cross-build already does all of this). If ③ dispatches on 10.9 → their terminal ports beautifully +
  native gestures for free. If not → dead-ends into the class-name path (= the owned-USB metaclass collision
  we rejected; note their simulator presents MT2 identity via HID props/VID 0x05ac PID 0x0265, NOT via the
  `AppleUSBMultitouchDriver` class name the 10.9 pane's `IOObjectConformsTo` checks — so pane recognition is
  a separate open question, largely mooted by our osax-pane plan covering both transports anyway).
- **(b) Add our fabricated-AMD-direct as a SECOND terminal backend** — slot into their existing
  `version_major` gate (`< ElCapitan → MavericksAMDTerminal : VoodooInputSimulatorDevice`). Uses our PROVEN
  mechanism (`mt2_synth_amd` + `mt1_encode`, `constructReport → handleTouchFrame`), sidesteps the ③ risk, and
  is a clean upstream contribution.

## Effort sizing

- **Compile for 10.9:** small / mechanical — no Lilu is the gift; our repo already has the 10.9 SDK cross-
  build, kext KPI machinery, AppleClang gate, C++11 (`nullptr` etc. fine with AppleClang 6.0 on 10.9).
- **Terminal WORKS on 10.9:** settled — U2 answered NO (above), so fork (b) is the terminal, and it's the
  PROVEN fabricated-AMD (`MavericksTerminalBackend`), already shipped and USB-validated.
- **MT2 satellite itself:** nearly free — already speaks the vendored ABI verbatim.
- **Ally:** iphone2g&3gfan's Wellspring backend is converging on the same upstream — join forces.

**Net: U2 is answered (NO) — fork (b) shipped. The remaining diff-reduction is structural, not a risk:
convert our `MAVERICKS_TERMINAL` build-macro seam into upstream's own `version_major` runtime gate
(`< kVoodooInputVersionElCapitan → MavericksTerminalBackend : VoodooInputSimulatorDevice`), which requires
vendoring + 10.9-compiling upstream's simulator/actuator/trackpoint so both branches exist. That turns
`VoodooInput.cpp` from a private fork-with-a-flag into a mergeable upstream patch.**

Source read: acidanthera/VoodooInput `master`, `VoodooInput/{Info.plist, VoodooInput.cpp, VoodooInput.hpp,
VoodooInputSimulator/VoodooInputSimulatorDevice.hpp}`, 2026-07-19.

## Adopting upstream's VoodooInput.cpp verbatim — what the REAL source reveals (2026-07-21)

Fetched acidanthera/VoodooInput `master` `VoodooInput.{cpp,hpp}` (218/54 lines) to plan piece 3 (run
upstream's mux with our `MavericksTerminalBackend` as the `< ElCapitan` terminal). Three things the design
sketch missed, from the actual code:

1. **The mux is ENTANGLED with the whole simulator/actuator/trackpoint subsystem.** `start()` unconditionally
   `OSTypeAlloc`s `VoodooInputSimulatorDevice` + `VoodooInputActuatorDevice` + `TrackpointDevice` and
   inits/attaches/starts all three; `message()` calls `simulator->constructReport(...)`,
   `trackpoint->updateRelativePointer/…`. To compile+link "verbatim" we must vendor AND 10.9-port ALL THREE
   — including `VoodooInputSimulatorDevice`, the IOHIDDevice ③ terminal whose 10.9 spawn/dispatch is the
   still-open U2 question — even though our 10.9 build never uses them. Alternative: `#ifdef` them out for
   the 10.9 build (then it's not verbatim). This materially enlarges piece 3 vs the "just add a version
   branch" sketch. → the "verbatim mux" decision should be revisited with this cost known.
2. **`start()` calls `updateProperties()` which HARD-REQUIRES 5 provider keys** (`VOODOO_INPUT_TRANSFORM_KEY`,
   `LOGICAL_MAX_X/Y_KEY`, `PHYSICAL_MAX_X/Y_KEY`); missing any → `updateProperties` false → `start` returns
   false → mux never starts. **Our satellites advertise only `LOGICAL_MAX_X/Y`.** Adopting upstream's mux
   requires our satellites to also advertise `TRANSFORM_KEY` + `PHYSICAL_MAX_X/Y` (a small satellite change,
   but required).
3. **The mux `open()`s its provider** (`parentProvider->open(this)`) and closes on `willTerminate`. Our
   satellites don't expect to be opened; their teardown must tolerate it.
4. Confirmed GOOD: upstream's `start()` DOES `setProperty(VOODOO_INPUT_IDENTIFIER, kOSBooleanTrue)` — so our
   satellites' mux-locator works unchanged. And `version_major` is used ONLY in `VoodooInputGetProductId`
   (>= Monterey), NOT for terminal selection — so our seam ADDS a terminal-gate, it doesn't extend one.

Net: piece 3 is bigger + more coupled than designed. Decide the fork before planning: (A) vendor+10.9-port
the full simulator/actuator/trackpoint subsystem (heavy; reopens U2 for the simulator's compile), or (B)
vendor VoodooInput.cpp with a 10.9 `#ifdef`/seam that excludes the unused subsystem (small, but the shipped
file diverges from upstream — though the CONTRIBUTED diff to upstream is still just the `< ElCapitan` branch).

## SHIPPED — fork (B), verbatim mux with a `MAVERICKS_TERMINAL` seam (2026-07-22)

Chose **(B)**. Upstream's `VoodooInput.cpp/.hpp` are vendored in `third_party/VoodooInput/` (SHA-pinned
`d897813`, the same pin as the ABI headers) and compiled as the kext's mux; our hand-written
`MavericksVoodooInput.{cpp,h}` is deleted. A build macro `MAVERICKS_TERMINAL` gates the file:

- `#ifdef MAVERICKS_TERMINAL` → the mux holds a `MavericksTerminalBackend* backend`, allocs/starts it in
  `start()`, routes `kIOMessageVoodooInputMessage` to `backend->handleEvent()`, feeds
  `kIOMessageVoodooInputUpdateDimensionsMessage` to `backend->updateDimensions()`, tears down in `stop()`,
  and exposes `publishBattery()` (the BT reader's battery bridge calls it). The four trackpoint cases are
  `#ifndef`-excluded.
- `#else` → upstream's simulator/actuator/trackpoint path, **byte-identical to pristine**. Verified with
  `unifdef -UMAVERICKS_TERMINAL … | diff` against the pristine vendor commit — empty. **That guaranteed-clean
  diff (guard directives + guarded `< ElCapitan` code only) IS the contributable-upstream artifact.**

**The three predicted requirements, all handled:**
1. Subsystem coupling → resolved by the `#ifdef` seam (fork B); we did NOT vendor/port
   simulator/actuator/trackpoint.
2. 5-key `updateProperties()` gate → both satellites now also advertise `VOODOO_INPUT_TRANSFORM_KEY` (0),
   `VOODOO_INPUT_PHYSICAL_MAX_X/Y_KEY` (= `MT2_SPAN_X/Y`, mirroring logical since our path never reads
   physical) alongside the pre-existing logical-max keys. Confirmed at runtime — the mux `start()`s.
3. Mux `open()`s its provider → **audited as a no-op**: neither reader overrides `handleOpen/Close`, asserts
   on `isOpen()`, or self-terminates; the mux (a client) closes before the reader (its provider) at teardown
   by IOKit ordering; the USB reader's own `fIntf->open()` is on a *different* service (Apple's interface),
   no collision. No code change needed.

Also folded in: the outbound `VoodooInputEvent` transducer is now fully populated
(`isValid/type=FINGER/fingerType/supportsPressure/timestamp=0/previousCoordinates`) in
`mavericks_voodoo_from_frame`, so the SAME satellites can drive upstream's simulator terminal on newer OSes;
our 10.9 backend ignores these fields (it consumes the *inbound* `frame_from_voodoo`), so no on-device change.

**As-built:** kext IOClass `VoodooInput` (plist), target-wide `-DMAVERICKS_TERMINAL` (every TU that includes
`VoodooInput.hpp` — notably `MT2BTReader.cpp` calling `publishBattery` — must see the same class layout, or
ODR/layout mismatch). Commits `59e69c5` (pristine vendor) → `f226695` (seam+satellites+CMake+retire) →
`ee204ab` (transducer completeness) on `main`.

**Validation:** 31/31 host tests green; on-device USB confirmed — `ioclasscount VoodooInput = 1`,
`MT2USBReader = 1`, `MavericksVoodooInput = <no such class>`; clean-start log (no "could not open" / "could
not get provider properties" / "backend start failed"); user confirmed cursor + gestures + clean
unplug/replug (the teardown-ordering panic candidate). **⚠️ BT path NOT exercised** (device was on USB;
`MT2BTReader = 0`) — the BT reader binding + the retyped `gBtMux` battery bridge are shared-code but
unvalidated at runtime. Follow-up: BT cold-boot + battery-%-in-pane check.
