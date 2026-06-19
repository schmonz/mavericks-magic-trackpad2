/* click_monitor - listen-only CGEventTap that logs mouse down/up events, so we can
 * verify (without a human) whether the gesture recognizer committed a tap-to-click.
 * Runs for N seconds (argv[1], default 6) then prints a summary and exits.
 * Run as root. */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>

static int g_clicks = 0;
static double g_prev_ms = -1;

static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef e, void *ctx) {
    (void)proxy; (void)ctx;
    const char *name = 0;
    switch (type) {
        case kCGEventLeftMouseDown:   name = "LeftDown";  break;
        case kCGEventLeftMouseUp:     name = "LeftUp";    break;
        case kCGEventLeftMouseDragged:name = "LeftDrag";  break;
        case kCGEventRightMouseDown:  name = "RightDown"; break;
        case kCGEventRightMouseUp:    name = "RightUp";   break;
        default: return e;
    }
    CGPoint p = CGEventGetLocation(e);
    /* event timestamp -> ms; dt = gap since previous event (reveals double-clicks). */
    double ms = (double)CGEventGetTimestamp(e) / 1.0e6;
    double dt = (g_prev_ms < 0) ? 0.0 : (ms - g_prev_ms);
    g_prev_ms = ms;
    if (type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown) g_clicks++;
    printf("%-9s at (%.0f,%.0f)  dt=%+8.1fms\n", name, p.x, p.y, dt);
    fflush(stdout);
    return e;
}

int main(int argc, char **argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 6;
    CGEventMask mask = CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
                       CGEventMaskBit(kCGEventLeftMouseDragged) |
                       CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp);
    CFMachPortRef tap = CGEventTapCreate(kCGHIDEventTap, kCGTailAppendEventTap,
                                         kCGEventTapOptionListenOnly, mask, cb, NULL);
    if (!tap) { fprintf(stderr, "click_monitor: CGEventTapCreate FAILED (permission?)\n"); return 2; }
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    printf("click_monitor: listening for %ds...\n", secs);
    fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, (CFTimeInterval)secs, false);
    printf("click_monitor: done, %d click event(s) seen\n", g_clicks);
    return g_clicks > 0 ? 0 : 1;
}
