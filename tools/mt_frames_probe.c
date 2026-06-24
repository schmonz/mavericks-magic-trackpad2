/* mt_frames_probe - drive Apple's genuine MultitouchSupport consumer path and report whether
 * frames flow from our manual-started genuine AppleUSBMultitouchDriver.
 *
 * Why: genuine-USB test #2 proved our reframed 0x60 packets PASS Apple's validateChecksum, but
 * `handleReport` only enqueues to an open AppleUSBMultitouchUserClient frames client, and none was
 * open (0 instances) -> no cursor/pane. Off-device RE showed MultitouchSupport's MTDeviceCreateList
 * matches IOServiceMatching("AppleUSBMultitouchDriver") and _MTDeviceCreateFromService accepts our
 * class+USB transport. The remaining unknowns are LIVE-ONLY. This tool exercises them:
 *   1. MTDeviceCreateList() -> does our instance show up in the genuine device list?
 *   2. MTDeviceStart() -> opens the user client (kicks registerUserClient->addFramesClient->
 *      configureDataMode->streaming on the driver) and registers a frame callback.
 *   3. callback prints contact count/timestamp -> PROVES the end-to-end path
 *      device -> our reframe -> genuine handleReport -> enqueueData -> user client -> userspace.
 *
 * If contacts arrive here, the translate-and-feed pipeline is fully working and the only missing
 * piece for cursor/pane is auto-discovery by WindowServer (publish props / fire hotplug).
 *
 * Private MultitouchSupport API is resolved via dlopen/dlsym (no link-time framework dependency),
 * so this builds cleanly on 10.9.
 *
 * Build:  cc -O2 -o tools/mt_frames_probe tools/mt_frames_probe.c -framework CoreFoundation
 * Run:    ./tools/mt_frames_probe          (after manual-starting the genuine driver; touch the pad)
 *         Ctrl-C to stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <signal.h>
#include <CoreFoundation/CoreFoundation.h>

typedef void *MTDeviceRef;

/* MTTouch / contact record. We only read a few fields to prove frames flow; layout per the
 * long-documented MultitouchSupport struct (10.x). Sizes are stable enough for a count+sample probe. */
typedef struct { float x, y; } MTPoint;
typedef struct { MTPoint pos, vel; } MTVector;
typedef struct {
    int   frame;
    double timestamp;
    int   pathIndex;
    int   state;
    int   fingerID;
    int   handID;
    MTVector normalized;
    float size;
    int   pressure;
    float angle, majorAxis, minorAxis;
    MTVector absVec;
    int   z1, z2;
    float zDensity;
} MTTouch;

typedef CFArrayRef (*MTDeviceCreateList_f)(void);
typedef int  (*MTContactCallback_f)(int device, MTTouch *contacts, int n, double ts, int frame);
typedef void (*MTRegister_f)(MTDeviceRef, MTContactCallback_f);
typedef void (*MTDeviceStart_f)(MTDeviceRef, int);
typedef void (*MTDeviceStop_f)(MTDeviceRef);
typedef void (*MTDeviceGetID_f)(MTDeviceRef, uint64_t *);
typedef int  (*MTDeviceGetFamilyID_f)(MTDeviceRef, int *);
typedef int  (*MTDeviceIsRunning_f)(MTDeviceRef);

static MTDeviceStop_f gStop;
static CFArrayRef     gList;

static int frame_cb(int device, MTTouch *contacts, int n, double ts, int frame) {
    static unsigned long count = 0;
    if ((count++ % 30) == 0 || n > 0) {   /* throttle idle frames, always show touches */
        printf("FRAME dev=%d n=%d ts=%.3f frame=%d\n", device, n, ts, frame);
        for (int i = 0; i < n; i++)        /* print ALL slots + the density inputs (axes/pressure/zDensity) */
            printf("   c%d: state=%d id=%d path=%d norm=(%.3f,%.3f) size=%.2f maj=%.2f min=%.2f press=%d zd=%.2f\n",
                   i, contacts[i].state, contacts[i].fingerID, contacts[i].pathIndex,
                   contacts[i].normalized.pos.x, contacts[i].normalized.pos.y, contacts[i].size,
                   contacts[i].majorAxis, contacts[i].minorAxis, contacts[i].pressure, contacts[i].zDensity);
        fflush(stdout);
    }
    return 0;
}

static void on_sigint(int s) {
    (void)s;
    if (gStop && gList) {
        CFIndex n = CFArrayGetCount(gList);
        for (CFIndex i = 0; i < n; i++) gStop((MTDeviceRef)CFArrayGetValueAtIndex(gList, i));
    }
    printf("\nstopped.\n");
    exit(0);
}

int main(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen MultitouchSupport failed: %s\n", dlerror()); return 1; }

    MTDeviceCreateList_f  CreateList = (MTDeviceCreateList_f) dlsym(h, "MTDeviceCreateList");
    MTRegister_f          Register   = (MTRegister_f)         dlsym(h, "MTRegisterContactFrameCallback");
    MTDeviceStart_f       Start      = (MTDeviceStart_f)      dlsym(h, "MTDeviceStart");
    gStop                            = (MTDeviceStop_f)       dlsym(h, "MTDeviceStop");
    MTDeviceGetID_f       GetID      = (MTDeviceGetID_f)      dlsym(h, "MTDeviceGetDeviceID");
    MTDeviceGetFamilyID_f GetFam     = (MTDeviceGetFamilyID_f)dlsym(h, "MTDeviceGetFamilyID");
    MTDeviceIsRunning_f   IsRunning  = (MTDeviceIsRunning_f)  dlsym(h, "MTDeviceIsRunning");

    if (!CreateList || !Register || !Start || !gStop) {
        fprintf(stderr, "missing MT symbols: CreateList=%p Register=%p Start=%p Stop=%p\n",
                (void*)CreateList, (void*)Register, (void*)Start, (void*)gStop);
        return 1;
    }

    gList = CreateList();
    CFIndex n = gList ? CFArrayGetCount(gList) : 0;
    printf("MTDeviceCreateList: %ld device(s)\n", (long)n);
    if (n == 0) {
        printf("=> our genuine instance is NOT in MultitouchSupport's list (discoverability gap).\n");
        return 0;
    }
    for (CFIndex i = 0; i < n; i++) {
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(gList, i);
        uint64_t id = 0; int fam = -1;
        if (GetID)  GetID(d, &id);
        if (GetFam) GetFam(d, &fam);
        printf("  dev[%ld] id=0x%llx familyID=%d running=%d -> registering callback + start\n",
               (long)i, (unsigned long long)id, fam, IsRunning ? IsRunning(d) : -1);
        Register(d, frame_cb);
        Start(d, 0);
    }

    signal(SIGINT, on_sigint);
    printf("listening for frames; touch the pad. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
