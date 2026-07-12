/* mt2_synth_inject — Phase-D on-device oracle: open the MT2GestureUserClient,
 * stand up the synthetic terminal (selector 1), push a canned press/move/lift
 * sequence as mt2_frame structs (selector 2), then tear down (selector 3).
 * Run as root with the kext loaded. Watch the cursor move.
 *
 * Service: com_schmonz_MT2Gesture (same as synth_feed.c).
 * User-client selectors:
 *   1 = beginSyntheticTerminal (scalar, no args)
 *   2 = submitFrame            (struct input = one mt2_frame)
 *   3 = endSyntheticTerminal   (scalar, no args)
 */
#include "mt2_frame.h"
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* --- open the user client (mirrors synth_feed.c) --- */
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("com_schmonz_MT2Gesture"));
    if (!svc) {
        fprintf(stderr, "MT2Gesture user client not found — is the kext loaded?\n");
        return 1;
    }
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    /* --- selector 1: beginSyntheticTerminal --- */
    kr = IOConnectCallScalarMethod(conn, 1, NULL, 0, NULL, NULL);
    printf("beginSyntheticTerminal: kr=0x%x\n", kr);

    /* --- selector 2: submitFrame — press, three move steps, lift --- */
    mt2_frame f;

    /* press: one contact at pad midpoint, pressure 40, id 3 */
    memset(&f, 0, sizeof f);
    f.contact_count = 1;
    f.transducers[0].id = 3;
    f.transducers[0].state = TS_START;
    f.transducers[0].currentCoordinates.x = 0;
    f.transducers[0].currentCoordinates.y = 0;
    f.transducers[0].currentCoordinates.pressure = 40;
    f.transducers[0].touch_major = 600;
    f.transducers[0].touch_minor = 500;
    kr = IOConnectCallStructMethod(conn, 2, &f, sizeof f, NULL, NULL);
    printf("submitFrame (press):    kr=0x%x\n", kr);
    usleep(16000);

    /* move frames: sweep x from 0 toward +1200 in 3 steps */
    f.transducers[0].state = TS_TOUCHING;
    int step;
    for (step = 1; step <= 3; step++) {
        f.transducers[0].currentCoordinates.x = step * 400;
        kr = IOConnectCallStructMethod(conn, 2, &f, sizeof f, NULL, NULL);
        printf("submitFrame (move %d):  kr=0x%x  x=%d\n", step, kr,
               f.transducers[0].currentCoordinates.x);
        usleep(16000);
    }

    /* lift: contact_count = 0 */
    memset(&f, 0, sizeof f);
    f.contact_count = 0;
    kr = IOConnectCallStructMethod(conn, 2, &f, sizeof f, NULL, NULL);
    printf("submitFrame (lift):     kr=0x%x\n", kr);
    usleep(16000);

    /* --- selector 3: endSyntheticTerminal --- */
    kr = IOConnectCallScalarMethod(conn, 3, NULL, 0, NULL, NULL);
    printf("endSyntheticTerminal:  kr=0x%x\n", kr);

    IOServiceClose(conn);
    return 0;
}
