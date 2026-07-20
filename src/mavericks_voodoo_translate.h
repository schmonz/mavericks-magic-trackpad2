#ifndef MAVERICKS_VOODOO_TRANSLATE_H
#define MAVERICKS_VOODOO_TRANSLATE_H
#include "voodoo_wire.h"   /* wire: VoodooInputEvent (C++-only header) */
#include "mavericks_frame.h"     /* internal: MavericksTouchFrame */
#include <stdint.h>
/* Translate one wire VoodooInputEvent (satellite-authored) into our internal MavericksTouchFrame:
 * rescale each contact's [0..logical_max] coords into MT2 device units, OR per-contact
 * button into the frame, carry id/pressure/width, and zero the lifecycle/ellipse tail
 * (mavericks_lifecycle derives state; mavericks_amd_construct_report defaults radii). logical_max_{x,y}==0 => identity.
 * Declared extern "C" so the (C++) kext + tests share a stable symbol.
 *
 * CONTRACT: a satellite must report pressure > 0 for an active contact — the shared engine's
 * mavericks_drop_lifted gates presence on pressure, and isTransducerActive is NOT consulted here.
 * A conformant-but-pressureless satellite would drop to no-contacts; synthesizing a pressure
 * floor from isTransducerActive is a sub-project-2 (terminal-consumer) decision, not done here. */
#ifdef __cplusplus
extern "C" {
#endif
MavericksTouchFrame mavericks_frame_from_voodoo(const VoodooInputEvent *wire,
                                uint32_t logical_max_x, uint32_t logical_max_y);

/* Inverse of mavericks_frame_from_voodoo: pack an internal MavericksTouchFrame back into a wire VoodooInputEvent
 * for a satellite to emit. Carries ONLY what VoodooInputTransducer holds (id/x/y/pressure/width +
 * the frame button); the "beyond VoodooInput" tail (state/touch_major/touch_minor/orientation) is
 * NOT wire-representable and is dropped. With logical_max == the axis span (MT2_SPAN_*), the
 * round-trip mavericks_frame_from_voodoo(mavericks_voodoo_from_frame(f)) is exact on the carried fields.
 * logical_max_{x,y}==0 => identity (two's-complement passthrough), mirroring the forward fn. */
VoodooInputEvent mavericks_voodoo_from_frame(const MavericksTouchFrame *f,
                                       uint32_t logical_max_x, uint32_t logical_max_y);
#ifdef __cplusplus
}
#endif
#endif
