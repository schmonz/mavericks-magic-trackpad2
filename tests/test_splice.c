#include "test.h"
#include <string.h>
#define VTC_ALLOC(sz)   0
#define VTC_FREE(p,sz)  (void)0
#include "../src/mt2_splice.h"

/* ---- fake-memory ops: a slot array + a class string; clone ops model a cloned slot table -- */
#define NSLOT 32
typedef struct {
    void       *slots[NSLOT];     /* byte-offset model: slot i lives at offset i*sizeof(void*) */
    const char *cls;
    int         clone_installed;
    void       *clone_orig[NSLOT];
    int         restore_before_free_ok;
} fakemem_t;

static unsigned idx(unsigned off){ return off / sizeof(void*); }

static void *fm_read(void *ctx, void *t, unsigned off){ (void)t; return ((fakemem_t*)ctx)->slots[idx(off)]; }
static void  fm_write(void *ctx, void *t, unsigned off, void *fn){ (void)t; ((fakemem_t*)ctx)->slots[idx(off)] = fn; }
static const char *fm_class(void *ctx, void *t){ (void)t; return ((fakemem_t*)ctx)->cls; }

static int fm_clone_override(void *ctx, void *t, const mt2_splice_row_t *r, mt2_splice_state_t *st){
    (void)t; fakemem_t *m = (fakemem_t*)ctx;
    for (int i=0;i<NSLOT;i++) m->clone_orig[i] = m->slots[i];
    st->captured_orig = m->clone_orig[r->slot];
    m->slots[r->slot] = r->shim;
    if (r->slot2) m->slots[r->slot2] = r->shim2;
    m->clone_installed = 1;
    st->clone.clone_base = (void*)m;
    return 0;
}
static int fm_clone_override_fail(void *ctx, void *t, const mt2_splice_row_t *r, mt2_splice_state_t *st){
    (void)ctx;(void)t;(void)r;(void)st; return -1;
}
static void fm_clone_restore(void *ctx, void *t, mt2_splice_state_t *st){
    (void)t; fakemem_t *m = (fakemem_t*)ctx;
    for (int i=0;i<NSLOT;i++) m->slots[i] = m->clone_orig[i];
    m->restore_before_free_ok = 1;
    m->clone_installed = 0;
    st->clone.clone_base = 0;
}

static mt2_splice_ops_t mk_ops(fakemem_t *m){
    mt2_splice_ops_t o; o.read_slot=fm_read; o.write_slot=fm_write; o.class_name=fm_class;
    o.clone_override=fm_clone_override; o.clone_restore=fm_clone_restore; o.ctx=m; return o;
}

static void shimA(void){} static void shimB(void){}
static void origcb(void){} static void origtarget(void){} static void third(void){}

