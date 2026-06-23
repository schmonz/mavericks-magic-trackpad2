# Reference — facts for driving the 10.9 multitouch stack

Lookup tables. The **numbers live in `src/mt2_stack.h`** (build-consumed) and in the decode/encode
**code**; this file names each fact, says what it's for, and gives the `re/` command that re-derives
it from the live binaries. If a point release shifts something, run the command and fix the header —
nothing here restates a raw number that isn't also in the header or code.

See `explanation.md` for how these fit together, `how-to.md` for the workflows.

## Transport vtable slots — `BluetoothMultitouchTransport` / `BNBTrackpadDevice`

| Slot | `mt2_stack.h` | Purpose | Re-derive |
|------|---------------|---------|-----------|
| `0xcc8` | `MT2_VT_getMultitouchReport` | geometry **DATA** fetch | `re/vtable AppleBluetoothMultitouch BNBTrackpadDevice 0xcc8` |
| `0xcd8` | `MT2_VT_getMultitouchReportInfo` | geometry **LENGTH** probe — runs **first**; if it fails the data fetch is never reached | `re/vtable … 0xcd8` |
| `0xd08` | `MT2_VT_createMultitouchHandler` | spawns the AMD — **UNVERIFIED this session** | `re/vtable … 0xd08` |

Both `0xcc8` and `0xcd8` are stubs on the stock transport (return `kIOReturnUnsupported`); we override
**both** on our cloned transport instance (see `explanation.md` → geometry).

## AMD vtable slots — `AppleMultitouchDevice` (the geometry GET call chain)

| Slot | `mt2_stack.h` | Role | Re-derive |
|------|---------------|------|-----------|
| `0x858` | `MT2_AMD_setGetReportHandler` | installs the getReport handler | `re/vtable AppleMultitouchDriver AppleMultitouchDevice 0x858` |
| `0x868` | `MT2_AMD_setReportInfoHandler` | installs the reportInfo handler | `re/vtable … 0x868` |
| `0x880` | `MT2_AMD_getReport` | `_cacheDeviceProperties` calls this per D-report | `re/vtable … 0x880` |
| `0x8e8` | `MT2_AMD_deviceGetReportWithLookUp` | length-probe-then-data dispatcher | `re/vtable … 0x8e8` |
| `0x8f8` | `MT2_AMD_getFeatureReportInfo` | calls the handler obj → transport | `re/vtable … 0x8f8` |

Chain: `_cacheDeviceProperties → getReport(0x880) → _deviceGetReportWithLookUp(0x8e8) → _getFeatureReportInfo(0x8f8) → handler obj (AMD+0xa8) → transport 0xcd8 then 0xcc8`.

## Struct field offsets

| Offset | `mt2_stack.h` | Field | Re-derive |
|--------|---------------|-------|-----------|
| transport `+0x1b0` | `MT2_OFF_BNB_AMD` | the spawned `AppleMultitouchDevice*` | `re/xref-offset` / disasm `startMultitouchThreaded` |
| transport `+0xf0` | `MT2_OFF_BNB_INTERRUPT_CHANNEL` | `BNBDevice::_interruptChannel` | disasm BNBDevice |
| channel `+0x110` (`+0x118`=target) | `MT2_OFF_L2CAP_DELEGATE_CB` | L2CAP delegate callback fn-ptr | `re/xref-offset` on `newDataIn` |
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

Calibrated MT2 values (`src/mt2_geometry.c`): Family `0x80`(128), Rows 13, Cols 16, Surface 13000×11300.

## Properties

| Key | `mt2_stack.h` | Effect |
|-----|---------------|--------|
| `ExtractAndPostDeviceButtonState` | `MT2_PROP_EXTRACT_BUTTON` | in `DefaultMultitouchProperties` → copied to the AMD by `createMultitouchHandler` → `AMD::start` sets the S+9 device-button gate → physical + two-finger-right click dispatch |
| `DefaultMultitouchProperties` | — | dict on the transport (we pass it via `bnb->init`); `createMultitouchHandler` copies its keys (`parser-type`=1000, `parser-options`=47, `MTHIDDevice`, `IOCFPlugInTypes`→MultitouchHID, …) onto the spawned AMD |

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

## Runtime diagnostics

`debug.mt2_log` sysctl (`kext-gesture/mt2_log.{h,cpp}`): `0` off (default), `1` milestones +
CONNTRACE, `2` verbose (per-report geometry, per-edge clicks). `dmesg | ./re/conn-trace` →
per-connection STEADY/FAIL verdict.
