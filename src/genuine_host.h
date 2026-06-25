#ifndef GENUINE_HOST_H
#define GENUINE_HOST_H
#include <stdbool.h>

/*
 * genuine_host — the shared, host-tested mechanism for hosting one of Apple's genuine drivers:
 * manual-start it, verify its class before any vtable write, instance-vtable-interpose a seam, start
 * it, and tear it all down in the RIGHT order. Both transport readers (MT2BTReader, MT2USBReader) drive
 * this; the IOKit calls are injected via gh_adapter_t so the dangerous ordering logic lives — and is
 * unit-tested — in one place. (Policy here / mechanism in the adapter, mirroring mt2_session/sink.)
 */

/* Lifecycle of a manually-hosted Apple driver. */
typedef enum {
    GH_IDLE = 0, GH_ALLOCED, GH_ATTACHED, GH_INTERPOSED, GH_STARTED
} gh_state_t;

typedef struct { gh_state_t state; void *obj; } gh_host_t;

/* Per-transport config (becomes a device-table ROW in the phase-2 engine). */
typedef struct {
    const char *driver_class;   /* allocClassWithName target */
    const char *safety_class;   /* expected getClassName() before any vtable write */
} gh_config_t;

/* Injected IOKit mechanism; ctx = the reader. Split into single-responsibility ops so the core can do a
 * STATE-AWARE unwind: a service that never reached start() is detach+release'd; a started one is
 * terminate+release'd (terminate unwinds the attach, so no detach). */
typedef struct {
    void *(*alloc)(void *ctx, const char *cls);             /* allocClassWithName + OSDynamicCast(IOService) */
    bool  (*class_ok)(void *ctx, void *obj, const char *e); /* getClassName + strcmp */
    bool  (*init_attach)(void *ctx, void *obj);            /* init(seeded dict) + attach(provider); true iff attached */
    int   (*interpose)(void *ctx, void *obj);             /* vtc_clone_override + capture orig (+extra slots); 0=ok, all-or-nothing */
    bool  (*start)(void *ctx, void *obj);                 /* start(provider) */
    void  (*restore)(void *ctx, void *obj);               /* vtc_restore */
    void  (*detach)(void *ctx, void *obj);                /* detach(provider) — pre-start cleanup only */
    void  (*terminate)(void *ctx, void *obj);             /* terminate() — started-only */
    void  (*release)(void *ctx, void *obj);               /* release() */
} gh_adapter_t;

/* alloc -> init_attach -> class_ok -> interpose -> start. On any failure, fully unwinds (state==GH_IDLE)
 * and returns nonzero; on success returns 0 and state==GH_STARTED. */
int  gh_start(gh_host_t *h, const gh_config_t *cfg, const gh_adapter_t *a, void *ctx);

/* Idempotent, state-aware unwind (safe to call twice):
 *   STARTED    -> restore -> terminate -> release
 *   INTERPOSED -> restore -> detach -> release
 *   ATTACHED   -> detach -> release
 *   ALLOCED    -> release
 *   IDLE       -> no-op */
void gh_stop(gh_host_t *h, const gh_adapter_t *a, void *ctx);

#endif
