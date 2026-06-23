#include "test.h"
#include <stdlib.h>
#include <string.h>

/* Inject host allocators before including the header. */
#define VTC_ALLOC(sz)   malloc(sz)
#define VTC_FREE(p,sz)  free(p)
#include "vtable_clone.h"

typedef long (*slotfn)(void);
static long orig_slot3(void){ return 30; }
static long orig_slot5(void){ return 50; }
static long our_slot3(void){ return 999; }

static void run_tests(void) {
    /* Fake vtable with 2 leading Itanium-ABI pad words (offset-to-top, RTTI) so the
     * helper's `pre` copy reads valid memory; obj.vptr points PAST the pad at slot 0. */
    void *vt_storage[10];                 /* 2 ABI words + 8 slots */
    void **vt = vt_storage + 2;           /* obj.vptr points here  */
    int i; for (i=0;i<8;i++) vt[i] = (void*)0;
    vt[3] = (void*)orig_slot3;
    vt[5] = (void*)orig_slot5;

    struct { void *vptr; int payload; } obj;
    obj.vptr = (void*)vt;
    obj.payload = 7;

    vtc_clone_t saved;
    /* Clone obj's vtable covering 8 slots, override slot 3 with our fn. */
    CHECK_EQ(vtc_clone_override(&obj, /*span_bytes*/ 8 * sizeof(void *), /*slot*/3,
                               (void*)our_slot3, &saved), 0);

    /* Object now dispatches the cloned vtable. */
    CHECK(obj.vptr != (void*)vt);
    slotfn s3 = (slotfn)(((void**)obj.vptr)[3]);
    slotfn s5 = (slotfn)(((void**)obj.vptr)[5]);
    CHECK_EQ(s3(), 999);   /* overridden */
    CHECK_EQ(s5(), 50);    /* untouched, copied through */
    CHECK_EQ(obj.payload, 7);

    /* Restore puts the original vtable pointer back and frees the clone. */
    vtc_restore(&obj, &saved);
    CHECK(obj.vptr == (void*)vt);
    slotfn s3b = (slotfn)(((void**)obj.vptr)[3]);
    CHECK_EQ(s3b(), 30);
}
TEST_MAIN()
