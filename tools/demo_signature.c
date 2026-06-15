/* demo_signature - a deliberate, unmistakable cursor demo driven ONLY through our kext:
 * one finger glides, FREEZES ~1.5s, glides again, lifts. The mid-motion freeze is the
 * tell that this is commanded (nothing random pauses+resumes). mt2d cannot produce this
 * (it only reacts to physical MT2 touches; baseline with no synth = 0 cursor events). */
#include "../src/touch_model.h"
#include "../src/mt1_encode.h"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <unistd.h>

static uint32_t ems(void) {
    static mach_timebase_info_data_t tb; static uint64_t t0;
    if (tb.denom == 0) { mach_timebase_info(&tb); t0 = mach_absolute_time(); }
    return (uint32_t)(((mach_absolute_time() - t0) * tb.numer / tb.denom) / 1000000ULL);
}

static io_connect_t c;
static void snd(int on, int x, int y) {
    touch_frame_t f; f.button = 0; f.timestamp = 0; f.ntouches = on ? 1 : 0;
    if (on) {
        touch_t *t = &f.touches[0];
        t->id = 7; t->state = TS_TOUCHING; t->x = x; t->y = y;
        t->touch_major = 200; t->touch_minor = 180; t->orientation = 0; t->size = 50;
    }
    uint8_t o[256];
    int n = mt1_encode(&f, o, sizeof(o), ems());
    IOConnectCallStructMethod(c, 0, o, (size_t)n, NULL, NULL);
    usleep(16000);
}

int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("com_schmonz_MT2Gesture"));
    if (!s) { fprintf(stderr, "MT2Gesture service not found\n"); return 1; }
    IOServiceOpen(s, mach_task_self(), 0, &c); IOObjectRelease(s);

    int x = -1800;
    fprintf(stderr, "settle\n");        for (int i = 0; i < 20;  i++)  snd(1, x, 0);
    fprintf(stderr, "DRIFT 1\n");       for (int i = 0; i < 110; i++) { x += 30; snd(1, x, 0); }
    fprintf(stderr, "FREEZE ~1.5s\n");  for (int i = 0; i < 95;  i++)  snd(1, x, 0);
    fprintf(stderr, "DRIFT 2\n");       for (int i = 0; i < 110; i++) { x += 30; snd(1, x, 0); }
    fprintf(stderr, "stop\n");          for (int i = 0; i < 10;  i++)  snd(0, 0, 0);
    return 0;
}
