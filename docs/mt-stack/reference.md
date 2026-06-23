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

## Runtime diagnostics

`debug.mt2_log` sysctl (`kext-gesture/mt2_log.{h,cpp}`): `0` off (default), `1` milestones +
CONNTRACE, `2` verbose (per-report geometry, per-edge clicks). `dmesg | ./re/conn-trace` →
per-connection STEADY/FAIL verdict.
