/* mt_transport - print each MT device's transport method / driver type / built-in flag.
 * The MultitouchHID plugin's gesture logic is parameterized by MTDeviceGetTransportMethod
 * + MTDeviceIsBuiltIn, so this may reveal what gates actuation for our synthetic device. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *MTDeviceRef;
extern CFArrayRef MTDeviceCreateList(void);
extern int MTDeviceGetDeviceID(MTDeviceRef, uint64_t *);
extern int MTDeviceGetDriverType(MTDeviceRef, int *);
extern int MTDeviceGetTransportMethod(MTDeviceRef, int *);
extern int MTDeviceIsBuiltIn(MTDeviceRef);
extern int MTDeviceGetFamilyID(MTDeviceRef, int *);
extern int MTDeviceGetParserType(MTDeviceRef);     /* returns the value */
extern int MTDeviceGetParserOptions(MTDeviceRef);
/* The geometry the recognizer actually scales the cursor with (cached at MTDevice
 * birth; see RE findings). Surface dims in device units, sensor dims in rows/cols. */
extern int MTDeviceGetSensorSurfaceDimensions(MTDeviceRef, int *width, int *height);
extern int MTDeviceGetSensorDimensions(MTDeviceRef, int *rows, int *cols);

int main(void) {
    CFArrayRef list = MTDeviceCreateList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("MTDeviceCreateList: %ld device(s)\n", (long)n);
    for (CFIndex i = 0; i < n; i++) {
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
        uint64_t id = 0; int dt = -1, tm = -1, fam = -1;
        MTDeviceGetDeviceID(d, &id);
        MTDeviceGetDriverType(d, &dt);
        MTDeviceGetTransportMethod(d, &tm);
        MTDeviceGetFamilyID(d, &fam);
        int builtin = MTDeviceIsBuiltIn(d);
        int pt = MTDeviceGetParserType(d);
        int po = MTDeviceGetParserOptions(d);
        int sw = -1, sh = -1, rows = -1, cols = -1;
        MTDeviceGetSensorSurfaceDimensions(d, &sw, &sh);
        MTDeviceGetSensorDimensions(d, &rows, &cols);
        printf("  dev 0x%llx: driverType=%d transportMethod=%d familyID=%d builtIn=%d parserType=%d parserOptions=%d\n",
               id, dt, tm, fam, builtin, pt, po);
        printf("           surfaceW=%d surfaceH=%d sensorRows=%d sensorCols=%d  (recognizer's cached geometry)\n",
               sw, sh, rows, cols);
    }
    return 0;
}
