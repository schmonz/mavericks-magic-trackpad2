// MavericksTrackpad2Updater — a tiny, Dock-less helper that hosts Sparkle 1.27 and runs an on-demand
// update check. Standalone app (NOT the GC-neutral osax), normal ObjC/ARC. Launched by the Trackpad
// pane's "Check for Updates" (Phase 4); for now runnable directly.
//
// We DON'T #import <Sparkle/Sparkle.h> — Sparkle 1.27's headers use nullability/generics that the
// 10.9-era clang 6.0 can't parse. ObjC dispatch is runtime, so we declare the SUUpdater slice we use
// ourselves (below) and link the real framework binary. Same source builds on clang 6.0 (dev) and
// modern clang (CI). Delegate = informal protocol (respondsToSelector:).
#import <Cocoa/Cocoa.h>

@interface SUUpdater : NSObject
+ (SUUpdater *)sharedUpdater;
- (void)checkForUpdates:(id)sender;
- (void)setDelegate:(id)delegate;
@end

// Sparkle 1.x relaunches the host after a .pkg install and there is no config to suppress it. We drop
// a marker just before relaunch so the relaunched instance exits quietly instead of re-checking.
static NSString *const kRelaunchMarker = @"/tmp/.mt2updater-relaunched";

@interface MT2UpdaterDelegate : NSObject   // informal SUUpdaterDelegate (invoked via respondsToSelector:)
@end
@implementation MT2UpdaterDelegate
- (void)updaterWillRelaunchApplication:(SUUpdater *)updater {
    (void)updater;
    [[NSData data] writeToFile:kRelaunchMarker atomically:YES];
}
- (void)updaterDidNotFindUpdate:(SUUpdater *)updater { (void)updater; [NSApp terminate:nil]; }
- (void)userDidCancelDownload:(SUUpdater *)updater   { (void)updater; [NSApp terminate:nil]; }
@end

static MT2UpdaterDelegate *gDelegate;  // strong; SUUpdater.delegate is unretained in 1.x

int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:kRelaunchMarker]) {         // Sparkle just relaunched us post-update
            [fm removeItemAtPath:kRelaunchMarker error:NULL];
            return 0;                                         // nothing to do; exit quietly
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp activateIgnoringOtherApps:YES];                // LSUIElement: bring Sparkle's dialog forward
        gDelegate = [[MT2UpdaterDelegate alloc] init];
        SUUpdater *updater = [SUUpdater sharedUpdater];        // host bundle = this app (SUFeedURL/SUPublicEDKey)
        [updater setDelegate:gDelegate];
        [updater checkForUpdates:nil];                        // user-initiated: always shows UI
        [NSApp run];
    }
    return 0;
}
