#ifndef GENUINE_HOST_H
#define GENUINE_HOST_H
#include <stdbool.h>

/*
 * genuine_host — the shared, host-tested mechanism for hosting one of Apple's genuine drivers:
 * manual-start it, verify its class before any vtable write, instance-vtable-interpose a seam, start
 * it, and tear it all down in the RIGHT order. Both transport readers (MT2BTReader, MT2USBReader) drive
 * this; the IOKit calls are injected via gh_adapter_t so the dangerous ordering logic lives — and is
 * unit-tested — in one place. (Policy here / mechanism in the adapter, mirroring mt2_session/sink.)
 *
 * Each adapter callback receives the gh_host_t, which carries everything it needs: the hosted object
 * (obj), the IOKit provider to attach/start on (provider), the device context (ctx), and the config
 * (cfg, holding the class names). This lets the generic IOKit ops — alloc/class_ok/start/detach/
 * terminate/release — be SHARED defaults (see kext-gesture/gh_default_adapter); a device supplies only
 * its three genuinely-specific callbacks: init_attach, interpose, restore.
 */

/* Lifecycle of a manually-hosted Apple driver. */
typedef enum {
    GH_IDLE = 0, GH_ALLOCED, GH_ATTACHED, GH_INTERPOSED, GH_STARTED
} gh_state_t;

/* Per-transport config (a device-table ROW: the per-device facts the generic adapter ops read). */
typedef struct {
    const char *driver_class;   /* allocClassWithName target */
    const char *safety_class;   /* expected getClassName() before any vtable write */
    void *(*build_props)(void); /* build the seeded init dict (as OSDictionary*, returned as void*;
                                   gh_default_init_attach init+attaches it, caller releases). */
} gh_config_t;

/* The live hosting operation: state + everything the adapter callbacks read. */
typedef struct {
    gh_state_t state;
    void *obj;                  /* the hosted IOService (NULL until alloc) */
    void *ctx;                  /* device context (the reader) — for the device-specific callbacks */
    void *provider;             /* IOKit provider to init/attach/start on (IOUSBInterface / L2CAPChannel) */
    const gh_config_t *cfg;     /* class names */
} gh_host_t;

/* Injected IOKit mechanism. The generic ops have shared defaults (gh_default_*); a device overrides
 * only init_attach/interpose/restore. Split so the core can do a STATE-AWARE unwind: a service that
 * never reached start() is detach+release'd; a started one is terminate+release'd (NO detach). */
typedef struct {
    void *(*alloc)(gh_host_t *h);       /* allocClassWithName(cfg->driver_class) + OSDynamicCast(IOService) */
    bool  (*class_ok)(gh_host_t *h);    /* getClassName(obj) == cfg->safety_class */
    bool  (*init_attach)(gh_host_t *h); /* init(seeded dict) + attach(provider); true iff attached */
    int   (*interpose)(gh_host_t *h);   /* vtc_clone_override on obj + capture orig (+extra slots); 0=ok, all-or-nothing */
    bool  (*start)(gh_host_t *h);       /* obj->start(provider) */
    void  (*restore)(gh_host_t *h);     /* vtc_restore */
    void  (*detach)(gh_host_t *h);      /* obj->detach(provider) — pre-start cleanup only */
    void  (*terminate)(gh_host_t *h);   /* obj->terminate() — started-only */
    void  (*release)(gh_host_t *h);     /* obj->release() */
} gh_adapter_t;

/* alloc -> class_ok -> init_attach -> interpose -> start (class gate runs BEFORE init/attach, so a
 * misidentified service is never init'd/attached). On any failure, fully unwinds (state==GH_IDLE) and
 * returns nonzero; on success returns 0 and state==GH_STARTED. */
int  gh_start(gh_host_t *h, const gh_config_t *cfg, const gh_adapter_t *a, void *ctx, void *provider);

/* Idempotent, state-aware unwind (safe to call twice):
 *   STARTED    -> restore -> terminate -> release
 *   INTERPOSED -> restore -> detach -> release
 *   ATTACHED   -> detach -> release
 *   ALLOCED    -> release
 *   IDLE       -> no-op */
void gh_stop(gh_host_t *h, const gh_adapter_t *a);

#endif
