#ifndef VTABLE_CLONE_H
#define VTABLE_CLONE_H
/* Instance-scoped C++ vtable override. Clones ONE object's vtable into fresh
 * memory, overrides a single slot, and repoints the object at the copy — so only
 * this instance sees the override; the shared class vtable (and any sibling
 * instance, e.g. a co-connected genuine MT1's transport) is untouched.
 * [[mt2-dont-perturb-coconnected-apple-devices]]
 *
 * The kext defines VTC_ALLOC/VTC_FREE to IOMalloc/IOFree; the host test injects
 * malloc/free. span_bytes must cover every slot the object dispatches through
 * PLUS the two Itanium-ABI words below the vtable pointer (offset-to-top, RTTI);
 * we copy starting two words below *obj and repoint to clone+2 words. */

#ifndef VTC_ALLOC
#error "define VTC_ALLOC(sz) and VTC_FREE(p,sz) before including vtable_clone.h"
#endif

#include <string.h>

typedef struct {
    void  *orig_vptr;   /* the object's original vtable pointer (to restore)     */
    void  *clone_base;  /* allocation base (clone start = orig_vptr - 2 words)   */
    unsigned long alloc_bytes;
} vtc_clone_t;

/* Returns 0 on success, -1 on allocation failure. */
static inline int vtc_clone_override(void *obj, unsigned long span_bytes,
                                     unsigned slot, void *fn, vtc_clone_t *saved) {
    void **objp = (void **)obj;
    void  *orig_vptr = *objp;
    const unsigned long pre = 2 * sizeof(void *);          /* ABI words below vptr */
    unsigned long total = pre + span_bytes;
    char *base = (char *)VTC_ALLOC(total);
    if (!base) return -1;
    memcpy(base, (char *)orig_vptr - pre, total);
    void **clone_vtbl = (void **)(base + pre);             /* mirrors orig vptr     */
    clone_vtbl[slot] = fn;                                 /* the one override      */
    saved->orig_vptr   = orig_vptr;
    saved->clone_base  = base;
    saved->alloc_bytes = total;
    *objp = (void *)clone_vtbl;                            /* repoint the instance  */
    return 0;
}

/* Override an ADDITIONAL slot in an already-installed clone (e.g. when one query path
 * dispatches through two vtable slots). No-op if no clone is installed. */
static inline void vtc_override_slot(vtc_clone_t *saved, unsigned slot, void *fn) {
    if (!saved || !saved->clone_base) return;
    void **clone_vtbl = (void **)((char *)saved->clone_base + 2 * sizeof(void *));
    clone_vtbl[slot] = fn;
}

static inline void vtc_restore(void *obj, vtc_clone_t *saved) {
    void **objp = (void **)obj;
    *objp = saved->orig_vptr;                              /* restore FIRST         */
    if (saved->clone_base) {
        VTC_FREE(saved->clone_base, saved->alloc_bytes);
        saved->clone_base = 0;
    }
}
#endif
