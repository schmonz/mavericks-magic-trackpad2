#ifndef MT2_FRAME_H
#define MT2_FRAME_H
#include <stdint.h>
#include <stddef.h>

/* The decoded contact-set both transport readers reduce to, and the shared engine
 * (lifecycle + mt1_encode) consumes. Shaped after acidanthera's VoodooInput contact
 * interface (VoodooInputMessage.h) with our own field-for-field types, so the seam
 * speaks a standard multitouch-event vocabulary.
 *
 * Fields VoodooInput does not have but mt1_encode requires (the MTTouchState lifecycle
 * plus MT2 ellipse geometry) live in the clearly marked "beyond VoodooInput" block;
 * do NOT drop them. Fields VoodooInput has but nothing here reads yet are defined for
 * interface fidelity and left zero-initialized. Full field mapping + rationale:
 * docs/mt-stack/reader-seam-map.md. */

#define MT2_MAX_CONTACTS 16
#define MAX_TOUCHES MT2_MAX_CONTACTS   /* legacy alias, same value */

typedef enum { TS_NONE = 0, TS_START, TS_TOUCHING, TS_END } touch_state_t;

typedef struct {
    int32_t x, y;         /* device units in MT2 coordinate space */
    int32_t pressure;     /* MT2 size/pressure proxy (drop-lifted + encoded from here) */
    int32_t width;        /* unread today; zero-initialized for interface fidelity */
} mt2_coord;

typedef struct {
    uint8_t  type;                   /* VoodooInput transducer type; unread today, 0 */
    uint32_t id;                     /* stable contact id within a touch lifetime */
    uint8_t  fingerType;             /* unread today, 0 */
    uint8_t  isValid;                /* unread today, 0 */
    uint8_t  isPhysicalButtonDown;   /* per-contact button; unread (frame-level carries it), 0 */
    uint8_t  supportsPressure;       /* unread today, 0 */
    int32_t  maxPressure;            /* unread today, 0 */
    mt2_coord currentCoordinates;
    mt2_coord previousCoordinates;   /* unread today, zero-initialized */

    /* --- beyond VoodooInput: consumed by mt1_encode, do NOT drop --- */
    touch_state_t state;
    int touch_major, touch_minor;
    int orientation;
} mt2_contact;

typedef struct {
    uint32_t contact_count;
    uint64_t timestamp;              /* device frame time; 0 if unknown */
    uint8_t  isPhysicalButtonDown;   /* frame-level physical click: 0/1 */
    /* field name deliberately retains the VoodooInput seam vocabulary even though our
     * element type is mt2_contact (not a partial rename); see the header comment above. */
    mt2_contact transducers[MT2_MAX_CONTACTS];
} mt2_frame;

#endif
