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

// The appcast item Sparkle hands to -updater:didFindValidUpdate:. We only read its display version.
@interface SUAppcastItem : NSObject
- (NSString *)displayVersionString;
@end

// The prefs key (in THIS app's own domain = com.schmonz.MavericksTrackpad2Updater) where we record the
// version of an available update. The prefpane osax reads it to show an "Update available" hint. Set when
// Sparkle finds an update (either check mode), cleared when we're up to date.
static NSString *const kAvailableUpdateKey = @"MT2AvailableUpdateVersion";

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
- (void)updater:(SUUpdater *)updater didFindValidUpdate:(SUAppcastItem *)item {
    (void)updater;
    // Record the available version so the prefpane's About tab can show an "Update available" hint even
    // if the user dismisses Sparkle's dialog without installing. Fires on both user + background checks.
    NSString *v = [item respondsToSelector:@selector(displayVersionString)] ? [item displayVersionString] : nil;
    if (v.length) {
        [[NSUserDefaults standardUserDefaults] setObject:v forKey:kAvailableUpdateKey];
        [[NSUserDefaults standardUserDefaults] synchronize];
    }
}
- (void)updaterDidNotFindUpdate:(SUUpdater *)updater {
    (void)updater;
    // Up to date -> clear any stale "update available" hint the pane might be showing.
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kAvailableUpdateKey];
    [[NSUserDefaults standardUserDefaults] synchronize];
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
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];  /* normal foreground app (see Info.plist) */

        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:kRelaunchMarker]) {          // Sparkle relaunched us after installing the pkg
            [fm removeItemAtPath:kRelaunchMarker error:NULL];
            // The update changed the kext/daemon/pane, not this helper, so there's nothing to "return to" —
            // and the user otherwise gets NO signal the driver updated. Show one clear confirmation, then
            // exit. The pkg replaced this app too, so our own bundle version IS the just-installed version.
            [NSApp activateIgnoringOtherApps:YES];
            NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            NSAlert *done = [[NSAlert alloc] init];
            [done setMessageText:@"Mavericks Trackpad 2 updated"];
            [done setInformativeText:(ver.length
                ? [NSString stringWithFormat:@"The trackpad driver was updated to version %@ and is ready to use.", ver]
                : @"The trackpad driver was updated and is ready to use.")];
            [done addButtonWithTitle:@"OK"];
            [done runModal];
            return 0;
        }

        // The About-tab "Check automatically" checkbox is the SINGLE control for auto-updates (default OFF).
        // Pre-seed SUEnableAutomaticChecks=NO the first time (if never set) so Sparkle 1.x doesn't pop its
        // own "check automatically?" prompt at an odd moment — the checkbox owns this decision.
        NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
        if ([ud objectForKey:@"SUEnableAutomaticChecks"] == nil) [ud setBool:NO forKey:@"SUEnableAutomaticChecks"];

        // Background (daily LaunchAgent) mode HONORS that checkbox: checkForUpdatesInBackground is an
        // explicit call Sparkle runs regardless of the setting, so gate it ourselves — auto-checks OFF
        // means the daily agent does nothing. (A user-initiated check is never gated.)
        if (gBackground && ![ud boolForKey:@"SUEnableAutomaticChecks"]) return 0;

        [NSApp activateIgnoringOtherApps:YES];                 // LSUIElement: bring Sparkle's dialog forward
        gDelegate = [[MT2UpdaterDelegate alloc] init];
        SUUpdater *updater = [SUUpdater sharedUpdater];         // host bundle = this app (SUFeedURL/SUPublicEDKey)
        [updater setDelegate:gDelegate];
        if (gBackground) {
            [updater checkForUpdatesInBackground];             // silent: UI only if an update exists
        } else {
            [updater checkForUpdates:nil];                     // user-initiated: always shows UI
        }
        [NSApp run];
    }
    return 0;
}
