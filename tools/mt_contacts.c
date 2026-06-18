/* mt_contacts - print per-frame finger positions from MultitouchSupport, so we can
 * see whether our fed contacts actually TRACK the finger (vs being present-but-static
 * or mis-mapped). Run as root with our device present. Move one finger slowly and
 * watch normalized x/y (0..1 across the surface) change. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *MTDeviceRef;
typedef struct { float x, y; } MTPoint;
typedef struct { MTPoint pos, vel; } MTReadout;
typedef struct {                 /* the classic reverse-engineered MTTouch layout */
    int   frame;
    double timestamp;
    int   identifier;
    int   state;
    int   fingerID;
    int   handID;
    MTReadout normalized;        /* position 0..1, velocity */
    float size;
    int   pressure;
    float angle, majorAxis, minorAxis;
    MTReadout absolute;
    int   unk1, unk2;
    float zDensity;
} MTTouch;

typedef int (*MTContactCallback)(MTDeviceRef, MTTouch *, int, double, int);
extern CFArrayRef MTDeviceCreateList(void);
extern void MTRegisterContactFrameCallback(MTDeviceRef, MTContactCallback);
extern void MTDeviceStart(MTDeviceRef, int);

static int onFrame(MTDeviceRef d, MTTouch *touches, int n, double ts, int frame) {
    (void)d;
    if (n <= 0) {                                  /* show empty frames too (the lift) */
        printf("frame %d ts=%.4f  (no contacts)\n", frame, ts);
        fflush(stdout);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        MTTouch *t = &touches[i];
        /* state, size, pressure, and zDensity are the tap-strength inputs; ts gives duration. */
        printf("frame %d ts=%.4f id=%d st=%d fingerID=%d  norm(%.3f,%.3f) sz=%.2f press=%d maj=%.1f min=%.1f den=%.2f\n",
               frame, ts, t->identifier, t->state, t->fingerID,
               t->normalized.pos.x, t->normalized.pos.y, t->size, t->pressure,
               t->majorAxis, t->minorAxis, t->zDensity);
    }
    fflush(stdout);
    return 0;
}

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    if (!list || CFArrayGetCount(list) == 0) { fprintf(stderr, "no MT devices\n"); return 1; }
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
        MTRegisterContactFrameCallback(d, onFrame);
        MTDeviceStart(d, 0);
    }
    printf("watching contacts - move ONE finger slowly. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
