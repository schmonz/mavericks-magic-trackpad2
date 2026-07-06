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
- (void)checkForUpdates:(id)sender;                 // user-initiated: always shows UI
- (void)checkForUpdatesInBackground;                // scheduled/silent: shows UI only if an update exists
- (void)setDelegate:(id)delegate;
@end

// Silent scheduled mode, selected by a `--background` argv flag (the LaunchAgent passes it). In this
// mode Sparkle shows NO "You're up-to-date!" alert, so the "let the modal show first" dance below is
// both unnecessary and undesirable -- we terminate immediately on a no-update result.
static BOOL gBackground = NO;

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
- (void)terminateNow { [NSApp terminate:nil]; }
- (void)updaterDidNotFindUpdate:(SUUpdater *)updater {
    (void)updater;
    // Background (scheduled) mode shows no alert -> nothing to wait for; exit right away so the
    // LSUIElement process never lingers.
    if (gBackground) { [NSApp terminate:nil]; return; }
    // Sparkle 1.27 calls this delegate BEFORE it shows its modal "You're up-to-date!" alert
    // (SUUIBasedUpdateDriver -didNotFindUpdate: delegate first, then a blocking -runModal). Calling
    // [NSApp terminate:] here killed the app before that alert could appear -- so a manual check
    // silently did nothing visible.
    //
    // We must let the alert show and still exit cleanly (LSUIElement, no window -> can't linger).
    // The alert runs a nested modal run loop in NSModalPanelRunLoopMode; schedule the terminate as a
    // zero-delay timer registered ONLY in NSDefaultRunLoopMode. It cannot fire during the modal loop
    // (wrong mode), and this call stack never returns to the default loop until after the alert is
    // dismissed -- so termination happens exactly once the user closes the alert. No window/notification
    // guessing, deterministic ordering.
    [self performSelector:@selector(terminateNow)
               withObject:nil
               afterDelay:0.0
                  inModes:@[NSDefaultRunLoopMode]];
}
- (void)userDidCancelDownload:(SUUpdater *)updater   { (void)updater; [NSApp terminate:nil]; }
@end

static MT2UpdaterDelegate *gDelegate;  // strong; SUUpdater.delegate is unretained in 1.x

int main(int argc, const char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--background") == 0) { gBackground = YES; break; }
    }
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
        if (gBackground) {
            [updater checkForUpdatesInBackground];            // silent: UI only if an update exists
        } else {
            [updater checkForUpdates:nil];                    // user-initiated: always shows UI
        }
        [NSApp run];
    }
    return 0;
}
