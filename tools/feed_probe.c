/* feed_probe - send a few frames to our kext and print the IOConnectCallStructMethod
 * return code, to confirm whether the data path into AppleMultitouchDevice still works. */
#include "../src/touch_model.h"
#include "../src/mavericks_amd_terminal_encode.h"
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("MavericksVoodooInputHost"));
    if (!s) { printf("service not found\n"); return 1; }
    io_connect_t c;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0, &c);
    IOObjectRelease(s);
    printf("IOServiceOpen kr=0x%x conn=%u\n", kr, c);
    if (kr) return 1;

    touch_frame_t f;
    f.ntouches = 1; f.button = 0; f.timestamp = 0;
    touch_t *t = &f.touches[0];
    t->id = 7; t->state = TS_TOUCHING; t->x = 0; t->y = 0;
    t->touch_major = 900; t->touch_minor = 800; t->orientation = 0; t->size = 60;
    uint8_t o[256];
    int n = mavericks_amd_construct_report(&f, o, sizeof(o), 1000);
    for (int i = 0; i < 5; i++) {
        kr = IOConnectCallStructMethod(c, 0, o, (size_t)n, NULL, NULL);
        printf("feed[%d] kr=0x%x n=%d\n", i, kr, n);
        usleep(16000);
    }
    IOServiceClose(c);
    return 0;
}
