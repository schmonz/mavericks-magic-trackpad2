/* mt_cursorpos - poll the system cursor location ~20x/sec and print it when it changes.
 * Lets us detect (over SSH, no eyeballing a big screen) whether injected contacts actually
 * move the cursor. Keep hands off the real mouse/trackpad during the test. */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 12;
    CGPoint last = {-1, -1};
    int moves = 0;
    for (int i = 0; i < secs * 20; i++) {
        CGEventRef e = CGEventCreate(NULL);
        CGPoint p = CGEventGetLocation(e);
        CFRelease(e);
        if (p.x != last.x || p.y != last.y) {
            printf("t=%.2fs cursor=(%.1f, %.1f)\n", i / 20.0, p.x, p.y);
            fflush(stdout);
            if (last.x >= 0) moves++;
            last = p;
        }
        usleep(50000);
    }
    printf("=> cursor changed %d times in %ds\n", moves, secs);
    return 0;
}
