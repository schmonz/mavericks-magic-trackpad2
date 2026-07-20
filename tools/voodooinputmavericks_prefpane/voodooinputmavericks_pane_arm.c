/*
 * voodooinputmavericks_pane_arm — send the 'MT2x'/'load' Apple event that triggers our .osax
 * handler (MT2InjectHandler) inside System Preferences, arming the live USB
 * observer. Run by a per-user LaunchAgent when System Preferences launches.
 *
 * Targets System Preferences by bundle id by default, or a specific pid if given
 * (the LaunchAgent knows the launched pid). Retries a few times because the
 * OpenScripting additions scan may not have registered our handler the instant
 * the app appears.
 *
 * Build: clang -mmacosx-version-min=10.9 -framework ApplicationServices -framework CoreFoundation
 * Usage: voodooinputmavericks_pane_arm [pid]
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ApplicationServices/ApplicationServices.h>

#define EVT_CLASS 'MT2x'
#define EVT_ID    'load'
#define BUNDLE_ID "com.apple.systempreferences"
#define RETRIES   8
#define RETRY_US  250000   /* 0.25s */

static OSStatus make_target(AEDesc *target, pid_t pid) {
    if (pid > 0)
        return AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), target);
    return AECreateDesc(typeApplicationBundleID, BUNDLE_ID, sizeof(BUNDLE_ID) - 1, target);
}

int main(int argc, char **argv) {
    pid_t pid = (argc > 1) ? (pid_t)atoi(argv[1]) : 0;

    AEDesc target = { typeNull, NULL };
    OSStatus err = make_target(&target, pid);
    if (err != noErr) { fprintf(stderr, "voodooinputmavericks_pane_arm: AECreateDesc target err %d\n", (int)err); return 1; }

    int i;
    for (i = 0; i < RETRIES; i++) {
        AppleEvent evt = { typeNull, NULL }, reply = { typeNull, NULL };
        err = AECreateAppleEvent(EVT_CLASS, EVT_ID, &target,
                                 kAutoGenerateReturnID, kAnyTransactionID, &evt);
        if (err == noErr) {
            err = AESendMessage(&evt, &reply, kAEWaitReply | kAENeverInteract, kAEDefaultTimeout);
            AEDisposeDesc(&reply);
        }
        AEDisposeDesc(&evt);
        if (err == noErr) { fprintf(stderr, "voodooinputmavericks_pane_arm: armed (try %d)\n", i + 1); break; }
        usleep(RETRY_US);
    }
    AEDisposeDesc(&target);

    if (err != noErr) { fprintf(stderr, "voodooinputmavericks_pane_arm: failed after %d tries, last err %d\n", RETRIES, (int)err); return 1; }
    return 0;
}
