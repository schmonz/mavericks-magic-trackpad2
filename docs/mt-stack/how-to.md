# How-to — working on the 10.9 multitouch stack

Recipes. For the model see `explanation.md`; for facts see `reference.md`.

## Drive a new behavior end-to-end

1. **Decode** the relevant MT2 report field (extend `src/mt2_decode.c`; it's pure C, host-testable).
2. **Condition** it through the shared session if it's stream-shaped (`src/mt2_session.c` lifecycle),
   or handle it as an edge (like the click sink).
3. **Feed** Apple's AMD: translate to the MT1 `0x28` report (`src/mt1_encode.c`) → `handleTouchFrame`,
   or post an event (`handlePointerEventFromDevice`).
4. **Inject a side-channel** only if Apple's path can't obtain it for this device (geometry, the
   button gate are the two precedents — both done by making Apple's own query/bring-up succeed, not
   by reimplementing).
5. **Verify on-device** (host-green is necessary but not sufficient — the effect must be observed in
   the real system).

## Dev-loop (reload without reboot)

```sh
# 1. pad OFF first (unloading while it streams risks a UAF panic)
sudo kextunload -b com.schmonz.MT2Gesture
# 2. build + stage + load
make -C kext-gesture load
# 3. pad ON
```

- **Full-BNB needs ABM loaded**, or `allocClassWithName("BNBTrackpadDevice")` returns NULL:
  `sudo kextload /System/Library/Extensions/AppleBluetoothMultitouch.kext`
- **Always verify the loaded binary contains your edit** before trusting a test (stale `.o` has
  burned us): `strings kext-gesture/MT2Gesture.kext/Contents/MacOS/MT2Gesture | grep '<your string>'`
- **`dmesg` accumulates across reloads** — mark the line count (`sudo dmesg | grep -c CONNTRACE`)
  before a run so you read only new lines.
- If a load fails with **kextload error 71**, it's usually an unresolved symbol / missing
  `OSBundleLibraries` entry (`sudo kextutil -n -t /tmp/MT2Gesture.kext` shows which).

## Watch the live stack (runtime diagnostics)

```sh
sudo sysctl -w debug.mt2_log=1   # milestones + CONNTRACE
sudo sysctl -w debug.mt2_log=2   # verbose: per-report geometry GET/INFO, per-edge clicks
sudo sysctl -w debug.mt2_log=0   # silence (default)
```

Connect-flap verdict (after a connect cycle / reboot):
```sh
sudo dmesg | ./re/conn-trace      # per-connection timeline + STEADY/FAIL
```
A `FAIL` that never reaches `INTERRUPT_BOUND` ⇒ PSM 19 didn't open ⇒ the targeted fix is
`waitForChannelState(OPEN)` on PSM 17 (then a real repro exists to verify against).

## Fix the connect flap (when cold-boot/sleep-wake measurement shows one)

The defer-0xF1 fix already in the tree handles warm reconnect. If the CONNTRACE oracle shows a
`FAIL` that stalls before `INTERRUPT_BOUND` (PSM 19 never opened), reproduce the genuine
control-channel acceptance (see `reference.md` → BT connect handshake; constants in `src/mt2_stack.h`):

1. In `MT2BTReader::start`, after winning the control channel: `listenAt(control, cb)` then
   `waitForChannelState(OPEN)` (`MT2_L2CAP_VT_waitForChannelState`, arg `MT2_L2CAP_STATE_OPEN`)
   **before** anything else.
2. Keep deferring `0xF1`; move SET_PROTOCOL (`MT2_HIDP_SET_PROTOCOL | bit`, inverted for 05AC:0309)
   + enable to a deviceReady-equivalent — only after **both** channels are OPEN.
3. Treat the PSM-19 channel with the wait/accept ordering, not identically to control.
4. Do **not** open PSM 19 ourselves — it's device-initiated.

Verify with the same oracle: re-run the failing scenario, expect the timeline to reach `STEADY`.
If reproducing the order does **not** fix it, take the one justified live capture (see
`open-questions.md`).

## Safety before any kext load

- **Assess panic risk and get go/no-go first** — passwordless sudo is not authorization to proceed.
- **Pad off before unload** (use-after-free if the channel is still streaming into our shim).
- Reversible changes to shared Apple state must be undone in `stop()` (vtable restore, delegate
  restore, listenAt(NULL)) **in-gate, before release**.

## Add or verify a fact

Each constant in `src/mt2_stack.h` carries its `re/` command. To check one after a point release,
run it and compare; fix the header if it moved. Example:
```sh
re/vtable AppleBluetoothMultitouch BNBTrackpadDevice 0xcd8   # expect getMultitouchReportInfo
```
Do RE only through the in-tree `re/` wrappers (readable, allowlist-friendly), never raw otool/nm/ioreg
ad hoc. A future `re/verify-facts` could read the header and check every constant at once.
