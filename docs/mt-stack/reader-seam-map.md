# Reader seam map — per-transport vs shared engine

Task-1 investigation (READ-ONLY). Maps every region of the two transport readers
(`kext-gesture/MT2BTReader.cpp`, `kext-gesture/MT2USBReader.cpp`) to one of
`PER-TRANSPORT` (the ~3%, one copy per reader) / `SHARED-ENGINE` (the ~97%, one copy
total) / `CROSS-CUTTING`, plus its intended target module. No code was moved; this
guides the later extraction tasks.

> **RESOLVED (2026-07-07, engine unification).** The "still duplicated" finding below is
> fixed: both readers now feed ONE `mt2_session` via `MavericksVoodooInputHost::submitFrame`, with the
> three per-transport behavior deltas expressed as a policy row (`mt2_session_policy_t`:
> liftoff shape / emit-empties / watchdog; rows `mt2_policy_bt` / `mt2_policy_usb` in
> `src/mt2_session.c`, which later — 2026-07-08 — converged into one `mt2_policy_default`)
> and delivery as a reader-registered `mt2_transport_sink_t`
> (BT: encode → `handleTouchFrame`; USB: encode + checksum → chained `handleReport`).
> USB's private lifecycle (`g_lc`), `usb_assemble_compactv4`/`mt2_usb_to_compactv4`, and
> the raw-byte button edge are deleted; the USB absence pump was removed 2026-07-08 when USB
> adopted the shared session's liftoff + silence watchdog. (The convergence queue — see
> `explanation.md` — completed 2026-07-08.) Byte identity was proven by a parallel-run oracle
> before the old assembly
> was deleted; `tests/test_reader_characterization.c` re-pins the engine path against the
> UNCHANGED goldens. The line-number tables below describe the PRE-unification readers —
> kept as the map that guided the extraction.

## The intended split (recap)

- **PER-TRANSPORT**: open transport, read raw MT2 report, **decode** into a contact-set
  (`touch_frame_t`), and declare MT2 config (class names, geometry, enable payload).
- **SHARED ENGINE**: consume a decoded contact-set → condition (lifecycle/density) →
  encode (`mt1_encode`, report `0x28`) → drive Apple's genuine 10.9 driver (manual-start,
  instance vtable clone, seam interpose, ordered restore). The genuine-host embryo
  (`src/genuine_host.*` + `kext-gesture/gh_default_adapter.*`) already carries the
  manual-start/interpose/teardown half.
- **SEAM**: sits between **decode** and **conditioning/encode**. The decoded contact-set is
  today a `touch_frame_t` (`src/touch_model.h`) — that struct is the de-facto
  shared contact-set the later task will formalize.

  > NOTE (2026-07-12): that boundary type was formalized and is now named **`mt2_frame`**
  > (`src/mt2_frame.h`). In this map's older prose it was called `VoodooInputEvent`; that
  > name has since been taken by the **vendored** acidanthera VoodooInput *wire* ABI
  > (`src/voodoo_wire.h` → `third_party/VoodooInput/`), which a new inbound multiplexer
  > (`kext-gesture/VoodooInputMux.cpp`) translates INTO `mt2_frame` via `mt2_frame_from_voodoo`
  > before feeding this same engine. So below, read `VoodooInputEvent` as `mt2_frame`.

---

## MT2BTReader.cpp (790 lines)

