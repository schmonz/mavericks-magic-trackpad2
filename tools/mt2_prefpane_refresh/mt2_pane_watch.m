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

// Send the MT2x/load Apple event to pid, retrying until the osax CONFIRMS it loaded.
//
// The load race: a plain noErr is NOT proof of load. When our OSAX handler isn't registered yet
// (right after a reinstall, before OpenScripting rescans /Library/ScriptingAdditions), the event
// falls to System Prefs' default handler, which also returns noErr -> false-positive "injected", and
// the osax never loads. So we require our handler's reply MARKER (MT2InjectHandler puts it), and keep
// sending — OpenScripting's first unhandled event kicks off the additions scan/load asynchronously,
// so a later retry hits our now-registered handler. 40 tries x 250ms ~= 10s covers the rescan.
#define MT2_INJECT_MARKER 0x4D543258  /* 'MT2X'; must match MT2InjectHandler */
static void inject(pid_t pid) {
    for (int i = 0; i < 40; i++) {
        OSStatus err = procNotFound;
        Boolean confirmed = false;
        AEAddressDesc target = { typeNull, NULL };
        if (AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &target) == noErr) {
            AppleEvent evt = { typeNull, NULL }, reply = { typeNull, NULL };
            if (AECreateAppleEvent('MT2x', 'load', &target,
                                   kAutoGenerateReturnID, kAnyTransactionID, &evt) == noErr) {
                err = AESendMessage(&evt, &reply, kAEWaitReply | kAENeverInteract, kAEDefaultTimeout);
                if (err == noErr) {
                    SInt32 marker = 0; DescType dt = 0; Size sz = 0;
                    if (AEGetParamPtr(&reply, keyDirectObject, typeSInt32, &dt,
                                      &marker, sizeof(marker), &sz) == noErr && marker == MT2_INJECT_MARKER)
                        confirmed = true;
                }
                AEDisposeDesc(&reply);
            }
            AEDisposeDesc(&evt);
        }
        AEDisposeDesc(&target);
        if (confirmed) { NSLog(@"[mt2panewatch] injected+confirmed pid %d (try %d)", pid, i + 1); return; }
        usleep(250000);  // 250ms
    }
    NSLog(@"[mt2panewatch] inject NOT CONFIRMED for pid %d after 40 tries", pid);
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
