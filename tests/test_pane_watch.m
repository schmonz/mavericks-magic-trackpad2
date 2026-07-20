// Branch A watcher: which launched app should we inject VoodooInputMavericksPane into?
#import <Foundation/Foundation.h>
#include "test.h"

// Defined in tools/voodooinputmavericks_prefpane/voodooinputmavericks_pane_watch.m (compiled with -DMAVERICKS_PANE_WATCH_TEST).
BOOL mavericks_should_inject(NSString *bundleID);

int main(void) {
    @autoreleasepool {
        CHECK(mavericks_should_inject(@"com.apple.systempreferences") == YES);
        CHECK(mavericks_should_inject(@"com.apple.finder") == NO);
        CHECK(mavericks_should_inject(nil) == NO);
    }
    if (test_failures) { fprintf(stderr, "%d failure(s)\n", test_failures); return 1; }
    printf("test_pane_watch OK\n");
    return 0;
}
