#include <IOKit/IOLib.h>                 /* IOMalloc / IOFree */
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
/* Define the vtable_clone allocator macros BEFORE the splice headers pull vtable_clone.h in
 * (mt2_splice_kext.h -> ../src/mt2_splice.h -> vtable_clone.h, which #errors without them). */
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "mt2_splice_kext.h"
#include "vtable_clone.h"

static void *ks_read(void *ctx, void *t, unsigned off) {
    (void)ctx; return *(void **)((char *)t + off);
}
static void ks_write(void *ctx, void *t, unsigned off, void *fn) {
    (void)ctx; *(void **)((char *)t + off) = fn;
}
static const char *ks_class(void *ctx, void *t) {
    (void)ctx;
    const OSMetaClass *mc = ((IOService *)t)->getMetaClass();
    return mc ? mc->getClassName() : 0;
}
/* Clone the target's vtable, override the primary slot (+2nd if set), and read the ORIGINAL
 * primary-slot fn from the clone's preserved copy into captured_orig — exactly how
 * gOrigUsbHandleReport was set (gUsbVtableClone.orig_vptr[slot]). */
static int ks_clone_override(void *ctx, void *t, const mt2_splice_row_t *r, mt2_splice_state_t *st) {
    (void)ctx;
    if (vtc_clone_override(t, r->span, r->slot, r->shim, &st->clone) != 0) return -1;
    if (r->slot2) vtc_override_slot(&st->clone, r->slot2, r->shim2);
    st->captured_orig = ((void **)st->clone.orig_vptr)[r->slot];
    return 0;
}
static void ks_clone_restore(void *ctx, void *t, mt2_splice_state_t *st) {
    (void)ctx; vtc_restore(t, &st->clone);
}

const mt2_splice_ops_t mt2_splice_kext_ops = {
    ks_read, ks_write, ks_class, ks_clone_override, ks_clone_restore, 0
};
