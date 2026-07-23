// bt_powercycle.m — replay the BT controller bring-up off-boot, with clean timestamps.
//
// Why: on a real boot, syslogd starts mid-boot and stamps everything it buffered at the same
// second, so the ~8s gap between blued's "hostControllerOnline" and it servicing paired devices
// is unmeasurable from the boot log. Toggling the controller power OFF then ON replays blued's
// controller-online -> paired-device bring-up sequence NOW (post-syslogd => real per-line
// timestamps), which lets us (a) time that gap precisely and (b) discriminate:
//   - if this reproduces the ~8s blued gap  -> the cost is blued's paired-device bring-up
//     (read the clean log to see whether the absent Magic Mouse is implicated)
//   - if this is fast (no ~8s)              -> the 8s is the cold USB firmware/DFU load that
//     only a real boot pays — a hardware floor, not blued.
//
// Non-destructive + reversible: the MT2 drops during the toggle; the mt2_linkstated keeper
// reconnects it. Requires the user's backup pointer to be available while BT is off.
//
// Build:  clang -fobjc-arc -framework Foundation -framework IOBluetooth -o /tmp/bt_powercycle tools/spikes/bt_powercycle.m
// Run:    /tmp/bt_powercycle           (watch: tail -f /var/log/system.log)

#import <Foundation/Foundation.h>
#import <unistd.h>

// Exported from IOBluetooth.framework (verified via nm); no public header, declare here.
extern int IOBluetoothPreferenceSetControllerPowerState(int powerState);
extern int IOBluetoothPreferenceGetControllerPowerState(void);

static void wait_for_state(int want) {
    for (int i = 0; i < 100 && IOBluetoothPreferenceGetControllerPowerState() != want; i++)
        usleep(100 * 1000);  // up to 10s
}

int main(void) {
    @autoreleasepool {
        NSLog(@"bt_powercycle: MARK t0 — power=%d, toggling OFF", IOBluetoothPreferenceGetControllerPowerState());
        IOBluetoothPreferenceSetControllerPowerState(0);
        wait_for_state(0);
        NSLog(@"bt_powercycle: MARK off — power=%d, holding 2s", IOBluetoothPreferenceGetControllerPowerState());
        sleep(2);
        NSLog(@"bt_powercycle: MARK on-request — toggling ON");
        IOBluetoothPreferenceSetControllerPowerState(1);
        wait_for_state(1);
        NSLog(@"bt_powercycle: MARK on — power=%d (watch for blued hostControllerOnline -> link key -> keeper openConnection)",
              IOBluetoothPreferenceGetControllerPowerState());
    }
    return 0;
}
