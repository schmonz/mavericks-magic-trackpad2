#ifndef MT2_SPLICE_H
#define MT2_SPLICE_H
/* Declarative interpose/splice engine. A seam is a const row (kind + gate + slots + shims);
 * per-attempt saved-originals live in state; the engine reaches hardware only through ops.
 * The kext backs ops with IOKit + vtable_clone.h; host tests back them with fake memory.
 * The engine owns WHAT happens (save-before-write, gate, restore-only-if-still-ours,
 * idempotent, restore-before-free), never WHEN — callers invoke it at each seam's existing
 * site/thread. See docs/mt-stack/explanation.md "The refactoring shape". */
#include "vtable_clone.h"   /* vtc_clone_t (state carries one) */

typedef enum { MT2_SPLICE_MEM_SLOT, MT2_SPLICE_VTABLE_CLONE } mt2_splice_kind_t;
typedef enum { MT2_GATE_NONE, MT2_GATE_SLOT_POPULATED, MT2_GATE_CLASS_NAME } mt2_splice_gate_t;

/* install() return codes (negative = failure the caller must handle). */
#define MT2_SPLICE_OK          0
#define MT2_SPLICE_NOT_READY   1   /* gate SLOT_POPULATED: slot null, or already our shim */
#define MT2_SPLICE_ALREADY     2   /* st->installed already (idempotent re-attempt) */
#define MT2_SPLICE_ABORT      (-2) /* gate CLASS_NAME mismatch */
#define MT2_SPLICE_OP_FAIL    (-1) /* clone_override failed (alloc) */

/* MEM_SLOT ABI ASSUMPTION: the original target is read from slot+sizeof(void*)
 * (cb-then-target adjacency, true for both L2CAP delegate seams). A future MEM_SLOT seam whose target
 * is NOT adjacent would need a target_off field here — it is NOT expressible today. */
typedef struct {
    const char        *name;        /* logs: "bt-interrupt" etc. */
    mt2_splice_kind_t  kind;
    mt2_splice_gate_t  gate;
    const char        *gate_class;  /* CLASS_NAME: required class (or first accepted) */
    const char        *gate_class2; /* CLASS_NAME: optional 2nd accepted class; 0 = none */
    unsigned           slot;        /* MEM_SLOT: byte offset of the cb; CLONE: primary vtable slot INDEX */
    void              *shim;        /* fn installed at slot */
    unsigned           slot2;       /* CLONE: 2nd slot index (0 = unused) */
    void              *shim2;       /* CLONE: 2nd slot fn */
    unsigned           span;        /* CLONE: vtable span bytes */
} mt2_splice_row_t;

typedef struct {
    int          installed;
    void        *saved_cb;          /* MEM_SLOT: original cb at slot */
    void        *saved_target;      /* MEM_SLOT: original target at slot+sizeof(void*) */
    vtc_clone_t  clone;             /* CLONE: the clone bookkeeping */
    void        *captured_orig;     /* CLONE: original primary-slot fn, read from the clone;
                                     * only the PRIMARY clone slot's original is captured;
                                     * a future 2-slot seam that must chain BOTH would need captured_orig2. */
} mt2_splice_state_t;

typedef struct {
    void       *(*read_slot)(void *ctx, void *target, unsigned off);
    void        (*write_slot)(void *ctx, void *target, unsigned off, void *fn);
    const char *(*class_name)(void *ctx, void *target);
    int         (*clone_override)(void *ctx, void *target,
                                  const mt2_splice_row_t *r, mt2_splice_state_t *st);
    void        (*clone_restore)(void *ctx, void *target, mt2_splice_state_t *st);
    void       *ctx;
} mt2_splice_ops_t;

/* CONTRACT: target must be non-NULL — the CLASS_NAME gate and the ops dereference it. Callers
 * that can see a NULL target (e.g. the BT geometry install) guard it before calling. */
int  mt2_splice_install(const mt2_splice_row_t *row, void *target,
                        const mt2_splice_ops_t *ops, mt2_splice_state_t *st);
void mt2_splice_restore(void *target, const mt2_splice_row_t *row,
                        const mt2_splice_ops_t *ops, mt2_splice_state_t *st);
#endif
