/* Diagnostic: what does MultitouchSupport consider the DEFAULT device, and is our
 * device built-in / in the list? Helps decide why WindowServer won't adopt us. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *MTDeviceRef;
extern CFArrayRef MTDeviceCreateList(void);
extern MTDeviceRef MTDeviceCreateDefault(void);
extern int  MTDeviceGetDeviceID(MTDeviceRef, uint64_t *);
extern int  MTDeviceIsBuiltIn(MTDeviceRef);       /* returns bool */
extern int  MTDeviceGetDriverType(MTDeviceRef, int *);

static void describe(const char *tag, MTDeviceRef d) {
    if (!d) { printf("%s: (null)\n", tag); return; }
    uint64_t id = 0; int dt = -1;
    MTDeviceGetDeviceID(d, &id);
    MTDeviceGetDriverType(d, &dt);
    int builtin = MTDeviceIsBuiltIn(d);
    printf("%s: deviceID=0x%llx driverType=%d builtIn=%d\n", tag, id, dt, builtin);
}

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("MTDeviceCreateList: %ld device(s)\n", (long)n);
    for (CFIndex i = 0; i < n; i++) describe("  list", (MTDeviceRef)CFArrayGetValueAtIndex(list, i));
    MTDeviceRef def = MTDeviceCreateDefault();
    describe("DEFAULT", def);
    return 0;
}
