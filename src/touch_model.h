#ifndef TOUCH_MODEL_H
#define TOUCH_MODEL_H
#include <stdint.h>
#include <stddef.h>

#define MAX_TOUCHES 16

typedef enum { TS_NONE = 0, TS_START, TS_TOUCHING, TS_END } touch_state_t;

typedef struct {
    int id;                 /* stable contact id within a touch lifetime */
    touch_state_t state;
    int x, y;               /* device units in MT2 coordinate space */
    int touch_major, touch_minor;
    int orientation;
    int size;               /* size/pressure proxy */
} touch_t;

typedef struct {
    int ntouches;
    touch_t touches[MAX_TOUCHES];
    int button;             /* physical click: 0/1 */
    double timestamp;       /* seconds; 0 if unknown */
    uint32_t ts_offset_ms;  /* relative device-time delay for this frame: the shell adds it to
                               its clock when stamping. 0 = stamp at "now". Used to space the
                               trailing liftoff (absence) frame a few ms after the BreakTouch so
                               a clean lift isn't read as two coincident liftoffs (the per-tap
                               phantom). Pure-core stays clock-free; only the relative delay. */
} touch_frame_t;

#endif
