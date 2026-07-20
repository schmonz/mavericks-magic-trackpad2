/* replay_capture - replay recorded REAL MT2 frames through the full translation chain
 * (mt2_usb_decode -> mavericks_amd_construct_report -> MavericksVoodooInputHost kext), to verify the pipeline end-to-end on
 * genuine hardware frames without needing the live trackpad.
 *
 *   replay_capture <capture.txt>            # decode + PRINT each frame (no kext, non-disruptive)
 *   replay_capture <capture.txt> --feed     # also push to the kext (moves the cursor)
 *
 * Capture lines look like: "len=21: 02 00 00 ... 09"  (hex bytes after the colon).
 */
#include "../src/mt2_usb_decode.h"
#include "../src/mavericks_amd_terminal_encode.h"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t ems(void) {
    static mach_timebase_info_data_t tb; static uint64_t t0;
    if (tb.denom == 0) { mach_timebase_info(&tb); t0 = mach_absolute_time(); }
    return (uint32_t)(((mach_absolute_time() - t0) * tb.numer / tb.denom) / 1000000ULL);
}

/* parse the hex bytes after the first ':' in a capture line. returns byte count. */
static int parse_line(const char *line, uint8_t *out, int cap) {
    const char *p = strchr(line, ':');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t') p++;
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <capture.txt> [--feed]\n", argv[0]); return 2; }
    int feed = (argc > 2 && strcmp(argv[2], "--feed") == 0);

    io_connect_t conn = IO_OBJECT_NULL;
    if (feed) {
        io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault,
            IOServiceMatching("com_schmonz_MavericksVoodooInputHost"));
        if (!s) { fprintf(stderr, "MavericksVoodooInputHost service not found (kext loaded?)\n"); return 1; }
        if (IOServiceOpen(s, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
            fprintf(stderr, "IOServiceOpen failed\n"); return 1; }
        IOObjectRelease(s);
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    char line[1024]; uint8_t raw[512]; unsigned long frames = 0, decoded = 0, fed = 0;
    while (fgets(line, sizeof(line), f)) {
        int len = parse_line(line, raw, sizeof(raw));
        if (len < 1) continue;
        frames++;
        touch_frame_t tf;
        if (mt2_usb_decode(raw, (size_t)len, &tf) != 0) continue;
        decoded++;
        if (!feed) {
            printf("frame %lu: ntouches=%d", frames, tf.ntouches);
            for (int i = 0; i < tf.ntouches && i < 3; i++)
                printf("  [id=%d st=%d x=%d y=%d maj=%d sz=%d]",
                       tf.touches[i].id, tf.touches[i].state, tf.touches[i].x, tf.touches[i].y,
                       tf.touches[i].touch_major, tf.touches[i].size);
            printf("\n");
        } else {
            uint8_t out[256];
            int n = mavericks_amd_construct_report(&tf, out, sizeof(out), ems());
            if (n > 0) { IOConnectCallStructMethod(conn, 0, out, (size_t)n, NULL, NULL); fed++; }
            usleep(8000); /* ~replay pacing */
        }
    }
    fclose(f);
    if (feed) {
        /* lift */
        touch_frame_t e; memset(&e, 0, sizeof(e));
        uint8_t out[256]; int n = mavericks_amd_construct_report(&e, out, sizeof(out), ems());
        if (n > 0) IOConnectCallStructMethod(conn, 0, out, (size_t)n, NULL, NULL);
        IOServiceClose(conn);
    }
    fprintf(stderr, "lines=%lu decoded=%lu fed=%lu\n", frames, decoded, fed);
    return 0;
}
