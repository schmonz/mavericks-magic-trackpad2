# Runbook: write the persistent on-device name of a Magic Trackpad 2

> **✅ SOLVED + SHIPPED 2026-07-05 — this runbook is kept only as the RE trail.**
> The mechanism turned out to be simpler than the macOS-26 handoff below guessed: the MT2 declares
> **no** `DeviceName1..4`/`LongDeviceName` report. Its persistent name lives in its **one** declared
> HID Feature report — the 64-byte vendor report **`0x55`** — written verbatim via
> `SET_REPORT(Feature, 0x55, [id][name bytes])`. Proven on-device (read-back, survives power-cycle,
> shows on Tahoe + follows the device). The product writes it **only on an explicit user Rename in the
> Bluetooth pane**, mirrored onto the device by the injected osax (`mt2_prefpane_refresh.c` §7b). The
> read probe below (`tools/re mt2-name`) still stands; the standalone write tool was removed (the osax
> owns the only write path — we never write the name unrequested). Authoritative write-up:
> `docs/mt-stack/explanation.md` "✅ SOLVED …" + "Rename routing + the mirror".

**Goal (job to be done):** from the Mavericks target, write the name string that
the MT2 stores *in its own hardware* (the one that persists when the device is
paired to another computer) — not the host-side Bluetooth alias.

This runbook hands off an investigation done on a modern Mac (macOS 26) to an
agent working on the 10.9 target. The *what* and *where* are settled; the
remaining work is *observe the exact framing on real hardware, then write it*.

## What we know (RE'd, high confidence)

The name lives on the device as a set of **HID Feature reports** named
`DeviceName1`..`DeviceName4` (plus a `LongDeviceName` report). macOS reads them
in `-[AppleBluetoothHIDDevice deviceNameFromHardware]` (IOBluetooth) and writes
them from **bluetoothd** (`send setRemoteDeviceName` / `Writing device name as`;
LE devices instead get a GATT write to GAP Device-Name char `0x2A00`).

- **Transport:** HID GET_REPORT / SET_REPORT, report type = **Feature** (2).
- **Report IDs are per-device, not constants.** macOS builds a table
  `reportKey -> {reportID, size, min, max}` from the device's **HID report
  descriptor** (`reportIDForReportKey:` / `report:info:`). So you *discover* the
  IDs from the descriptor, keyed by the reports whose string names are
  `DeviceNameN`.
- **Read framing (from the disassembly):** per fragment,
  `getReport(Feature, id, buf[10], &len)`, then take `buf[1 .. len-2]`
  (skip a 1-byte header at `buf[0]`, drop the trailing byte) and concatenate.
  ~≤8 chars/report, ~32 total (`getMaxDeviceNameLength` default 32).
- **Write:** SET_REPORT Feature on the same IDs; the framework primitives are
  `-[AppleBluetoothHIDDevice setFeatureReport:value:]` /
  `setFeatureWithReportID:value:`.

**Still an inference — this is what to confirm on hardware:** the exact 1–2 byte
per-report header/terminator, and that BR/EDR write really is SET_REPORT on these
reports (bluetoothd's C++ is stripped, so we have its writer by log-string only).

## Step 1 — observe (READ-ONLY probe, already built)

Connect the MT2, then:

```
tools/re mt2-name              # auto-match Apple/trackpad HID devices
tools/re mt2-name 05ac 0265    # or match the MT2 by VID PID (hex)
tools/re mt2-name -a           # every HID device
# if reads come back empty/error unprivileged:
sudo tools/mt2_name_probe
```

`tools/re mt2-name` builds `tools/mt2_name_probe` (x86_64) from source on first
run. Source: `tools/mt2_name_probe.c` (IOHIDManager + IOHIDDeviceGetReport,
Feature reports only, no writes).

**Read from the output:**
- the Feature reports whose ASCII shows the name in fragments;
- their **report IDs and sizes** (the SET_REPORT targets + fragment caps);
- compare the raw `hex` line to `payload[1..len-2]`: if the reassembled
  candidate is the clean name, the skip-1/drop-1 framing is confirmed; if it's
  shifted/truncated, the raw bytes show the real header/terminator.

## Step 2 — write (to build)

Write `tools/mt2_name_write.c` as the SET_REPORT counterpart of the probe:
- reuse the probe's descriptor parse to find the `DeviceNameN` Feature report IDs;
- fragment the new name across them using the framing confirmed in Step 1
  (header byte + payload, one report per fragment, honoring each report's size);
- `IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, id, buf, len)` per fragment;
- keep it explicit and dry-run-able (print what it will write before writing).

Wire it as `tools/re mt2-name-write` mirroring `sub_mt2_name` in `tools/re`.

## Step 3 — validate

- Re-run `tools/re mt2-name` and confirm the on-device fragments now read back
  the new name.
- Verify persistence: the name should survive re-pair, and show up when the MT2
  is paired to a *different* host (that's the whole point — it's on the device,
  not a host cache).

## The RE toolkit (`tools/re`) you have here

One allowlisted entry (`Bash(tools/re:*)`), platform-aware — same subcommands
work on this 10.9/x86_64 target (on-disk Mach-O, otool/nm) and on a modern
Apple-Silicon Mac (dyld shared cache, `dyld_info`). Useful for this task:

- `tools/re objc-methods <bin> <filter>` — selectors -> IMP
- `tools/re disasm <bin> "<fn>" [filter]` — disassemble by name
- `tools/re calls <bin> <sym|selector>` — call sites (x86 callq / arm64 bl)
- `tools/re strings <bin> <regex>` — strings (VMADDRs on arm64)
- `tools/re xref <bin> <addr>` — who references an address (arm64 adrp+add/ldr)
- `tools/re syms <bin> <filter>` — symbols

Aliases include `IOBluetooth`, `IOBluetoothHIDDriver`, `bluetoothd`, `blued`.
Tests: `bats tests/test_re.bats` (green on both platforms; target-specific
tests skip by arch). If you extend `tools/re`, add a test.

Key binaries for this trail: `IOBluetooth` (AppleBluetoothHIDDevice —
`deviceNameFromHardware`, `reportIDForReportKey:`, `report:info:`,
`setFeatureReport:value:`) and `bluetoothd` (`setRemoteDeviceName`, CBNVRAM*,
CloudPairing — mostly host-side/cloud, C++ symbols stripped).
