# Reference — facts for driving the 10.9 multitouch stack

Lookup tables. The **numbers live in `src/mt2_stack.h`** (build-consumed) and in the decode/encode
**code**; this file names each fact, says what it's for, and gives the `re/` command that re-derives
it from the live binaries. If a point release shifts something, run the command and fix the header —
nothing here restates a raw number that isn't also in the header or code.

See `explanation.md` for how these fit together, `how-to.md` for the workflows.

## Transport vtable slots — `BluetoothMultitouchTransport` / `BNBTrackpadDevice`

| Slot | `mt2_stack.h` | Purpose | Re-derive |
|------|---------------|---------|-----------|
| `0xcc8` | `MT2_VT_getMultitouchReport` | geometry **DATA** fetch | `tools/re vtable AppleBluetoothMultitouch BNBTrackpadDevice 0xcc8` |
| `0xcd8` | `MT2_VT_getMultitouchReportInfo` | geometry **LENGTH** probe — runs **first**; if it fails the data fetch is never reached | `tools/re vtable … 0xcd8` |
| `0xd08` | `MT2_VT_createMultitouchHandler` | spawns the AMD — **UNVERIFIED this session** | `tools/re vtable … 0xd08` |

Both `0xcc8` and `0xcd8` are stubs on the stock transport (return `kIOReturnUnsupported`); we override
**both** on our cloned transport instance (see `explanation.md` → geometry).

## AMD vtable slots — `AppleMultitouchDevice` (the geometry GET call chain)

| Slot | `mt2_stack.h` | Role | Re-derive |
|------|---------------|------|-----------|
| `0x858` | `MT2_AMD_setGetReportHandler` | installs the getReport handler | `tools/re vtable AppleMultitouchDriver AppleMultitouchDevice 0x858` |
| `0x868` | `MT2_AMD_setReportInfoHandler` | installs the reportInfo handler | `tools/re vtable … 0x868` |
| `0x880` | `MT2_AMD_getReport` | `_cacheDeviceProperties` calls this per D-report | `tools/re vtable … 0x880` |
| `0x8e8` | `MT2_AMD_deviceGetReportWithLookUp` | length-probe-then-data dispatcher | `tools/re vtable … 0x8e8` |
| `0x8f8` | `MT2_AMD_getFeatureReportInfo` | calls the handler obj → transport | `tools/re vtable … 0x8f8` |

Chain: `_cacheDeviceProperties → getReport(0x880) → _deviceGetReportWithLookUp(0x8e8) → _getFeatureReportInfo(0x8f8) → handler obj (AMD+0xa8) → transport 0xcd8 then 0xcc8`.

## Struct field offsets

| Offset | `mt2_stack.h` | Field | Re-derive |
|--------|---------------|-------|-----------|
| transport `+0x1b0` | `MT2_OFF_BNB_AMD` | the spawned `AppleMultitouchDevice*` | `tools/re xref-offset` / disasm `startMultitouchThreaded` |
| transport `+0xf0` | `MT2_OFF_BNB_INTERRUPT_CHANNEL` | `BNBDevice::_interruptChannel` | disasm BNBDevice |
| channel `+0x110` (`+0x118`=target) | `MT2_OFF_L2CAP_DELEGATE_CB` | L2CAP delegate callback fn-ptr | `tools/re xref-offset` on `newDataIn` |
| AMD `+0xa8` | `MT2_OFF_AMD_HANDLER_OBJ` | getReport handler object `{report fn@+0x0, refcon+0x8, reportinfo fn@+0x20, refcon+0x28}` | disasm `_deviceGetReportWithLookUp` + `_getFeatureReportInfo` |
| AMD `this+0xb0` byte `+9` | (no macro — set via property) | device-button "S+9" gate | disasm `AppleMultitouchDevice::start` |

Verified empirically 2026-06-22: a live dump showed AMD `+0xa8` → handler obj whose `+0x20` =
`AppleBluetoothMultitouch staticReportInfoHandler @0x1356` and `+0x28` = our transport.

## Report formats (live in code, not restated here)

