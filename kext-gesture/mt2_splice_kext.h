#ifndef MT2_SPLICE_KEXT_H
#define MT2_SPLICE_KEXT_H
#include "../src/mt2_splice.h"
/* The kext-side ops backing mt2_splice: read/write raw pointer slots, read an IOService's
 * class name, and clone/restore a vtable via vtable_clone.h. Stateless (ctx unused) — the
 * per-seam state lives in the caller's mt2_splice_state_t. Shared by both readers. */
extern const mt2_splice_ops_t mt2_splice_kext_ops;
#endif
