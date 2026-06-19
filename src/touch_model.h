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
} touch_frame_t;

#endif