| Report | id | Source of truth |
|--------|----|-----------------|
| MT2 multitouch | `0x31` (`MT2_REPORT_ID_MT2`) | `src/mt2_decode.c` (4-byte header + 9-byte finger records; 13-bit signed coords; button = `report[1]&0x01`) |
| MT1 multitouch | `0x28` (`MT2_REPORT_ID_MT1`) | `src/mt1_encode.c` (button at `buf[1]&0x01`; CompactV4 timestamp packing) |
| geometry D-reports | `0xd1 0xd3 0xd0 0xa1 0xd9 0x7f` | `src/mt2_geometry.c` |

### CompactV4 PATH frame + 9-byte contact (genuine-USB reframe target)

The genuine-USB path feeds Apple's USB driver a CompactV4 PATH frame (`handleReport` → `enqueueData` →
`AppleUSBMultitouchUserClient` → MultitouchSupport). Frame = `[0x28][3 more header bytes][N×9 contacts]
[2-byte checksum]`; **contact count = `(frameLen-4)/9`** (`_MTParse_CompactV4BinaryPath` @0x5f69) and the
length is the kext's enqueue length = `handleReport`'s descriptor `getLength`. The 9-byte contact byte
layout, RE'd from `_MTCompactV4BinaryContactUnpack` (@0x5cd3):

| field | bits | notes |
|-------|------|-------|
| X | `(b1&0x1F)<<8 \| b0` | 13-bit; **same bytes as MT2** (`src/mt2_decode.c`) — bit-identical |
| Y | `b1>>5 \| b2<<3 \| (b3&0x3)<<11` | 13-bit; same bytes as MT2 |
| **touch state** | **`(b6>>6) \| ((b7&0x3)<<2)`** | 4-bit `MTTouchState`; Touching=4 ⇒ `b7\|=1`. **NOT MT2's byte** |
| size | `b6 & 0x3F` | low 6 bits of b6 (state is the top 2) |
| angle/axis | from `b7` (`<<7 & 0x7E00`) | b7's low 2 bits are state |

**Critical mismatch (the genuine-USB cursor blocker, 2026-06-24):** MT2 encodes finger-down in **`b3 & 0xC0`
(`0x80`=down)** (`src/mt2_decode.c`), but CompactV4 reads state from **b6/b7**. X/Y are bit-identical so a
pass-through reframe tracks position, but **state reads ≈0 (not touching)** → the recognizer ignores every
contact → no cursor. The reframe (`src/mt2_usb_to_compactv4`) must **translate** MT2 `b3` state into the
CompactV4 `b6[7:6]`/`b7[1:0]` bits, not pass bytes through. See `explanation.md` → genuine-USB cursor path.

Geometry id → property (from `AppleMultitouchDevice::decodeDeviceProperty`):

| id | Publishes |
|----|-----------|
| `0xd1` | Family ID |
| `0xd3` | Endianness, Sensor Rows, Sensor Columns, bcdVersion |
| `0xd9` | Sensor Surface Width/Height, Surface Descriptor |
| `0xd0` | Sensor Region Descriptor |
| `0xa1` | Sensor Region Param |
| `0x7f` | rCRITICAL_ERRORS (must read 0) |
| `0xdb` | Multitouch ID — **skipped** (driver must not answer) |

**GENUINE MT2 geometry — ground truth (captured 2026-06-25 from a real Magic Trackpad 2, ProductID 613,
on macOS 26.5.1 / M1, via `tools/genuine_mt2_geometry.sh` → `ioreg`).** These are the values to seed; our
earlier seeds were a half-resolution grid + zeroed region (the edge-dead-zone root cause — see
`open-questions.md` / [[mt2-cursor-edge-clamp]]):

| Property (`0xNN`) | Genuine MT2 | We had seeded |
|---|---|---|
| Family ID (`0xd1`) | **129** | 128 (`0x80`) |
| Sensor Rows (`0xd3`) | **22** | 13 |
| Sensor Columns (`0xd3`) | **30** | 16 |
| Surface Width/Height (`0xd9`) | **15600 × 11040** (156.0 × 110.4 mm) | 13000×11300 |
| Surface Descriptor (`0xd9` tail) | `f0 3c 00 00  20 2b 00 00  44 e3 52 ff bd 1e e4 26` (16B; first 8 = W/H LE) | not sent (len 8) |
| Sensor Region Descriptor (`0xd0`) | `02 01 00 14 01 00 1e 00 02 14 02 01 0e 02 00` (15B; `0x1e`=30=Cols) | 16 zero bytes |
| Sensor Region Param (`0xa1`) | `00 00 05 00 fe 01` (6B) | 16 zero bytes |

