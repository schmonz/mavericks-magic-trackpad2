# Open questions — things we need to understand but don't yet

Genuine known-unknowns about driving the 10.9 multitouch stack: behaviours we've *observed* but
can't yet *explain*, and things we haven't measured. Distinct from `decisions.md` (which records
choices we've already resolved). When one of these gets understood, fold the finding into
`explanation.md`/`reference.md` and remove it here (or move a closed one to `decisions.md` if it
settled a choice).

---

## Edge-clamp: frozen-X band near the L/R pad edges — DOWNSTREAM (recognizer normalization)

**Symptom:** cursor X freezes in a band near each L/R edge; up/down still works there. Unpleasant.

**Decisive measurement (2026-06-23, full-BNB):** an in-band finger wiggle made decoded `x` **vary**
(e.g. `3440→3573`, spread 133) while the cursor X stayed frozen (user: accepts up/down, not left/right).
So decoded `x` is faithful and *changing* in the band — the clamp is **downstream of decode/encode**,
in MultitouchSupport's report-X → position normalization, which pins `norm.x` for decoded `x` beyond
~3440 even though the device reports up to `3934`. (This **overturns** the 2026-06-19 "device-side, x
hard-stuck at 3934" reading — that doesn't reproduce under full-BNB; likely the hybrid/fDevice path or
a measurement right at the true physical edge.) **H1 (mt1_encode clamp) stays dead** — `scale()` maps
the full in-range decoded span linearly; the clamp is after encode.

**Live hypothesis — H3 (recognizer's X range too narrow):** the recognizer normalizes X against a range
narrower than our encoded span, so the outer ~13% pins to `norm.x` 0/1. Prime suspect: the **zeroed
Sensor Region** — `src/mt2_geometry.c` answers the Region Descriptor (`0xd0`) and Region Param (`0xa1`)
with all zeros, and MultitouchSupport derives coordinate/pixel constraints from them
(`_MTParseSensorRegionParam` @0x706b reads `0xa1` as 3×`u16`; `_alg_DerivePixelConstraintsPerRegion`
@0xb980, `_mt_DefineSurfaceGrid` @0xc1fa). A degenerate region → a degenerate/narrow coordinate range.
(NOTE: edge-clamp is therefore SEPARATE from the waitQuiet/flap deviceReady root — it's our published
geometry, which we control directly, not a device-config gap.)

**Discriminators (cheap):** (1) vary published `SurfaceWidth` (13000→26000) + reload — if the band
shifts, contact-norm uses the surface; if not, the region. (2) publish a non-zero region (full-range
bounds guess) — if the band shrinks, H3 confirmed; then find the correct region bytes (genuine-MT2
capture or driver sources). Also RE `_alg_DerivePixelConstraintsPerRegion` to confirm region→X-range.
**Oracle:** in-band wiggle — decoded `x` varies (kext `debug.mt2_log=2` "edge x=") while `re/mt-contacts`
`norm.x` holds constant = downstream confirmed; fixed when `norm.x` tracks decoded `x` to the edges.
Relates to [[mt2-cursor-edge-clamp]].

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
