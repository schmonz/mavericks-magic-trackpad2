# Porting to real upstream VoodooInput (forward-looking sizing, 2026-07-19)

**Context.** We currently *vendor* only the 3 VoodooInput ABI headers (verbatim, SHA-pinned) and run our
OWN reimplemented mux (`com_schmonz_VoodooInput`) + fabricated-AMD terminal. The end-state ambition: fork
**real** acidanthera/VoodooInput and contribute (1) the MT2 as a genuine satellite and (2) a **Mavericks
terminal backend**, so the ecosystem gains a 10.9 target and the MT2 driver becomes upstreamable. This note
is the grounded difficulty sizing from actually reading upstream `master` (2026-07-19).

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

Whether upstream's own terminal works on 10.9 hinges on the ONE unresolved question we deferred as **U2/③**:
*does 10.9's `AppleMultitouchDriver` bind a virtual IOHIDDevice and spawn + dispatch an AMD?*

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
- **Terminal WORKS on 10.9:** bounded; the only hard part is the U2/③ dispatch question we already own, and
  we already hold the proven fallback (b), which fits upstream's version-gating architecture.
- **MT2 satellite itself:** nearly free — already speaks the vendored ABI verbatim.
- **Ally:** iphone2g&3gfan's Wellspring backend is converging on the same upstream — join forces.

**Net: a moderate, mostly-mechanical port whose single risk (U2/③) we were already going to answer, with a
clean landing either way.** Do U2 (started IOHIDDevice + built-in identity + enable handshake → does an AMD
spawn/dispatch on 10.9) FIRST — its answer picks (a) vs (b).

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