(Built-in MacBookAir trackpad, for contrast: Family 106, Cols 24, Rows 18, Surface 11897×8044.) **How we
got it:** `ioreg -lrw0 -c AppleMultitouchDevice` on modern macOS with the genuine MT2 attached; the MT2 is
the node with `Product="Magic Trackpad"` / `ProductID 613` / `MT Built-In=No`. The recognizer builds its
position-normalization rectangle from this grid/region, so the half-resolution seed shrank the active area
to ~half the pad → perpendicular-axis edge dead zones. Seed these genuine values in `src/mt2_geometry.c`
(BT) and `usb_build_init_props` (USB).

**Coordinate-range caveat (edge-clamp root):** MT1 native X `-2909..3167`, Y `-2456..2565`; MT2 X
`-3678..3934`, Y `-2478..2587` — an ~18–20% X-scale variance. `mt1_encode`'s X range can clamp near
the L/R pad edges (the frozen-X band). Tracked as the edge-clamp bug ([[mt2-cursor-edge-clamp]]).
Physical MT2 surface ≈ **160.0 × 114.9 mm**, res ≈ 47.6 / 44.1 units/mm (Linux+Windows drivers agree).

**Richer per-finger fields we decode coarsely:** the 9-byte record carries the device's own
classification — a 3-bit **Finger type** (6 = palm) and a 3-bit **State** (0x4 valid / 0x2 floating /
0x1 transition) — in the same bytes. `mt2_decode` reads only the coarse `t[3] & 0xC0 == 0x80`
down-bit (inherited from Linux `hid-magicmouse`, which delegates palm rejection to userspace). Carrying
the full Finger/State is a lead for future palm/tap work (the device provides it; Windows driver uses it).

## Properties

