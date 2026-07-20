#ifndef MAVERICKS_SPLICE_KEXT_H
#define MAVERICKS_SPLICE_KEXT_H
#include "../src/mavericks_splice.h"
/* The kext-side ops backing mavericks_splice: read/write raw pointer slots, read an IOService's
 * class name, and clone/restore a vtable via vtable_clone.h. Stateless (ctx unused) — the
 * per-seam state lives in the caller's mavericks_splice_state_t. Shared by both readers. */
extern const mavericks_splice_ops_t mavericks_splice_kext_ops;
#endif
