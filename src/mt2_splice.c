/* This engine never allocates — the CLONE ops (kext or fake) own vtc_clone_override.
 * But mt2_splice.h -> vtable_clone.h #errors unless VTC_ALLOC/FREE exist, and its static
 * inlines reference them. Stub to a harmless no-op ONLY if the includer (the kext) hasn't
 * already defined the real IOMalloc-backed macros. WARNING: do NOT add a vtc_* CALL in this
 * TU — it would bind this 0-returning stub in the kext build and silently always fail. */
#ifndef VTC_ALLOC
#define VTC_ALLOC(sz)  0
#define VTC_FREE(p,sz) (void)0
#endif
#include "mt2_splice.h"
#include <string.h>

static int gate_ok(const mt2_splice_row_t *row, void *target,
                   const mt2_splice_ops_t *ops, int *out_code) {
    switch (row->gate) {
    case MT2_GATE_NONE:
        return 1;
    case MT2_GATE_SLOT_POPULATED: {
        void *cur = ops->read_slot(ops->ctx, target, row->slot);
        if (!cur || cur == row->shim) { *out_code = MT2_SPLICE_NOT_READY; return 0; }
        return 1;
    }
    case MT2_GATE_CLASS_NAME: {
        const char *c = ops->class_name(ops->ctx, target);
        if (c && (strcmp(c, row->gate_class) == 0 ||
                  (row->gate_class2 && strcmp(c, row->gate_class2) == 0)))
            return 1;
        *out_code = MT2_SPLICE_ABORT; return 0;
    }
    }
    *out_code = MT2_SPLICE_ABORT; return 0;
}

int mt2_splice_install(const mt2_splice_row_t *row, void *target,
                       const mt2_splice_ops_t *ops, mt2_splice_state_t *st) {
    if (st->installed) return MT2_SPLICE_ALREADY;

    int code = MT2_SPLICE_OK;
    if (!gate_ok(row, target, ops, &code)) return code;

    if (row->kind == MT2_SPLICE_MEM_SLOT) {
        /* Save two, write one: the L2CAP delegate ABI stores the callback at `slot` and its target at
         * the ADJACENT word (slot+8: cb +0x110, target +0x118). We capture BOTH (the shim forwards to
         * the saved target) but overwrite ONLY the callback. Target adjacency is an engine assumption —
         * see the mt2_splice_row_t comment. Capture BEFORE the write. */
        st->saved_cb     = ops->read_slot(ops->ctx, target, row->slot);
        st->saved_target = ops->read_slot(ops->ctx, target, row->slot + sizeof(void *));
        ops->write_slot(ops->ctx, target, row->slot, row->shim);
        st->installed = 1;
        return MT2_SPLICE_OK;
    }
    /* CLONE: the op clones the vtable, overrides slot(+slot2), and fills
     * st->captured_orig with the pre-override primary-slot fn (how the old gOrigUsbHandleReport was set). */
    if (ops->clone_override(ops->ctx, target, row, st) != 0)
        return MT2_SPLICE_OP_FAIL;
    st->installed = 1;
    return MT2_SPLICE_OK;
}

void mt2_splice_restore(void *target, const mt2_splice_row_t *row,
                        const mt2_splice_ops_t *ops, mt2_splice_state_t *st) {
    if (!st->installed) return;
    if (row->kind == MT2_SPLICE_MEM_SLOT) {
        void *cur = ops->read_slot(ops->ctx, target, row->slot);
        /* Restore-only-if-still-ours: if a third party overwrote the slot after us, leave it.
         * saved_cb is non-NULL after any real install (the SLOT_POPULATED gate guaranteed the
         * slot was populated), so the && is belt-and-suspenders. */
        if (cur == row->shim && st->saved_cb)
            ops->write_slot(ops->ctx, target, row->slot, st->saved_cb);
        st->installed = 0;
        return;
    }
    /* clone_restore repoints the vptr BEFORE freeing the clone (vtc_restore). */
    ops->clone_restore(ops->ctx, target, st);
    st->installed = 0;
}
