/* mt2d: Magic Trackpad 2 -> usable pointer on Mavericks.
 *
 * Reads raw MT2 multitouch frames (via the MT2Claim kext + USB), decodes them,
 * synthesizes cursor / two-finger scroll / click, and injects them through a
 * kextless virtual mouse the system fully understands. */
#include "mt2_reader.h"
#include "mt2_decode.h"
#include "gesture.h"
#include "vhid_mouse.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <signal.h>

static vhid_mouse_t *g_mouse;
static gesture_state_t *g_gesture;

static void on_frame(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
    touch_frame_t tf = {0};
    if (mt2_decode(frame, len, &tf) != 0) return;
    mouse_report_t m;
    if (gesture_process(g_gesture, &tf, &m))
        vhid_mouse_send(g_mouse, m.buttons, m.dx, m.dy, m.wheel_v, m.wheel_h);
}

static void on_sig(int s) {
    (void)s;
    mt2_reader_stop();
    if (g_mouse) vhid_mouse_destroy(g_mouse);
    _exit(0);
}

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    g_mouse = vhid_mouse_create();
    if (!g_mouse) { fprintf(stderr, "vhid_mouse_create failed (run as root)\n"); return 1; }
    g_gesture = gesture_create();

    if (mt2_reader_start(on_frame, NULL) != 0) {
        fprintf(stderr, "mt2_reader_start failed (run as root; is the MT2Claim kext loaded?)\n");
        vhid_mouse_destroy(g_mouse);
        return 1;
    }
    printf("mt2d running: cursor + two-finger scroll + click. Ctrl-C to stop.\n");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
