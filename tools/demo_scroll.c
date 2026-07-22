/* demo_scroll - two-finger scroll demo through our kext. Two fingers (spread in x) glide
 * together in y, FREEZE ~1.5s, glide again, lift. The mid-gesture freeze is the tell that
 * it's commanded. Drives Apple's real recognizer -> scroll-wheel events. */
#include "../src/touch_model.h"
#include "../src/mavericks_amd_terminal_encode.h"
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
static void snd(int on, int y) {
    touch_frame_t f; f.button = 0; f.timestamp = 0; f.ntouches = on ? 2 : 0;
    int offs[2] = { -900, 900 };
    for (int i = 0; on && i < 2; i++) {
        touch_t *t = &f.touches[i];
        t->id = 7 + i; t->state = TS_TOUCHING; t->x = offs[i]; t->y = y;
        t->touch_major = 200; t->touch_minor = 180; t->orientation = 0; t->size = 50;
    }
    uint8_t o[256];
    int n = mavericks_amd_construct_report(&f, o, sizeof(o), ems());
    IOConnectCallStructMethod(c, 0, o, (size_t)n, NULL, NULL);
    usleep(16000);
}

int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("MavericksVoodooInputHost"));
    if (!s) { fprintf(stderr, "MavericksVoodooInputHost service not found\n"); return 1; }
    IOServiceOpen(s, mach_task_self(), 0, &c); IOObjectRelease(s);

    int y = 1500;
    fprintf(stderr, "settle\n");        for (int i = 0; i < 20;  i++)  snd(1, y);
    fprintf(stderr, "SCROLL 1\n");      for (int i = 0; i < 110; i++) { y -= 26; snd(1, y); }
    fprintf(stderr, "FREEZE ~1.5s\n");  for (int i = 0; i < 95;  i++)  snd(1, y);
    fprintf(stderr, "SCROLL 2\n");      for (int i = 0; i < 110; i++) { y -= 26; snd(1, y); }
    fprintf(stderr, "lift\n");          for (int i = 0; i < 10;  i++)  snd(0, 0);
    return 0;
}
