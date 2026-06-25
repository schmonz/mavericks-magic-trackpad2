/* mt_svc_observe — mirror the Trackpad prefpane's live-update mechanism so we can see whether our
 * manually-started genuine driver fires the IOKit notifications the pane keys on.
 *
 * The pane uses -[IOServiceObserver initForService:target:selector:] (PreferencePanesSupport), which
 * registers kIOFirstMatchNotification + kIOTerminatedNotification on IOServiceMatching(class) and
 * reloads when either fires. This tool registers the SAME two notifications on both MT driver classes
 * and logs each callback. Run it, then power the MT2 off->on and hot-swap BT<->USB:
 *   - "init " lines at startup  = services already present when we armed (the relaunch-works path).
 *   - "LIVE " lines afterward   = the live notifications an ALREADY-OPEN pane would receive.
 * If our manual-start appear/terminate does NOT print LIVE lines, that's exactly why the open pane
 * doesn't update; the fix is to make our instance deliver those notifications.
 *
 * Build:  clang -Wall -O2 -std=c99 -o /tmp/mt_svc_observe tools/mt_svc_observe.c \
 *               -framework IOKit -framework CoreFoundation
 * Run:    /tmp/mt_svc_observe        (no root needed; Ctrl-C to stop)
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const char *CLASSES[] = { "AppleUSBMultitouchDriver", "BNBTrackpadDevice" };
#define NCLASS (sizeof(CLASSES)/sizeof(CLASSES[0]))

static char g_labels[NCLASS * 2][64];   /* refcons must outlive registration */

static void stamp(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    char b[16]; strftime(b, sizeof b, "%H:%M:%S", &tm);
    printf("[%s.%03d] ", b, (int)(tv.tv_usec / 1000));
}

/* Drain (which also ARMS the notification) and print each service in the iterator. */
static void drain(const char *phase, const char *label, io_iterator_t it) {
    io_service_t s;
    while ((s = IOIteratorNext(it))) {
        io_name_t name; name[0] = 0;
        IORegistryEntryGetName(s, name);
        uint64_t id = 0; IORegistryEntryGetRegistryEntryID(s, &id);
        stamp();
        printf("%s %-26s name=%-26s id=0x%llx\n", phase, label, name, id);
        IOObjectRelease(s);
    }
}

static void live_cb(void *refcon, io_iterator_t it) { drain("LIVE", (const char *)refcon, it); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: output reaches a redirected file live */
    IONotificationPortRef port = IONotificationPortCreate(kIOMasterPortDefault);
    if (!port) { fprintf(stderr, "IONotificationPortCreate failed\n"); return 1; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);

    for (unsigned i = 0; i < NCLASS; i++) {
        io_iterator_t itM = 0, itT = 0;
        char *lM = g_labels[i * 2], *lT = g_labels[i * 2 + 1];
        snprintf(lM, 64, "FIRST-MATCH %s", CLASSES[i]);
        snprintf(lT, 64, "TERMINATED  %s", CLASSES[i]);

        /* IOServiceMatching() and the dict are consumed per call -> create a fresh one each time. */
        kern_return_t kM = IOServiceAddMatchingNotification(
            port, kIOFirstMatchNotification, IOServiceMatching(CLASSES[i]), live_cb, lM, &itM);
        if (kM == KERN_SUCCESS) drain("init", lM, itM);
        else fprintf(stderr, "add kIOFirstMatchNotification(%s) failed: 0x%x\n", CLASSES[i], kM);

        kern_return_t kT = IOServiceAddMatchingNotification(
            port, kIOTerminatedNotification, IOServiceMatching(CLASSES[i]), live_cb, lT, &itT);
        if (kT == KERN_SUCCESS) drain("init", lT, itT);
        else fprintf(stderr, "add kIOTerminatedNotification(%s) failed: 0x%x\n", CLASSES[i], kT);
    }

    stamp();
    printf("armed: FirstMatch+Terminated for AppleUSBMultitouchDriver + BNBTrackpadDevice.\n");
    printf("       now power the MT2 off->on and hot-swap BT<->USB. 'LIVE' lines = what an open pane gets. Ctrl-C to stop.\n");
    CFRunLoopRun();
    return 0;
}
