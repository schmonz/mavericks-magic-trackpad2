// voodooinputmavericks_pane_watch — per-user LaunchAgent (Branch A of the standalone-osax delivery):
// inject VoodooInputMavericksPane into System Preferences when it launches, by sending the
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
#import <sys/sysctl.h>   // kinfo_proc — target-process credential probe (diagnostic)
#import <sys/proc.h>     // P_SUGID
#import <signal.h>       // kill(pid,0) liveness
#import <errno.h>
#import <string.h>       // strerror

// The MT2x/load handler's reply marker. A plain noErr is NOT proof of load: before OpenScripting
// rescans /Library/ScriptingAdditions, the event falls to System Prefs' DEFAULT handler, which ALSO
// returns noErr -> false-positive "injected". So we require OUR handler's reply marker (MT2InjectHandler
// puts it); "confirmed" therefore means "our osax is loaded + registered in that process".
#define MT2_INJECT_MARKER 0x4D543258  /* 'MT2X'; must match MT2InjectHandler */

// One MT2x/load round-trip. Reports the send status + whether OUR marker came back, so a caller can
// distinguish "event didn't reach the app" (send_err != noErr) from "reached the default handler, osax
// not loaded yet" (send_err == noErr, no marker) from "our osax handled it" (got_marker).
typedef struct { OSStatus send_err; Boolean got_marker; } inject_probe_t;

static inject_probe_t inject_once(pid_t pid) {
    inject_probe_t r = { procNotFound, false };
    AEAddressDesc target = { typeNull, NULL };
    if (AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &target) == noErr) {
        AppleEvent evt = { typeNull, NULL }, reply = { typeNull, NULL };
        if (AECreateAppleEvent('MT2x', 'load', &target,
                               kAutoGenerateReturnID, kAnyTransactionID, &evt) == noErr) {
            r.send_err = AESendMessage(&evt, &reply, kAEWaitReply | kAENeverInteract, kAEDefaultTimeout);
            if (r.send_err == noErr) {
                SInt32 marker = 0; DescType dt = 0; Size sz = 0;
                if (AEGetParamPtr(&reply, keyDirectObject, typeSInt32, &dt,
                                  &marker, sizeof(marker), &sz) == noErr && marker == MT2_INJECT_MARKER)
                    r.got_marker = true;
            }
            AEDisposeDesc(&reply);
        }
        AEDisposeDesc(&evt);
    }
    AEDisposeDesc(&target);
    return r;
}

// (B) Diagnostic: log the target System Preferences process's credential state. Tests the OSAX load
// gate we saw at boot ("scripting addition loading restricted to system domains because this process
// has mixed credentials"): P_SUGID==1 (or ruid!=euid) means mixed/tainted credentials. Our osax lives
// in a SYSTEM domain (/Library/ScriptingAdditions), so this should NOT block us — logging it either
// confirms that (clean creds on the failing instance -> gate is elsewhere) or surprises us (tainted).
static void log_target_creds(pid_t pid) {
    struct kinfo_proc kp; size_t len = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len > 0) {
        NSLog(@"[mt2panewatch] target pid %d creds: ruid=%d euid=%d rgid=%d P_SUGID=%d",
              pid, (int)kp.kp_eproc.e_pcred.p_ruid, (int)kp.kp_eproc.e_ucred.cr_uid,
              (int)kp.kp_eproc.e_pcred.p_rgid, (kp.kp_proc.p_flag & P_SUGID) ? 1 : 0);
    } else {
        NSLog(@"[mt2panewatch] target pid %d creds: sysctl failed (%s)", pid, strerror(errno));
    }
}

static Boolean proc_alive(pid_t pid) {
    return (kill(pid, 0) == 0) || (errno == EPERM);   // EPERM = alive but not signalable by us
}

// Inject with RE-ARM (not a fixed 10s timeout): keep sending MT2x/load until OUR marker confirms, the
// target exits, or a generous backstop. OpenScripting loads scripting additions lazily/asynchronously,
// and at boot the OSA path in System Prefs can take much longer than the old 40-try (~10s) window to
// engage — so timing out abandoned the tab until a manual reopen. The marker is a deterministic success
// signal, so we simply keep re-arming: cheap (one AppleEvent per tick), self-stopping on confirm or on
// the pane's process going away. Runs on a background queue so the run loop stays responsive to other
// launches. Backoff: 250ms for the first ~10s (fast path for the normal case), then 2s. Backstop:
// ~10min of a still-alive-but-never-loading process (pathological) so we never spin forever.
static void inject(pid_t pid) {
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_BACKGROUND, 0), ^{
        NSLog(@"[mt2panewatch] inject start pid=%d", pid);
        log_target_creds(pid);                    // (B) baseline + failure-case gate probe, once per launch
        const int kFastTries = 40;                // ~10s at 250ms
        const int kMaxTries  = kFastTries + 300;  // + ~10min at 2s -> backstop
        Boolean diag_logged = false;
        for (int i = 0; i < kMaxTries; i++) {
            if (!proc_alive(pid)) {
                NSLog(@"[mt2panewatch] pid %d exited before confirm (after %d tries) — done", pid, i);
                return;
            }
            inject_probe_t p = inject_once(pid);
            if (p.got_marker) {
                NSLog(@"[mt2panewatch] injected+confirmed pid %d (try %d)", pid, i + 1);
                return;
            }
            // (B) After a short sustained miss, log WHY once: send error vs default-handler (no marker).
            if (!diag_logged && i >= 3) {
                NSLog(@"[mt2panewatch] pid %d not confirmed yet: send_err=%d marker=%d (osax not loaded/registered)",
                      pid, (int)p.send_err, p.got_marker);
                diag_logged = true;
            }
            usleep(i < kFastTries ? 250000 : 2000000);
        }
        NSLog(@"[mt2panewatch] inject backstop reached for pid %d (~10min, still alive, never confirmed)", pid);
    });
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
