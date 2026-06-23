# Open questions — things we need to understand but don't yet

Genuine known-unknowns about driving the 10.9 multitouch stack: behaviours we've *observed* but
can't yet *explain*, and things we haven't measured. Distinct from `decisions.md` (which records
choices we've already resolved). When one of these gets understood, fold the finding into
`explanation.md`/`reference.md` and remove it here (or move a closed one to `decisions.md` if it
settled a choice).

---

## `waitQuiet` times out on every manual-BNB teardown

**Observed (2026-06-22, ~10 warm reconnect cycles):** in `MT2BTReader::stop()`,
`fManualBnb->waitQuiet(2s)` returns `0xe00002d6` (`kIOReturnTimeout`) **every** time — the
manually-started `BNBTrackpadDevice` never reaches quiescence within the 2s bound.

**Why it's currently benign:** no panic across many teardowns. Before `terminate()`/`release()`,
`stop()` already (a) nulls our `listenAt` callback in-gate (`teardownInGate`), (b) restores BNB's
original delegate callback on the interposed channel in-gate (`restoreInGate`), and (c) quiesces +
removes the interpose timer. So in-flight L2CAP data goes to Apple's own code, not our freed shim —
clean unload rests on those in-gate restores, **not** on `waitQuiet` succeeding.

**What we don't understand:** *why* the manual BNB stays non-quiescent. Candidates: a retained
reference we don't release; an outstanding async operation (e.g. BNB's own interrupt-channel
`listenAt(NULL)`/closeChannel teardown still in flight); or `waitQuiet` simply being the wrong
readiness primitive for this object. Until we know, the 2s bound is a guess.

**How to investigate (no on-device guess-and-check):** at `terminate()` time, log the BNB retain
count and any outstanding I/O; compare against a *genuine* (IOKit-matched) BNB teardown to see what
quiesces there that doesn't for ours; confirm the interpose restore + `listenAt(NULL)` fully detach
us before `terminate()`. Relates to [[mt2-unload-while-streaming-uaf]] and the connect-hardening work.

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