static void run_tests(void) {
    /* MEM_SLOT save-two-write-one */
    { fakemem_t m; memset(&m,0,sizeof m);
      unsigned S = 4*sizeof(void*);
      m.slots[4] = (void*)origcb; m.slots[5] = (void*)origtarget;
      mt2_splice_row_t row = {"mem", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0,0, S, (void*)shimA, 0,0,0};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_OK);
      CHECK(st.saved_cb == (void*)origcb);
      CHECK(st.saved_target == (void*)origtarget);
      CHECK(m.slots[4] == (void*)shimA);
      CHECK(m.slots[5] == (void*)origtarget);
      CHECK(st.installed);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_ALREADY);
      mt2_splice_restore((void*)1,&row,&ops,&st);
      CHECK(m.slots[4] == (void*)origcb);
      CHECK(!st.installed);
      mt2_splice_restore((void*)1,&row,&ops,&st);
      CHECK(m.slots[4] == (void*)origcb); }

    /* MEM_SLOT reuse (connect/disconnect): the same state struct is re-installed after a
       restore — saved_cb must RE-CAPTURE the current original, not carry stale state. */
    { fakemem_t m; memset(&m,0,sizeof m);
      unsigned S = 4*sizeof(void*);
      m.slots[4] = (void*)origcb; m.slots[5] = (void*)origtarget;
      mt2_splice_row_t row = {"mem", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0,0, S, (void*)shimA, 0,0,0};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      mt2_splice_install(&row,(void*)1,&ops,&st);
      mt2_splice_restore((void*)1,&row,&ops,&st);        /* slot back to origcb */
      m.slots[4] = (void*)third; m.slots[5] = (void*)origtarget;  /* a NEW connection: different original */
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_OK);
      CHECK(st.saved_cb == (void*)third);                /* re-captured the NEW original, not stale origcb */
      CHECK(m.slots[4] == (void*)shimA);
      mt2_splice_restore((void*)1,&row,&ops,&st);
      CHECK(m.slots[4] == (void*)third); }               /* restored the NEW original */

    /* MEM_SLOT gate SLOT_POPULATED: null slot -> NOT_READY */
    { fakemem_t m; memset(&m,0,sizeof m);
      unsigned S = 4*sizeof(void*);
      mt2_splice_row_t row = {"mem", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0,0, S, (void*)shimA, 0,0,0};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_NOT_READY);
      CHECK(!st.installed); CHECK(m.slots[4] == 0); }

    /* MEM_SLOT gate: already our shim -> NOT_READY */
    { fakemem_t m; memset(&m,0,sizeof m);
      unsigned S = 4*sizeof(void*);
      m.slots[4] = (void*)shimA;
      mt2_splice_row_t row = {"mem", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0,0, S, (void*)shimA, 0,0,0};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_NOT_READY); }

    /* MEM_SLOT restore-only-if-still-ours: third party overwrote -> LEAVE ALONE */
    { fakemem_t m; memset(&m,0,sizeof m);
      unsigned S = 4*sizeof(void*);
      m.slots[4] = (void*)origcb; m.slots[5] = (void*)origtarget;
      mt2_splice_row_t row = {"mem", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0,0, S, (void*)shimA, 0,0,0};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      mt2_splice_install(&row,(void*)1,&ops,&st);
      m.slots[4] = (void*)third;
      mt2_splice_restore((void*)1,&row,&ops,&st);
      CHECK(m.slots[4] == (void*)third);
      CHECK(!st.installed); }

    /* CLONE capture-from-clone + two-slot override + restore-before-free */
    { fakemem_t m; memset(&m,0,sizeof m);
      m.slots[3] = (void*)origcb; m.slots[7] = (void*)origtarget;
      mt2_splice_row_t row = {"clone", MT2_SPLICE_VTABLE_CLONE, MT2_GATE_NONE, 0,0,
                              3, (void*)shimA, 7, (void*)shimB, 0x2000};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_OK);
      CHECK(st.captured_orig == (void*)origcb);
      CHECK(m.slots[3] == (void*)shimA);
      CHECK(m.slots[7] == (void*)shimB);
      mt2_splice_restore((void*)1,&row,&ops,&st);
      CHECK(m.restore_before_free_ok);
      CHECK(m.slots[3] == (void*)origcb); CHECK(m.slots[7] == (void*)origtarget);
      CHECK(!st.installed); }

    /* CLONE gate CLASS_NAME: wrong class -> ABORT */
    { fakemem_t m; memset(&m,0,sizeof m); m.cls = "SomethingElse";
      mt2_splice_row_t row = {"clone", MT2_SPLICE_VTABLE_CLONE, MT2_GATE_CLASS_NAME,
                              "BNBTrackpadDevice", "BluetoothMultitouchTransport",
                              3, (void*)shimA, 0,0, 0x2000};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_ABORT);
      CHECK(!st.installed); CHECK(!m.clone_installed); }

    /* CLONE gate CLASS_NAME: 2nd accepted class -> OK */
    { fakemem_t m; memset(&m,0,sizeof m); m.cls = "BluetoothMultitouchTransport";
      m.slots[3] = (void*)origcb;
      mt2_splice_row_t row = {"clone", MT2_SPLICE_VTABLE_CLONE, MT2_GATE_CLASS_NAME,
                              "BNBTrackpadDevice", "BluetoothMultitouchTransport",
                              3, (void*)shimA, 0,0, 0x2000};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m);
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_OK); }

    /* CLONE op failure -> OP_FAIL, state clean */
    { fakemem_t m; memset(&m,0,sizeof m);
      mt2_splice_row_t row = {"clone", MT2_SPLICE_VTABLE_CLONE, MT2_GATE_NONE, 0,0,
                              3, (void*)shimA, 0,0, 0x2000};
      mt2_splice_state_t st; memset(&st,0,sizeof st);
      mt2_splice_ops_t ops = mk_ops(&m); ops.clone_override = fm_clone_override_fail;
      CHECK_EQ(mt2_splice_install(&row,(void*)1,&ops,&st), MT2_SPLICE_OP_FAIL);
      CHECK(!st.installed);
      mt2_splice_restore((void*)1,&row,&ops,&st); }
}
TEST_MAIN()
