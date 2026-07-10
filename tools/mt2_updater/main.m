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

// Background-check watchdog. Sparkle's silent background check can hang before calling ANY delegate
// (a network/appcast stall) -> no dialog, no exit, the helper lingers in the Dock forever (observed
// 2026-07-07, "no dialog, wouldn't quit"). In --background mode we arm this timer at check-start and
// CANCEL it the instant either delegate fires (found/not-found); if neither does, Sparkle hung, so we
// force-exit. Cancel-on-resolution means it can NEVER kill a legit update dialog or an in-progress
// install (those already called a delegate). Foreground (manual) checks are untouched.
static const NSTimeInterval kBackgroundWatchdogSeconds = 120.0;
static NSTimer *gWatchdog = nil;

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
- (void)terminateNow { NSLog(@"[MT2Updater] terminating"); [NSApp terminate:nil]; }
- (void)cancelWatchdog { [gWatchdog invalidate]; gWatchdog = nil; }
- (void)watchdogFired:(NSTimer *)t {
    (void)t; gWatchdog = nil;
    NSLog(@"[MT2Updater] watchdog: background check didn't resolve in %.0fs, forcing exit", kBackgroundWatchdogSeconds);
    [NSApp terminate:nil];
}
- (void)updater:(SUUpdater *)updater didFindValidUpdate:(SUAppcastItem *)item {
    (void)updater;
    [self cancelWatchdog];   // check resolved (found) -> a real dialog/install follows; don't force-exit
    // Record the available version so the prefpane's About tab can show an "Update available" hint even
    // if the user dismisses Sparkle's dialog without installing. Fires on both user + background checks.
    NSString *v = [item respondsToSelector:@selector(displayVersionString)] ? [item displayVersionString] : nil;
    if (v.length) {
        [[NSUserDefaults standardUserDefaults] setObject:v forKey:kAvailableUpdateKey];
        [[NSUserDefaults standardUserDefaults] synchronize];
    }
    // DIAGNOSTIC: in background mode Sparkle now PRESENTS its update dialog and waits for the user — an
    // unattended daily run (pane closed) therefore lingers in the Dock until someone dismisses it. This
    // line marks that state so a "stuck updater" can be told apart from a genuine no-terminate hang.
    NSLog(@"[MT2Updater] update found: %@%@", (v.length ? v : @"(unknown)"),
          gBackground ? @" — background: Sparkle presents its dialog and WAITS for the user (unattended -> lingers)" : @"");
}
- (void)updaterDidNotFindUpdate:(SUUpdater *)updater {
    (void)updater;
    [self cancelWatchdog];   // check resolved (no update) -> we exit on our own below
    // Up to date -> clear any stale "update available" hint the pane might be showing.
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kAvailableUpdateKey];
    [[NSUserDefaults standardUserDefaults] synchronize];
    // Background (scheduled) mode shows no alert -> nothing to wait for; exit right away so the
    // LSUIElement process never lingers.
    if (gBackground) { NSLog(@"[MT2Updater] no update; background -> terminating now"); [NSApp terminate:nil]; return; }
    NSLog(@"[MT2Updater] no update; foreground -> terminate after the up-to-date alert is dismissed");
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
- (void)userDidCancelDownload:(SUUpdater *)updater   { (void)updater; NSLog(@"[MT2Updater] download cancelled -> terminating"); [NSApp terminate:nil]; }
@end

static MT2UpdaterDelegate *gDelegate;  // strong; SUUpdater.delegate is unretained in 1.x

int main(int argc, const char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--background") == 0) { gBackground = YES; break; }
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];  /* normal foreground app (see Info.plist) */

        NSString *selfVersion = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        NSLog(@"[MT2Updater] start: mode=%@ version=%@", gBackground ? @"background" : @"foreground",
              selfVersion.length ? selfVersion : @"(unknown)");

        // Sparkle relaunched us after a successful install (it dropped a marker in
        // updaterWillRelaunchApplication:). Show one "you're updated" confirmation, since the update
        // changed the kext/daemon/pane, not this helper, so there's otherwise NO visible signal.
        // ROBUSTNESS: ALWAYS remove the marker, and only honor it if it's FRESH — a stale marker left by an
        // interrupted/killed flow otherwise made EVERY later launch pop a bogus "updated" dialog and skip
        // the check (it once showed "updated to 0.4.0" when nothing had installed). Read the ACTUAL
        // installed version: the pkg replaced this app too, so our own bundle version IS the new version.
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:kRelaunchMarker]) {
            NSDate *stamp = [[fm attributesOfItemAtPath:kRelaunchMarker error:NULL] fileModificationDate];
            [fm removeItemAtPath:kRelaunchMarker error:NULL];               // always clear — never let it persist
            BOOL fresh = stamp && [[NSDate date] timeIntervalSinceDate:stamp] < 300.0;
            if (fresh && !gBackground) {
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
            // stale marker (or background mode) -> fall through to a normal check
        }

        // The About-tab "Check automatically" checkbox is the SINGLE control for auto-updates (default OFF).
        // Pre-seed SUEnableAutomaticChecks=NO the first time (if never set) so Sparkle 1.x doesn't pop its
        // own "check automatically?" prompt at an odd moment — the checkbox owns this decision.
        NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
        if ([ud objectForKey:@"SUEnableAutomaticChecks"] == nil) [ud setBool:NO forKey:@"SUEnableAutomaticChecks"];

        // Background (daily LaunchAgent) mode HONORS that checkbox: checkForUpdatesInBackground is an
        // explicit call Sparkle runs regardless of the setting, so gate it ourselves — auto-checks OFF
        // means the daily agent does nothing. (A user-initiated check is never gated.)
        if (gBackground && ![ud boolForKey:@"SUEnableAutomaticChecks"]) {
            NSLog(@"[MT2Updater] background: auto-check disabled -> exiting");
            return 0;
        }

        [NSApp activateIgnoringOtherApps:YES];                 // LSUIElement: bring Sparkle's dialog forward
        gDelegate = [[MT2UpdaterDelegate alloc] init];
        SUUpdater *updater = [SUUpdater sharedUpdater];         // host bundle = this app (SUFeedURL/SUPublicEDKey)
        [updater setDelegate:gDelegate];
        if (gBackground) {
            NSLog(@"[MT2Updater] background: starting silent check (watchdog %.0fs)", kBackgroundWatchdogSeconds);
            // Arm the watchdog in common modes so it still fires if Sparkle enters a nested runloop;
            // it's cancelled the moment a delegate resolves the check.
            gWatchdog = [NSTimer timerWithTimeInterval:kBackgroundWatchdogSeconds
                                                target:gDelegate selector:@selector(watchdogFired:)
                                              userInfo:nil repeats:NO];
            [[NSRunLoop mainRunLoop] addTimer:gWatchdog forMode:NSRunLoopCommonModes];
            [updater checkForUpdatesInBackground];             // silent: UI only if an update exists
        } else {
            NSLog(@"[MT2Updater] foreground: starting check");
            [updater checkForUpdates:nil];                     // user-initiated: always shows UI
        }
        [NSApp run];
    }
    return 0;
}