| Key | `mt2_stack.h` | Effect |
|-----|---------------|--------|
| `ExtractAndPostDeviceButtonState` | `MT2_PROP_EXTRACT_BUTTON` | in `DefaultMultitouchProperties` → copied to the AMD by `createMultitouchHandler` → `AMD::start` sets the S+9 device-button gate → physical + two-finger-right click dispatch (the **BT/AMD** button path) |
| `DefaultMultitouchProperties` | — | dict on the transport (we pass it via `bnb->init`); `createMultitouchHandler` copies its keys (`parser-type`=1000, `parser-options`=47, `MTHIDDevice`, `IOCFPlugInTypes`→MultitouchHID, …) onto the spawned AMD |
| **`parser-options` bit `0x2`** | — | **The recognizer's "clicky hardware" capability.** `MultitouchSupport` caches `parser-options` (`_mt_CachePropertiesForDevice`→device+0x1623c, read only via `_MTDeviceGetParserOptions`) and hands it to `MTSimpleHIDManager::initialize`, which stores it at the manager's **`this+0xb0`**. Bit `0x2` is then read ONLY by `handleButtonState` / `hwSupportsSecondaryClickCorners` / `hwSupports3FDrag` / `resetGestureParser` — i.e. it gates the recognizer's **gesture-side** button awareness (2-finger secondary-click, 3-finger-drag). It does NOT affect contact parsing (parser selects by `parser-type`=1000), cursor, scroll, or taps. Values: BT uses `47=0x2F` (bit set), Apple's genuine-USB personality `39=0x27` (set); `37=0x25` clears it → physical/2-finger click dead while taps work. **Seedable** → manual-start can supply it (RE'd 2026-06-24, `tools/re`). |

## Retired synthetic-path constants (deleted 2026-06-25; see `explanation.md` → "Retired synthetic approach")

The fabricated-`AppleMultitouchDevice` path's property/identity values, kept for the RE record. All were
set on a self-allocated `AppleMultitouchDevice` (`allocClassWithName`) published under our nub.

| Constant / key | Value | Role |
|----------------|-------|------|
| `IsFake` | `false` | STRICT `AMD::start` path — requires the provider to cast to an event driver/service (satisfied by `MT2HIDShell`); enables in-kernel cursor actuation wiring |
| `parser-type` | `1000` | parser selector (same as genuine; both transports) |
| `parser-options` | BT `47` (`0x2F`), USB `39` (`0x27`) | bit `0x2` = clicky-hardware gate (see Properties table) |
| `MT Built-In` | `true` | `MTDeviceIsBuiltIn` → system auto-drives it regardless of the "default device" pref |
| `Driver is Ready` | `true` | cached by `_mt_CachePropertiesForDevice` |
| `ExtractAndPostDeviceButtonState` | `true` | S+9 device-button gate (set directly on the fabricated device) |
| `IOCFPlugInTypes` | UUID `0516B563-B15B-11DA-96EB-0014519758EF` → `AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin` | required or `hidd` never instantiates the plugin / opens the user client |
| seeded prefs key | `MultitouchPreferences` (NOT `TrackpadUserPreferences`) | `determineHIDManagerSettings` reads `TrackpadUserPreferences` first, falls back to `MultitouchPreferences`; seeding the wrong one permanently shadows the live prefs push |

Synthetic Info.plist personalities (also deleted): **`MT2Gesture`** (IOResources nub publisher) and
**`MT2HIDEventDriver`** (Apple's `AppleMultitouchHIDEventDriver` `IOClass`, keyed VID 1452 / PID 782 /
source 2 — bound the `MT2HIDShell`). The kept cursor-actuation personality for genuine BT is
**`MT2HIDEventDriverBNB`** (VID 76 / PID 613 / source 1) — see "Cursor actuation personalities" above.

## BT connect handshake — the genuine sequence (input to the flap fix)

RE'd from the genuine `IOBluetoothHIDDriver` (10.9). **PSM 19 is device-initiated** — the host opens
it for nothing; the device opens its interrupt channel as a consequence of the control channel being
correctly accepted (`listenAt`-bound) and reaching OPEN. The genuine order (`handleStart` @0x32f2):

1. Win/own the control channel (PSM 17). *(We already do.)*
2. `listenAt(control, cb)` then `waitForChannelState(OPEN)` — **before anything else**
   (`staticPrepControlChannelAction` @0x3ab2). Writes no HID bytes.
3. `addMatchingNotification(IOBluetoothL2CAPChannel, PSM=0x13, interruptChannelOpeningCallback)` and
   **wait** — do *not* open PSM 19 ourselves.
4. On the PSM-19 nub: `listenAt(interrupt, cb)` then `waitForChannelState(OPEN)`
   (`prepInterruptChannelWL` @0x6338).
5. `deviceReady`: SET_PROTOCOL (`0x70 | bit`, with the subclass bit-inversion for 05AC:0309) **and
   enable reports/0xF1 — only now**, after both channels are OPEN (`setProtocol` base @0x5cd2 /
   subclass `IOAppleBluetoothHIDDriver` @0x1bfa). Gated by the `"SuppressSetProtocol"` provider prop.
6. Two 5000ms (`0x1388`) `waitForChannelState` timeouts (control + interrupt) bound the waits; no
   explicit PSM-19 retry loop — robustness is the state-waits + the device re-driving the connection.

Constants in `src/mt2_stack.h` (`[REF]`): `MT2_PSM_INTERRUPT`, `MT2_L2CAP_VT_listenAt` (0xa50),
`MT2_L2CAP_VT_waitForChannelState` (0xa20), `MT2_L2CAP_STATE_OPEN` (4), `MT2_HIDP_SET_PROTOCOL` (0x70).

**Why we flap:** our `start` enables `0xF1` early and does **not** `waitForChannelState(OPEN)` in
this order, so the device sometimes never opens PSM 19. Defer-0xF1 fixed the warm case; the full fix
is steps 2/4 (`waitForChannelState(OPEN)`) + the PSM-19 accept ordering. See
`how-to.md` → "fix the connect flap". The one unproven point is in `open-questions.md`.

### Genuine ordered teardown (panic-safe discipline)

`closeDownServicesWL` @0x60e8 unwinds every owned object **unregister/close → release → null the
slot**, never dereferencing after release: PM assertion; timer (`cancel → disable → removeEventSource
→ release → null`); PSM-19 notifier (`remove → null`); each channel
(`listenAt(NULL) → closeChannel-if-open → release → null`). Genuine driver channel fields:
interrupt `+0xe0`, control `+0xe8`, notifier `+0x110`, timer `+0x118` (these are the *genuine
driver's* layout, for understanding the discipline — not offsets we use). Our `stop()` already
applies this shape to our own owned objects.

## Cursor actuation personalities (`kext-gesture/Info.plist`)

The cursor moves only if an `AppleMultitouchHIDEventDriver` binds the AMD's `IOHIDInterface` (→
`IOHIDPointing`). Two static personalities, both Apple's `IOClass`, differing only in match identity:

| Personality | Matches | For |
|-------------|---------|-----|
| `MT2HIDEventDriver` | VID 1452 / PID 782 / source 2 | fDevice path (our MT1 HID shell) |
| `MT2HIDEventDriverBNB` | VID 76 / PID 613 / source 1 | full-BNB (BNB's real BT identity) |

Both carry `IOProbeScore` 100000 to beat the generic driver; each is inert when its interface is
absent, so they coexist unconditionally (no `kFullBnb` gate). Oracle: `tools/re amd-actuation`. See
`explanation.md` → cursor actuation.

## BT device enable, modes & the 0xF1 re-enable

- **Enable (multitouch):** MT2 BT feature report `0xF1 0x02 0x01`; over raw L2CAP the HIDP
  SET_REPORT(feature) framing is `0x53 0xF1 0x02 0x01` (`(SET_REPORT<<4)|FEATURE = 0x53`).
- **Modes:** the device streams report id `0x31` in multitouch mode, `0x02` in mouse mode.
- **Re-enable, critical:** BNB's interrupt-channel bring-up knocks the device **back to mouse mode
  (0x02) AFTER our initial enable**, so we **re-send `0xF1` ~8× on a 250ms timer from the control
  channel's command gate** once both channels are up. Without the re-enable the shim only sees 9-byte
  `0x02` reports and `mt2_decode` rejects them. (See `MT2BTReader` `reEnableInGate`.)
- **Handler-create trigger:** injecting `0xA1 0x60 0x02` into BNB's data callback drives
  `processDesyncedMultitouchData → createMultitouchHandler`, so BNB spawns its own AMD.
- **5s restart watchdog:** `BNBDevice::handleStart` arms a 5000ms (`0x1388`) `IOTimerEventSource`
  (logs "Forcing MT restart", calls `resetMultitouchTransport`); cancelled once real MT data
  (0x31 / the trigger) reaches `processDesyncedMultitouchData`. Starving it destabilizes the link.

## MTTouchState lifecycle (what `mt1_encode` targets)

Apple's recognizer keys tap/click on an 8-state lifecycle; the state is the **high nibble of finger
record byte `t[8]`** in the MT1 `0x28` report (fingerID = `t[8] & 0x0f`, verbatim):

| State | value | high-nibble |
|-------|-------|-------------|
| NotTracking | 0 | `0x00` |
| MakeTouch | 3 | `0x30` |
| Touching | 4 | `0x40` |
| BreakTouch | 5 | `0x50` |
| OutOfRange | 7 | `0x70` |

Healthy contact arc = `MakeTouch(3) → Touching(4) → BreakTouch(5)`; our session conditioning
(`mt2_session.c`) is presence-based (born → TS_START first frame, TS_TOUCHING after, vanished →
TS_END) and emits a trailing **zero-contact liftoff frame** after BreakTouch so the recognizer
finalizes the tap. Timestamp = CompactV4 packing `ts = (b1>>2) | (b2<<6) | (b3<<14)` (22-bit, in
`mt1_encode.c`).

## Runtime diagnostics

`debug.mt2_log` sysctl (`kext-gesture/mt2_log.{h,cpp}`): `0` off (default), `1` milestones +
CONNTRACE, `2` verbose (per-report geometry, per-edge clicks). `dmesg | tools/re conn-trace` →
per-connection STEADY/FAIL verdict.