| Lines | Region | Tag | Target |
|---|---|---|---|
| 1–42 | file header + includes + VTC_ALLOC macros | CROSS-CUTTING | split by content; keep transport includes in reader, engine includes move with engine |
| 44–56 | geometry slot-index + battery-cadence `#define`s | PER-TRANSPORT | stays (BT geometry-vtable + battery are BT-only) |
| 58–59 | `extern gActiveMavericksVoodooInputHost` | SHARED-ENGINE | engine (the session/sink owner) |
| 61 | `OSDefineMetaClassAndStructors` | PER-TRANSPORT | stays |
| 63–103 | statics/globals: `gGenuineBnb`, `gInterruptReader`, interpose state (`gOrigCb/gOrigTarget/gInterposedChannel`), control-interpose (`gCtrl*`), `gBnbVtableClone/gBnbVtableCloned` | PER-TRANSPORT | stays (BT L2CAP-delegate + BNB-geometry machinery; USB has no equivalent) |
| 105–139 | `mt2_bnb_get_multitouch_report` / `_info` (geometry D-report answerers) | PER-TRANSPORT | stays (BT-only; USB seeds geometry via the init dict instead) |
| 141–145 | `bt_uptime_ms` | PER-TRANSPORT | stays (BT clock helper; USB has its own `usb_ts_22bit`) |
| 147–169 | conntrace statics (`gConnId`, `gSteadyConn`) + `bt_conntrace` | CROSS-CUTTING (BT-only) | stays in reader — measures BT's connect-flap; no USB analog |
| 171–208 | battery: `gLastBattPct/Bnb`, `mt2_publish_battery`, `mt2_maybe_publish_battery` | CROSS-CUTTING (BT-only) | stays in reader; pure parse already in `src/mt2_battery.c` (host-tested) |
| 210–218 | `mt2_diag_report_id` | CROSS-CUTTING | stays (BT diagnostic) |
| **220–266** | **`bt_interpose_shim`** — the FEED path | **SEAM** | decode stays in reader; conditioning+encode+feed → shared engine (see Seam below) |
| 268–286 | `bt_l2cap_cb_t` typedef + `bt_control_shim` (control-plane peek/forward + battery) | CROSS-CUTTING (BT-only) | stays (BT control-channel; battery only) |
| 288–327 | `setupInGate` (bind channel, arm session source, defer 0xF1 enable) | PER-TRANSPORT | stays (L2CAP channel binding); `connectionEstablished` call is the engine boundary |
| 329–370 | `bt_build_bnb_props` | PER-TRANSPORT | stays → `cfg.build_props` (BT config row) |
| 372–387 | `bt_gh_interpose`/`bt_gh_restore` + `kBtAdapter` | PER-TRANSPORT seam callbacks over SHARED defaults | stays; 7 of 9 ops are already `gh_default_*` |
| 389–481 | `start` (marshal setupInGate, `gh_start` manual-BNB, seed Product/ExtendedFeatures, arm interpose timer) | mixed | genuine-host call → engine; Product/ExtendedFeatures/battery seeding = PER-TRANSPORT (BT-only) |
| 483–494 | `teardownInGate` | PER-TRANSPORT | stays (L2CAP delegate clear) |
| 496–515 | `interposeInGate` (swap `bt_interpose_shim` onto L2CAP delegate slot) | PER-TRANSPORT | stays — **the BT input seam install** (async, not in the adapter) |
| 517–524 | `restoreInGate` | PER-TRANSPORT | stays |
| 526–544 | `controlInterposeInGate` | CROSS-CUTTING (BT-only, battery) | stays |
| 546–553 | `controlRestoreInGate` | CROSS-CUTTING (BT-only, battery) | stays |
| 555–566 | `pollBatteryInGate` | CROSS-CUTTING (BT-only, battery) | stays |
| 568–581 | `reEnableInGate` (0xF1 multitouch re-enable) | PER-TRANSPORT | stays (BT enable payload; reconnect fix) |
| 583–599 | `triggerInGate` (0x60/0x02 handler-create trigger into BNB) | PER-TRANSPORT | stays (BNB-specific bring-up) |
| 601–632 | `installBnbGeometry` (vtable clone + 2 slot overrides) | PER-TRANSPORT | stays (uses shared `vtable_clone`, but BNB geometry slots) |
| 634–639 | `removeBnbGeometry` | PER-TRANSPORT | stays |
| 641–721 | `interposeTimerFired` (phase-1 poll for interrupt channel; phase-2a re-enable retry; phase-2b battery poll) | CROSS-CUTTING (BT-only) | stays — fuses BT bring-up state machine + reconnect re-enable + battery poll |
| 723–790 | `stop` (session-source clear, teardown/restore/gh_stop) | mixed | `gh_stop` → engine; delegate/geometry/battery restores = PER-TRANSPORT |

## MT2USBReader.cpp (341 lines)

