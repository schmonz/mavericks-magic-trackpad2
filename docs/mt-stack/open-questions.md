# Open questions — things we need to understand but don't yet

Genuine known-unknowns about driving the 10.9 multitouch stack: behaviours we've *observed* but
can't yet *explain*, and things we haven't measured. Distinct from `decisions.md` (which records
choices we've already resolved). When one of these gets understood, fold the finding into
`explanation.md`/`reference.md` and remove it here (or move a closed one to `decisions.md` if it
settled a choice).

---

## Edge-clamp: frozen edge bands — CAUSE CONFIRMED = `MTSlideGesture::isBlocked`, gated on BT transport

**Symptom:** cursor freezes in a band near the pad edges (you must clutch/lift more); the cursor still
reaches all screen edges (it's relative). Up/down works in the band.

**Not coordinates (H1 + the encode-range fix are dead).** In-band wiggle: decoded `x` *varies*
(`3440→3573`) while the cursor is frozen → the device reports position fine; nothing in decode/encode
clamps. On-device, narrowing the emitted X range (`MT1_MAX_X` ±2500) did **nothing** — confirming it's
not a coordinate-range problem. (Also overturns the 2026-06-19 "device-side saturation" reading.)

**CONFIRMED CAUSE (RE, off-device):** the 10.9 `MTSlideGesture::isBlocked` edge-swipe reserve
(MultitouchHID.plugin) — a known bug where the ~2cm edge strip stays reserved (blocking 1-finger
pointer) even with the NC gesture off. Instruction-verified at the `0xf8a4` conditional:
```
0xf89a  cmpl $0x4, %r8d         ; transport == 4 (Bluetooth)?
0xf89e  jne  0xf902             ;   not BT -> skip the buggy block
0xf8a4  testb $0x20, 0xd9(%r15) ; contact in the reserve zone?
0xf8ac  jne  0xf952             ;   -> BLOCKED  (pointer frozen)
```
So the block is **gated on transport == 4**. `_MTDeviceGetTransportMethod` returns the transport cached
at `MTDevice+0x90`, set by `_mt_CachePropertiesForDevice` from the AMD's **`Transport` property string**;
only `"Bluetooth"` maps to 4 (it's the only transport string in MultitouchSupport). We publish
`Transport="Bluetooth"` always (full-BNB via `createMultitouchHandler`; fDevice/USB via `makeHidProps`),
so we hit the block on every transport.

**FIX (identified, pending on-device verify):** make the recognizer see a **non-BT transport** (anything
≠ `"Bluetooth"` → numeric ≠ 4 → `jne 0xf902` skips the buggy block). fDevice/USB path: one-line change
in `makeHidProps`. Full-BNB path: harder — the AMD's `Transport` is set by `createMultitouchHandler`
(override after creation or via the props dict; same timing concern as geometry). **Risk to verify:**
no gesture/tap/pane regression from a non-BT transport (the only transport==4-gated thing found is this
block, but confirm). **Verify:** dead zone gone + gestures OK; `ioreg` shows `Transport` ≠ Bluetooth.
Relates to [[mt2-cursor-edge-clamp]]. (Test bed: the fDevice/USB path under `kFullBnb=false`.)

---

## Cold-boot and sleep/wake flap rate — unmeasured

**Measured clean (2026-06-22):** warm BT reconnect = 0 flaps across all observed cycles (CONNTRACE
oracle, all `STEADY`), with the committed defer-0xF1 fix. **Not measured:** cold boot (where the
historical "no cursor at boot" actually lived) and sleep/wake (classically fragile). Until measured
with the same oracle, we don't know whether the flap is genuinely put to bed or merely absent in the
warm-reconnect case.

**How to measure:** after a reboot (or a sleep/wake), `sudo sysctl debug.mt2_log=1` then
`dmesg | ./re/conn-trace` → STEADY/FAIL for that boot's first connect. A `FAIL` timeline that never
reaches `INTERRUPT_BOUND` ⇒ PSM 19 didn't open ⇒ the targeted fix is `waitForChannelState(OPEN)` on
PSM 17 (with, finally, a real repro to verify against). See `reference.md` → BT connect handshake for
the genuine sequence and `how-to.md` → fix the connect flap. Relates to [[mt2-bt-attach-flap-rootcause]].

---

## Which control-channel transition does the device key on to open PSM 19?

**Known (RE'd):** PSM 19 is device-initiated, and the genuine driver provokes it only by *correctly
accepting the control channel* (`listenAt`-bound + `waitForChannelState(OPEN)`), sending no HID
command first. **Not provable by static RE:** *which exact* control-channel state transition the
device watches for — pure L2CAP OPEN vs. `listenAt`-acceptance vs. an L2CAP-config detail.

**Why it matters:** if reproducing the genuine *order* (the flap fix above) doesn't fully fix
cold-boot/sleep-wake, we won't know which sub-step we're getting wrong without seeing the wire.

**The one justified live capture:** an `hcidump`/L2CAP trace of a genuine-driver connect vs. our
reader's connect, diffed on the control-channel exchange right before the device opens PSM 19. Only
needed if reproducing the order doesn't fix the flap — not up front.
