# MT2 battery reporting — findings + solution

**Status (2026-07-01): ✅ COMPLETE — the BT prefpane shows a real battery % (verified 100% on-device).**
Full chain: kext polls `GET_REPORT(0x90)` on the BT **control** channel, publishes `BatteryPercent` (+ an
`ExtendedFeatures` gate-property) on the `BNBTrackpadDevice` node, and the pane reads it. This doc is the
durable record. Companion memory note: `mt2-battery-reporting-plan`. Live probes: `tools/mt2_battery_probe.c`
(raw device read), `tools/mt2_panebattery_probe.m` (replicates the pane's exact battery read = the oracle),
`tools/mt2_hidnode_probe.c` (the pane's `IOServiceMatching("IOBluetoothHIDDriver")` node resolution).

## ▶▶ THE SOLUTION (shipped)
1. **Poll** (kext, `MT2BTReader.cpp`): the device never streams `0x90`; it only answers a poll. So the control
   reader interposes the **control channel's** L2CAP delegate (`bt_control_shim`, separate globals from the
   interrupt interpose, peek-and-forward so BNB's control plane is untouched) and, after the re-enable burst,
   sends `GET_REPORT(Input,0x90)` = `{0x41,0x90}` on a 30 s timer. The device answers `A1 90 <flags> <cap>` on
   the control channel; the shim catches it and publishes.
2. **Publish** (kext): `fManualBnb->setProperty("BatteryPercent", OSNumber(cap,32))` on change. The shared pure
   decode is `mt2_parse_battery_report` (`src/mt2_battery.c`, host-tested `tests/test_battery.c`).
3. **The `ExtendedFeatures` gate** (the non-obvious part — RE'd via `mt2_panebattery_probe`): publishing
   `BatteryPercent` alone was NOT enough — the pane still showed 0%. The pane's
   `-[AppleBluetoothHIDDevice initWithHIDDevice:]` (IOBluetooth @0x114d8) does
   `if (IORegistryEntryCreateCFProperty(node,"ExtendedFeatures")==nil){[self dealloc]; return nil;}` — so with
   no `ExtendedFeatures` the wrapper is nil and `batteryPercent` returns 0 **regardless** of `BatteryPercent`.
   Genuine MT1 sets `ExtendedFeatures` from real extended feature reports; the MT2 has none. Fix = publish a
   present-but-empty `ExtendedFeatures` OSDictionary on the node (in `start()`, alongside the `Product` seed)
   purely to pass that presence gate. `batteryPercent` does NOT read the dict's contents — it reads
   `BatteryPercent` off the same node and returns it as a 0.0–1.0 fraction (100 → 1.0 = full).
   BT-only (the USB pane has no battery row); USB parity would be separate.

---

## (history) read-side findings + earlier resume plan

## Goal
Make the Trackpad prefpane's Bluetooth battery row show a real percentage (today it reads 0% — a placeholder).

## How the pane displays battery (RE'd, disasm-confirmed)
- `-[BaseTrackPadController _checkBatteryTimer:]` (@0x4988) does
  `[[BluetoothHIDDevice withBluetoothDevice:dev] batteryPercent]` → `setFloatValue:` on `mBTBatteryControl`
  + `_batteryLabelForPercent:` → `setStringValue:`.
- `-[AppleBluetoothHIDDevice batteryPercent]` (IOBluetooth @0x11b8c):
  `IORegistryEntrySetCFProperties(node, {"UpdateBatteryLevel"})` (poke the driver to refresh) then
  `IORegistryEntryCreateCFProperty(node, "BatteryPercent")` → `unsignedLongValue`.
- **So the pane just reads a plain `"BatteryPercent"` OSNumber (0–100) off the BT device's IOReg node.**
  Everything downstream of that integer already works.

## Why it's 0% today
The genuine BT battery is an MT1-shaped **voltage + per-product chemistry-calibration** model:
`BNBDevice::updateBatteryLevel` / `getLatchedBatteryVoltage` / `createBatteryChemistryDict` /
`CalibratedBatteryThresholds`, all fed from MT1-era **extended-feature reports** (`getExtendedReport(char
const*)`, `AppleBluetoothMultitouch.kext`). The MT2 doesn't provide those → kernel log
`[BNBTrackpadDevice] No extended features available` / `Couldn't get battery percentage from device` →
`"BatteryPercent"` is never set → pane shows 0%. **Do not chase this model — the MT2 isn't MT1-shaped.**

## The MT2's own battery report (READ — solved + verified live)
In multitouch mode (what our driver puts the device in), the MT2 exposes battery as the standard Apple
**Power-Device** input report on the vendor interface (usagePage `0xff00` / usage `0x0b`):
- Report ID **`0x90`**, Usage Page `0x84` (Power Device) + `0x85` (Battery System); capacity = Usage `0x65`.
- `GET_REPORT(Input, 0x90)` → `[0x90 id][status flags][capacity 0–100]`. **Battery % = byte[2].**
- Verified live via `tools/mt2_battery_probe.c`:
  - USB: `90 05 64` → **100%** (byte[1] `0x05` = charging).
  - BT:  `90 00 64` → **100%** (byte[1] `0x00` = on battery). Over BT the MT2 enumerates as vid **`0x004c`**
    (Apple BT company id, not USB's `0x05ac`), pid `0x0265`.
- Same report id + byte offset on both transports; macOS handles the BT HIDP framing. byte[1] gives a free
  charging indicator.
- Red herring: the *mouse-mode* descriptor also had a battery-strength report `0x47` (Usage `0x06/0x20`, 1
  byte), but that interface is NOT present in multitouch mode. The first `0x47` read we saw was a vendor
  report. Ignore `0x47`.

## The bridge (PUBLISH — node confirmed)
Publish the capacity byte as the `"BatteryPercent"` OSNumber on the genuine `BNBTrackpadDevice` node that the
BT reader manual-starts (`fManualBnb` / global `gGenuineBnb` in `kext-gesture/MT2BTReader.cpp`):
`fManualBnb->setProperty("BatteryPercent", OSNumber::withNumber(cap, 32))`. This bypasses BNB's broken
voltage/chemistry model entirely.

**Node confirmed + userspace ruled out (2026-07-01 node test, userspace `IORegistryEntrySetCFProperties`):**
writing `BatteryPercent` to `BNBTrackpadDevice` and `IOBluetoothHIDDriver` both returned **`0xe00002c7`
(kIOReturnUnsupported), readback ABSENT** — the driver's `setProperties` gate rejects userspace writes of
`BatteryPercent` (it accepts only its known keys, e.g. `UpdateBatteryLevel`). That both **confirms
`BNBTrackpadDevice` is the pane's battery node** (it owns the handler) and **kills any userspace publish
path** — only the kext's kernel `setProperty` (which bypasses the `setProperties` gate) can set it.

## What's built so far (scaffold, in the working tree)
`kext-gesture/MT2BTReader.cpp` — a **passive watch** added to `bt_interpose_shim`:
```c
if (rlen >= 3 && rep[0] == 0x90 && gGenuineBnb) mt2_publish_battery(gGenuineBnb, rep[2]);
```
plus helpers `mt2_publish_battery` (setProperty `BatteryPercent` on change) and `mt2_diag_report_id`.
Built, loaded, and confirmed running (`first shim hit` in the kernel log). **But it never fires:** after ~4
min of trackpad activity `BatteryPercent` was never published → **the MT2 does NOT send report `0x90`
spontaneously on the interrupt channel; it only answers a `GET_REPORT` poll.** The passive watch is correct
but insufficient. Code is harmless (watches a report id that doesn't arrive); left in place as the foundation.

## ▶▶ RESUME HERE (next agent) — the poll
The device won't stream `0x90`, and `GET_REPORT` responses land on the **control** channel (PSM 17, owned by
genuine BNB), NOT our interrupt-channel shim. So finish it in `kext-gesture/MT2BTReader.cpp`:
1. **Interpose the CONTROL channel's delegate** too — same save-and-swap trick as the interrupt channel
   (`interposeInGate` swaps `channel + L2CAP_DELEGATE_CB_OFF`); do the equivalent for the PSM-17 channel so
   we see its incoming data. (First check: does the device send `0x90` *spontaneously* on the control channel?
   If so, no poll needed — just watch.)
2. **Poll on the existing timer** (`interposeTimerFired` / a sibling of `reEnableInGate`): `fChannel->sendTo`
   a HIDP `GET_REPORT(Input, 0x90)` request. Header ≈ `(GET_REPORT<<4)|reportType` = `0x41` (input) or `0x43`
   (feature) + report id `0x90`. Mirror the SET_REPORT enable in `reEnableInGate`
   (`{ MT2_HIDP_SET_REPORT_FEATURE, MT2_ENABLE_REPORT_ID, 0x02, 0x01 }`).
3. The control-channel shim catches the `[0x90][flags][cap]` response → call `mt2_publish_battery(gGenuineBnb,
   cap)` (already written). Verify: `ioreg -r -c BNBTrackpadDevice | grep BatteryPercent`, then open the
   Trackpad pane on BT → the number appears.

Gotchas learned this session:
- A plain `kext-load` **no-ops if a stale `com.schmonz.MT2Gesture` is still resident** → do `kext-unload`
  then `kext-load`; confirm with `kextstat | grep schmonz` (index changes).
- `debug.mt2_log` **resets to 0 on every kext reload** — `sudo sysctl -w debug.mt2_log=1` after loading.
- `MT2_DLOG(1)` is gated on that sysctl; `mt2_diag_report_id`'s seen-bitmap can mark ids while logging is off,
  so they won't re-log — don't trust its absence.
- After a reload the MT2 reconnects before the new reader is ready → run `cmake-build/sbin/mt2_bt_bounce` to
  force a fresh L2CAP connection so the reader re-interposes.
- Both-transports note: battery is a BT-pane feature (USB pane has no battery row natively). USB parity would
  be separate.

Crash risk of the publish (already assessed + accepted): LOW — bounds-checked read + `setProperty` on the
valid `gGenuineBnb` (teardown restores the delegate before clearing it, so no UAF); distinct report id, no
touch-path/vtable/timing change.