| Lines | Region | Tag | Target |
|---|---|---|---|
| 1–26 | file header + includes | CROSS-CUTTING | split by content |
| 27–40 | handleReport/handleButton slot-index + enable-settle `#define`s | PER-TRANSPORT | stays (USB vtable slots + panic-hardening settle) |
| 42–63 | statics: `gUsbVtableClone`, `gOrigUsbHandleReport`, `gLastUsbButton`, absence-pump globals (`gUsbWorkLoop/gPumpTimer/gGenuineSelf/gPumpBudget`) | mixed | clone/orig-report = PER-TRANSPORT; pump = CROSS-CUTTING (USB-only) |
| 65–69 | `usb_ts_22bit` | PER-TRANSPORT | stays (USB timestamp helper) |
| **71–111** | **`mt2_usb_handle_report`** — the FEED path (button edge, reframe, feed genuine `handleReport`, re-arm pump) | **SEAM (buried)** | decode must be split out; conditioning+encode+feed → shared engine (see Seam below) |
| 113–129 | `mt2_usb_pump_action` (post-liftoff absence-frame pump) | CROSS-CUTTING (USB-only) | stays — USB's answer to the deferred-tap-commit problem (BT uses the session's liftoff watchdog instead) |
| 131 | `OSDefineMetaClassAndStructors` | PER-TRANSPORT | stays |
| 133–143 | `start` → `startGenuine` | PER-TRANSPORT | stays (IOUSBInterface cast) |
| 145–161 | `mt2_dict_num/str/data` helpers | SHARED-ENGINE candidate | → shared dict-builder util (BT builds props by hand; could share) |
| 163–231 | `usb_build_init_props` | PER-TRANSPORT | stays → `cfg.build_props` (USB config row) |
| 233–253 | `usb_gh_interpose`/`usb_gh_restore` + `kUsbAdapter` | PER-TRANSPORT seam callbacks over SHARED defaults | stays; 7 of 9 ops already `gh_default_*` |
| 255–304 | `startGenuine` (pre-start 0xF1 enable + settle, `gh_start`, arm absence pump) | mixed | genuine-host call → engine; enable payload+settle = PER-TRANSPORT; pump = CROSS-CUTTING |
| 306–330 | `releaseInterface` (stop pump, `gh_stop`, null interface) | mixed | `gh_stop` → engine; pump teardown = CROSS-CUTTING; interface null = PER-TRANSPORT |
| 332–335 | `willTerminate` | PER-TRANSPORT | stays (USB unplug handshake; BT has no equivalent) |
| 337–341 | `stop` | PER-TRANSPORT | stays |

---

## The seam location (decode → engine)

Both readers today run **decode → lifecycle → `mt1_encode` → feed**, and both END at
`mt1_encode` producing report `0x28`. But the seam is placed differently, and the two
paths do NOT share the wiring:

### BT — seam already exists (decode in reader, engine in MavericksVoodooInputHost)
- **`bt_interpose_shim`** (MT2BTReader.cpp:220–266).
- Decode: **line 239** `int drc = mt2_bt_decode(rep, rlen, &tf);` → produces
  `touch_frame_t tf`. The `drc != 0` drop is line 243.
- **The seam is `tf`** — the decoded contact-set. It crosses into the engine at
  **line 264** `gActiveMavericksVoodooInputHost->submitFrame(gInterruptReader, &tf)`.
- Downstream (already shared, in `MavericksVoodooInputHost.cpp`): `submitFrame` → `mt2_session_frame`
  (settle gate + `mt2_lifecycle` via `fSession.lifecycle` + liftoff watchdog) → sink →
  `mt1_encode` (MavericksVoodooInputHost.cpp:61) → `handleTouchFrame(mt1,n)` on BNB's AMD
  (`fBnbTarget`, set via `setBnbTarget` at line 263). No checksum on this path.
- So BT's decode/engine split is CLEAN today; the boundary object is `touch_frame_t`.
  The refactor named that boundary `mt2_frame` (see the note above).

### USB — seam is BURIED inside a monolithic pure function
- **`mt2_usb_handle_report`** (MT2USBReader.cpp:71–111).
- The reader NEVER sees a `touch_frame_t`. It calls **line 96**
  `mt2_usb_to_compactv4(mt2, mn, ts, out, ...)`, which fuses decode + lifecycle +
  encode + checksum in one call.
- The actual decode/encode split line lives one level down, in
  `src/mt2_usb_reframe.c:mt2_usb_to_compactv4`: **line 60** `mt2_usb_decode(mt2, mt2_len,
  &frame)` produces the contact-set `frame`; **line 61** `mt2_drop_lifted`; **line 62**
  `mt2_lifecycle_step(&g_lc, …)`; **line 65** `mt1_encode(&frame, …)`; **lines 68–69**
  Apple checksum. Feed is at MT2USBReader.cpp:103 `gOrigUsbHandleReport(self, md, …)`.
- **To expose an `mt2_frame`, `mt2_usb_to_compactv4` must be split**: the reader
  should call `mt2_usb_decode` itself (→ contact-set), hand that to the shared engine,
  and the engine does drop_lifted + lifecycle + `mt1_encode`; only the USB-specific
  Apple-checksum append + descriptor-wrap + `handleReport` feed stay in the reader.

