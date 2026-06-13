/* Minimal MultitouchSupport lister: how many devices does MTDeviceCreateList see?
 * Build: clang -O2 -o /tmp/mt_list tools/mt_list.c -F/System/Library/PrivateFrameworks \
 *        -framework CoreFoundation -framework MultitouchSupport
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

extern CFArrayRef MTDeviceCreateList(void);

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("MTDeviceCreateList: %ld multitouch device(s)\n", (long)n);
    return (int)n;
}
