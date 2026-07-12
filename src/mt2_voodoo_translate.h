#ifndef MT2_VOODOO_TRANSLATE_H
#define MT2_VOODOO_TRANSLATE_H
#include "voodoo_wire.h"   /* wire: VoodooInputEvent (C++-only header) */
#include "mt2_frame.h"     /* internal: mt2_frame */
#include <stdint.h>
/* Translate one wire VoodooInputEvent (satellite-authored) into our internal mt2_frame:
 * rescale each contact's [0..logical_max] coords into MT2 device units, OR per-contact
 * button into the frame, carry id/pressure/width, and zero the lifecycle/ellipse tail
 * (mt2_lifecycle derives state; mt1_encode defaults radii). logical_max_{x,y}==0 => identity.
 * Declared extern "C" so the (C++) kext + tests share a stable symbol. */
#ifdef __cplusplus
extern "C" {
#endif
mt2_frame mt2_frame_from_voodoo(const VoodooInputEvent *wire,
                                uint32_t logical_max_x, uint32_t logical_max_y);
#ifdef __cplusplus
}
#endif
#endif