---

## Cross-cutting decisions

| Concern | Home | Reason |
|---|---|---|
| **Battery** (`mt2_publish_battery`, `mt2_maybe_publish_battery`, control poll/shim, `pollBatteryInGate`, `controlInterposeInGate`) | PER-READER (BT-only) | USB battery comes free via the genuine AMD; BT must poll GET_REPORT(0x90) on its own control channel. Pure parse already extracted to `src/mt2_battery.c` (host-tested) — the precedent. |
| **Conntrace** (`bt_conntrace`, `gConnId`, `gSteadyConn`) | PER-READER (BT-only) | Measures BT's L2CAP connect-flap state machine; USB has no multi-channel handshake to trace. |
| **Geometry** (`mt2_bnb_get_multitouch_report[_info]`, `installBnbGeometry`) | PER-READER (BT-only mechanism); constants SHARED | Both declare the SAME geometry from `src/mt2_geometry.*`, but via different mechanisms: BT answers GET_REPORT via a vtable-clone override; USB seeds the init dict. The values are one source of truth; the delivery is per-transport. |
| **Reconnect re-enable** (`reEnableInGate`, phase-2a of `interposeTimerFired`) | PER-READER (BT-only) | The 0xF1-drops-to-mouse-mode + rapid-power-cycle bug is a BT L2CAP artifact; USB re-enumerates cleanly and re-runs `startGenuine`. |
| **Control-channel interpose** (`controlInterposeInGate`, `bt_control_shim`) | PER-READER (BT-only) | Exists only to sniff battery on the second L2CAP channel; USB is single-pipe. |
| **Absence-frame pump** (`mt2_usb_pump_action`, pump globals) | PER-READER (USB-only) | USB's fix for deferred-tap-commit starvation; BT solves the same problem inside the session's liftoff watchdog. Both address the SAME concern differently → a future engine could unify "keep frames alive after liftoff." |
| **genuine-host** (manual-start, class-gate, interpose install/restore, ordered teardown) | SHARED-ENGINE (`src/genuine_host.*` + `gh_default_adapter.*`) | Already extracted; both readers use it. Each supplies only `build_props` + `interpose`/`restore`. |
| **`vtable_clone`** | SHARED util | Already shared; BT clones BNB's geometry slots, USB clones handleReport/handleButton. |
| **`mt2_coordinator`** | SHARED (no-op) | Single-transport MT2 → no-op seam; both call `mt2_coordinator_activate`. |

---

## What's already shared vs still duplicated

**Shared today:** the genuine-host lifecycle (`gh_start`/`gh_stop` + 7 `gh_default_*`
ops), `vtable_clone`, `mt2_coordinator`, the geometry CONSTANTS (`src/mt2_geometry.*`),
and — crucially — the **pure conditioning/encode primitives** `mt2_lifecycle` and
`mt1_encode` (both report-`0x28`). So the ~97% engine's *pieces* mostly exist.

**Still duplicated / the refactor's real target:** those pure primitives are **wired up
twice, differently**. BT threads decode → `submitFrame` → `mt2_session` (settle +
lifecycle + liftoff) → `mt1_encode` → `handleTouchFrame` (no checksum), living in
`MavericksVoodooInputHost.cpp`. USB threads decode → `mt2_drop_lifted` + a private static
`mt2_lifecycle g_lc` (NO session, NO settle, NO liftoff — the absence-pump substitutes)
→ `mt1_encode` → checksum → `handleReport`. So the two readers do NOT share a single
"consume a contact-set and drive Apple's driver" engine — they share leaf functions but
own separate assembly, separate lifecycle state (`fSession.lifecycle` vs `g_lc`), and
separate feed ABIs (`handleTouchFrame` vs `handleReport`+checksum).

**Biggest surprise:** contrary to the "decode→`mt1_encode`→feed inline in the shim"
framing, **only USB fuses the whole pipeline** (inside `mt2_usb_to_compactv4`); **BT
already has a clean decode/engine seam** (`touch_frame_t` handed to a separate class).
The extraction is therefore asymmetric: BT mostly needs its downstream engine (the
`MavericksVoodooInputHost` session path) *named* as the shared engine and its lifecycle state unified
with USB's; USB needs `mt2_usb_to_compactv4` *un-fused* so the reader hands a
`touch_frame_t` to that same engine. `touch_frame_t` is already the `mt2_frame`.
