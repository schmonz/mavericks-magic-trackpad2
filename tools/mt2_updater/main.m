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
- (void)checkForUpdateInformation;                  // scheduled: PROBING check -> calls delegates, NO UI ever
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
    // Record the available version so the prefpane's About tab can show an "Update available" hint.
    NSString *v = [item respondsToSelector:@selector(displayVersionString)] ? [item displayVersionString] : nil;
    if (v.length) {
        [[NSUserDefaults standardUserDefaults] setObject:v forKey:kAvailableUpdateKey];
        [[NSUserDefaults standardUserDefaults] synchronize];
    }
    if (gBackground) {
        // Background is a SILENT probe (checkForUpdateInformation) -> there is NO dialog and nothing to
        // wait for; we've recorded the pane hint, so exit immediately. This is the fix for the
        // unattended-linger family: a scheduled check that finds an update must never leave a blocking
        // dialog up. The user installs from the About tab (a foreground, UI check).
        [self cancelWatchdog];
        NSLog(@"[MT2Updater] update available: %@ — recorded pane hint; background probe -> exit", v.length ? v : @"(unknown)");
        [NSApp terminate:nil];
        return;
    }
    // Foreground (manual): let Sparkle present its normal update dialog. The watchdog is background-only,
    // so there's nothing to cancel here.
    NSLog(@"[MT2Updater] update available: %@ — foreground: presenting Sparkle's update dialog", v.length ? v : @"(unknown)");
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
            BOOL fresh = stamp && [[NSDate date] timeIntervalSinceDate:stamp] < 300.0;
            if (gBackground) {
                // A fresh marker means an update JUST installed. The postinstall reloads the
                // daily-check LaunchAgent, whose RunAtLoad fires a background instance mid-install --
                // don't run a redundant check right after an install, and DON'T consume the marker:
                // leave it so Sparkle's FOREGROUND relaunch shows the single "updated" confirmation
                // (whichever races first must not steal it). Only a STALE marker is cleared here.
                if (fresh) {
                    NSLog(@"[MT2Updater] fresh post-install marker; background -> exit (no check, marker kept)");
                    return 0;
                }
                [fm removeItemAtPath:kRelaunchMarker error:NULL];   // stale -> clear, fall through to a check
            } else {
                [fm removeItemAtPath:kRelaunchMarker error:NULL];   // foreground consumes the marker
                if (fresh) {
                    // The update changed the kext/daemon/pane, not this helper, so there's otherwise NO
                    // visible signal -- show one confirmation. Our own (just-replaced) bundle version IS
                    // the new version. Freshness gate stops a stale marker from popping a bogus dialog.
                    NSLog(@"[MT2Updater] post-install relaunch; showing 'updated to %@' confirmation",
                          selfVersion.length ? selfVersion : @"(unknown)");
                    [NSApp activateIgnoringOtherApps:YES];
                    NSAlert *done = [[NSAlert alloc] init];
                    [done setMessageText:@"Mavericks Trackpad 2 updated"];
                    [done setInformativeText:(selfVersion.length
                        ? [NSString stringWithFormat:@"The trackpad driver was updated to version %@ and is ready to use.", selfVersion]
                        : @"The trackpad driver was updated and is ready to use.")];
                    [done addButtonWithTitle:@"OK"];
                    [done runModal];
                    return 0;
                }
                // stale foreground marker -> fall through to a normal check
            }
        }

        // The About-tab "Check automatically" checkbox is the SINGLE control for auto-updates (default OFF).
        // Pre-seed SUEnableAutomaticChecks=NO the first time (if never set) so Sparkle 1.x doesn't pop its
        // own "check automatically?" prompt at an odd moment — the checkbox owns this decision.
        NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
        if ([ud objectForKey:@"SUEnableAutomaticChecks"] == nil) [ud setBool:NO forKey:@"SUEnableAutomaticChecks"];

        // Background (daily LaunchAgent) mode HONORS that checkbox: the probe runs regardless of the
        // setting, so gate it ourselves — auto-checks OFF means the daily agent does nothing.
        // (A user-initiated check is never gated.)
        if (gBackground && ![ud boolForKey:@"SUEnableAutomaticChecks"]) {
            NSLog(@"[MT2Updater] background: auto-check disabled -> exiting");
            return 0;
        }

        [NSApp activateIgnoringOtherApps:YES];                 // LSUIElement: bring Sparkle's dialog forward
        gDelegate = [[MT2UpdaterDelegate alloc] init];
        SUUpdater *updater = [SUUpdater sharedUpdater];         // host bundle = this app (SUFeedURL/SUPublicEDKey)
        [updater setDelegate:gDelegate];
        if (gBackground) {
            NSLog(@"[MT2Updater] background: starting silent probe (watchdog %.0fs)", kBackgroundWatchdogSeconds);
            // Watchdog backstop: force-exit if the probe hangs before any delegate fires. The probe
            // itself shows no UI, so the delegates (found/not-found) resolve quickly and we exit; the
            // watchdog only matters for a network/appcast stall.
            gWatchdog = [NSTimer timerWithTimeInterval:kBackgroundWatchdogSeconds
                                                target:gDelegate selector:@selector(watchdogFired:)
                                              userInfo:nil repeats:NO];
            [[NSRunLoop mainRunLoop] addTimer:gWatchdog forMode:NSRunLoopCommonModes];
            // PROBING check: calls didFindValidUpdate/updaterDidNotFindUpdate but shows NO UI, so a
            // scheduled/unattended run can never leave a blocking dialog up. The pane's About tab
            // surfaces any found update via the hint we record; the user installs from there.
            [updater checkForUpdateInformation];
        } else {
            NSLog(@"[MT2Updater] foreground: starting check");
            [updater checkForUpdates:nil];                     // user-initiated: always shows UI
        }
        [NSApp run];
    }
    return 0;
}
