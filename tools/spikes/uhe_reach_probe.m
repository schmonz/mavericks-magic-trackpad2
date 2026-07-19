/* uhe_reach_probe — can OUR layer (a non-blued root process) reach the controller's
 * HID-Emulation (UHE) registration API that arms login/boot reconnect?
 *
 * Context (docs/mt-stack/open-questions.md, 2026-07-18): login reconnect = the Broadcom
 * controller autonomously re-pages devices whose link key was written into its HW slot via
 * blued's LinkKeyNotification -> addDeviceToHIDEmulationMode -> [controller
 * addHIDEmulationDevice:classOfDevice:linkKey:]. For the MT2 that path never runs
 * (isConfiguredHIDDevice == false, because our manual-start kext means no IOBluetoothHIDDriver
 * calls blued's configureHIDDevice: DO). The revised seam is to bypass blued and issue the
 * controller call ourselves. This probe answers: is that call reachable/functional from our
 * own process, and (with --write) does it take?
 *
 * addHIDEmulationDevice is a base-class STUB (returns 0xE00002C7) but a REAL impl on
 * BroadcomHostController; [IOBluetoothHostController defaultController] returns the concrete
 * subclass on Broadcom hardware. Signature (from IOBluetooth symbol table):
 *   -(unsigned short)addHIDEmulationDevice:(IOBluetoothDevice*)dev
 *                              classOfDevice:(unsigned int)cod
 *                                    linkKey:(const void* key; 16 bytes)
 *
 *   clang -fobjc-arc -mmacosx-version-min=10.9 -framework Foundation -framework IOBluetooth \
 *         -o uhe_reach_probe tools/spikes/uhe_reach_probe.m
 *   uhe_reach_probe                                  # READ-ONLY diagnostics (no mutation)
 *   uhe_reach_probe --write <cod-hex> <linkkey-32hex># write MT2 into a UHE slot (MUTATES controller)
 *   uhe_reach_probe --remove                         # removeHIDEmulationDevice: (undo)
 *
 * Default MT2 address 04-4b-ed-ec-02-07. Keyboard CoD = 0x2540 (empty slot); mouse = 0x2580.
 */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

/* All args BY VALUE (confirmed by disasm of the Broadcom IMP @0x10002 + blued's call site):
 *   addr = BluetoothDeviceAddress (6 bytes, rdx); cod = unsigned int (ecx, 3 bytes used);
 *   key  = BluetoothKey (16 bytes, r8:r9). NOT pointers. */
@interface IOBluetoothHostController (UHEProbe)
- (unsigned short)addHIDEmulationDevice:(BluetoothDeviceAddress)addr classOfDevice:(unsigned int)cod linkKey:(BluetoothKey)key;
- (unsigned short)removeHIDEmulationDevice:(BluetoothDeviceAddress)addr;
@end

static const char *MT2_ADDR = "04-4b-ed-ec-02-07";

static int hexbyte(const char *s) {
    int hi = (s[0] <= '9') ? s[0]-'0' : (s[0]|0x20)-'a'+10;
    int lo = (s[1] <= '9') ? s[1]-'0' : (s[1]|0x20)-'a'+10;
    return (hi << 4) | lo;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        BOOL doWrite  = (argc > 1 && strcmp(argv[1], "--write")  == 0);
        BOOL doRemove = (argc > 1 && strcmp(argv[1], "--remove") == 0);

        IOBluetoothHostController *hc = [IOBluetoothHostController defaultController];
        printf("defaultController = %p  class = %s\n", hc,
               hc ? [NSStringFromClass([hc class]) UTF8String] : "(nil)");
        printf("  responds addHIDEmulationDevice:classOfDevice:linkKey: = %d\n",
               [hc respondsToSelector:@selector(addHIDEmulationDevice:classOfDevice:linkKey:)]);
        printf("  responds removeHIDEmulationDevice: = %d\n",
               [hc respondsToSelector:@selector(removeHIDEmulationDevice:)]);

        IOBluetoothDevice *dev = [IOBluetoothDevice deviceWithAddressString:@(MT2_ADDR)];
        printf("MT2 %s: dev=%p paired=%d connected=%d cod=0x%x\n",
               MT2_ADDR, dev, [dev isPaired], [dev isConnected], (unsigned)[dev getClassOfDevice]);

        /* MT2 address "04-4b-ed-ec-02-07" -> 6 raw bytes */
        BluetoothDeviceAddress bda = {{0x04,0x4b,0xed,0xec,0x02,0x07}};

        if (!doWrite && !doRemove) {
            printf("\n[read-only] no mutation performed. Re-run with --write <cod> <linkkey> to arm.\n");
            return 0;
        }

        if (doRemove) {
            unsigned short st = [hc removeHIDEmulationDevice:bda];
            printf("removeHIDEmulationDevice -> 0x%04x\n", st);
            return 0;
        }

        /* --write <cod-hex> <linkkey-32hex> */
        if (argc < 4) { printf("usage: --write <cod-hex> <linkkey-32hex>\n"); return 2; }
        unsigned int cod = (unsigned int)strtoul(argv[2], NULL, 16);
        const char *kh = argv[3];
        if (strlen(kh) != 32) { printf("link key must be 32 hex chars (16 bytes)\n"); return 2; }
        BluetoothKey key;
        for (int i = 0; i < 16; i++) key.data[i] = (unsigned char)hexbyte(kh + i*2);

        printf("calling addHIDEmulationDevice: addr=%s cod=0x%x key=%s\n", MT2_ADDR, cod, kh);
        unsigned short st = [hc addHIDEmulationDevice:bda classOfDevice:cod linkKey:key];
        printf("addHIDEmulationDevice -> 0x%04x  (0x0000 = success)\n", st);
        return 0;
    }
}
