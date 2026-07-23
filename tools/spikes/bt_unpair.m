// bt_unpair.m — unpair one BT device by address (private -[IOBluetoothDevice remove]).
// Used to test/fix whether a paired-but-absent device taxes controller bring-up.
// Build: clang -fobjc-arc -framework Foundation -framework IOBluetooth -o /tmp/bt_unpair tools/spikes/bt_unpair.m
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
@interface IOBluetoothDevice (Priv)
- (IOReturn)remove;
@end
int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *addr = (argc > 1) ? [NSString stringWithUTF8String:argv[1]] : @"34-15-9E-CD-0E-2C";
        IOBluetoothDevice *d = [IOBluetoothDevice deviceWithAddressString:addr];
        if (!d) { NSLog(@"bt_unpair: no device for %@", addr); return 1; }
        NSLog(@"bt_unpair: '%@' (%@) paired=%d -> remove", [d name], [d addressString], [d isPaired]);
        IOReturn r = [d remove];
        NSLog(@"bt_unpair: remove -> 0x%x", (unsigned)r);
    }
    return 0;
}
