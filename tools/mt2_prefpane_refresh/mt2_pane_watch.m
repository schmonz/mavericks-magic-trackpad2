// mt2_pane_watch — per-user LaunchAgent (Branch A of the standalone-osax delivery):
// inject MT2PaneRefresh into System Preferences when it launches, by sending the
// MT2x/load Apple event to its pid (the osax in /Library/ScriptingAdditions then
// loads and its constructor runs). Own process (NOT injected) -> normal ObjC/ARC.
#import <Foundation/Foundation.h>

static NSString * const kSysPrefsBundleID = @"com.apple.systempreferences";

BOOL mt2_should_inject(NSString *bundleID) {
    return bundleID != nil && [bundleID isEqualToString:kSysPrefsBundleID];
}

#ifndef MT2_PANE_WATCH_TEST
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

// Send the MT2x/load Apple event to pid, retrying (the app's AppleEvent machinery may
// not be ready the instant it launches). Phase 0 proved this loads the osax (in
// /Library/ScriptingAdditions) and runs its constructor. kAENeverInteract: never block
// on a UI prompt. The retry loop is our "wait until the app is ready" — if it proves
// insufficient on-device, escalate to KVO on -[NSRunningApplication finishedLaunching].
static void inject(pid_t pid) {
    OSStatus err = procNotFound;
    for (int i = 0; i < 12; i++) {
        AEAddressDesc target = { typeNull, NULL };
        err = AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &target);
        if (err == noErr) {
            AppleEvent evt = { typeNull, NULL }, reply = { typeNull, NULL };
            err = AECreateAppleEvent('MT2x', 'load', &target,
                                     kAutoGenerateReturnID, kAnyTransactionID, &evt);
            if (err == noErr) {
                err = AESendMessage(&evt, &reply, kAEWaitReply | kAENeverInteract, kAEDefaultTimeout);
                AEDisposeDesc(&reply);
            }
            AEDisposeDesc(&evt);
        }
        AEDisposeDesc(&target);
        if (err == noErr) { NSLog(@"[mt2panewatch] injected pid %d (try %d)", pid, i + 1); return; }
        usleep(250000);  // 250ms; up to 12 tries ~= 3s of settling
    }
    NSLog(@"[mt2panewatch] inject FAILED for pid %d (last err %d)", pid, (int)err);
}

int main(void) {
    @autoreleasepool {
        NSWorkspace *ws = [NSWorkspace sharedWorkspace];
        [ws.notificationCenter addObserverForName:NSWorkspaceDidLaunchApplicationNotification
                                           object:nil queue:nil usingBlock:^(NSNotification *note) {
            NSRunningApplication *app = note.userInfo[NSWorkspaceApplicationKey];
            if (mt2_should_inject(app.bundleIdentifier)) inject(app.processIdentifier);
        }];
        // Cover a System Preferences that's already running when the agent starts.
        for (NSRunningApplication *app in ws.runningApplications)
            if (mt2_should_inject(app.bundleIdentifier)) inject(app.processIdentifier);
        NSLog(@"[mt2panewatch] watching for System Preferences launches");
        [[NSRunLoop currentRunLoop] run];
    }
    return 0;
}
#endif /* MT2_PANE_WATCH_TEST */
