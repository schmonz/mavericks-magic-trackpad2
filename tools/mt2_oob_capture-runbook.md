# MT2 USB-OOB link-key capture — Path A runbook (dtrace on `bluetoothd`)

**Goal:** capture the exact HID `SET_REPORT` that a modern macOS sends to the Magic Trackpad 2
over USB to write the 16-byte Bluetooth link key during out-of-band ("MagicPairing") pairing.
That report is the port target for 10.9 (whose IOBluetooth OOB methods are stubbed). The capture
uses `dtrace` to hook `IOHIDDeviceSetReport` in `bluetoothd`.

**Run it on:** `taavibookair.local` (Sequoia 15.7.7, x86_64). The repo is on the same NFS mount,
so the capture scripts are already present at
`/Users/schmonz/Documents/code/trees/mavericks-magic-trackpad2/tools/`.

**Why a runbook:** the first attempt hit `failed to grab pid 181` — SIP's DTrace/Debugging
Restrictions block attaching to the hardened `bluetoothd`. These steps relax that just long enough
to capture, then restore it.

**You need:** the MT2 + a Lightning→USB cable. (While the MT2 is away, use the backup mouse on the
10.9 box.)

---

## Steps

### 0. Record the current SIP state (for OCLP restore)
This box runs an OCLP custom SIP config (`-lilubetaall`). Before changing anything:
```
csrutil status          # screenshot / copy the output
nvram boot-args         # screenshot / copy the output
```
Keep both so you can restore exactly, and so OCLP root patches can be reapplied if needed.

### 1. Disable SIP (Recovery)
Reboot into **Recovery** → **Utilities ▸ Terminal**:
```
csrutil disable
reboot
```

### 2. Run the capture
Back in the OS, in a normal Terminal:
```
sh /Users/schmonz/Documents/code/trees/mavericks-magic-trackpad2/tools/mt2_oob_capture.sh
```
It self-elevates (one sudo password) and logs to `/tmp/mt2_oob_capture-<timestamp>.txt`.
It should sit and wait (no error).

### 3. If it STILL says `failed to grab pid …`
AMFI is also guarding the daemon. Add the bypass boot-arg (keep the existing args from step 0,
append `amfi_get_out_of_my_way=1`), reboot, and re-run step 2:
```
sudo nvram boot-args="keepsyms=1 debug=0x100 -lilubetaall ipc_control_port_options=0 -nokcmismatchpanic amfi_get_out_of_my_way=1"
sudo reboot
```

### 4. Trigger the pairing
With the capture waiting:
1. **System Settings ▸ Bluetooth** — if the MT2 is listed, **Forget** it (so plugging in triggers a
   *fresh* OOB pairing = a new link-key write).
2. **Plug the MT2 into this Mac over USB.**
3. Watch for one or more blocks like:
   ```
   === IOHIDDeviceSetReport  reportType=2  reportID=…  length=… ===
            0  <hex bytes …>
   ```
4. Press **Ctrl-C** once you've got them.

### 5. Send me the log
Send `/tmp/mt2_oob_capture-<timestamp>.txt` (or paste it). I'll decode the report ID + payload
(link key 16B + host BD_ADDR + framing) → the exact 10.9 recipe, and it settles the
`MaxFeatureReportSize=1` puzzle (Feature vs Output report / pairing-mode descriptor).

### 6. Restore SIP
Recovery → `csrutil enable` (or restore via the OCLP app to the value from step 0). If you added the
AMFI boot-arg, remove it (restore the step-0 `boot-args`). Reapply OCLP root patches if OCLP asks.

---

## If pairing completes but the log is EMPTY
The write isn't going through `IOHIDDeviceSetReport` — tell me. I'll widen the hook in
`tools/mt2_oob_capture.d` (async `IOHIDDeviceSetReportWithCallback` is already covered; next is the
report-queue path or the lower `IOConnectCallMethod`). You pick up the change over NFS — nothing to
re-copy on your end.

## Zero-SIP alternative (Path B)
If you'd rather not touch SIP: Apple's **PacketLogger** (Additional Tools for Xcode) captures the
Bluetooth/HCI side (`VSC AddHIDDevice` + link key + connection) without dtrace. It sees the
device-side write only if that write goes over Bluetooth (not USB). Ask me for the PacketLogger steps.

_reportType legend: 0=Input, 1=Output, 2=Feature._
