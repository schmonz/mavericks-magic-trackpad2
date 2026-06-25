#include "genuine_host.h"
#include <stddef.h>

int gh_start(gh_host_t *h, const gh_config_t *cfg, const gh_adapter_t *a, void *ctx) {
    h->state = GH_IDLE; h->obj = NULL;
    void *obj = a->alloc(ctx, cfg->driver_class);
    if (!obj) return -1;
    h->obj = obj; h->state = GH_ALLOCED;
    if (!a->class_ok(ctx, obj, cfg->safety_class)) { gh_stop(h, a, ctx); return -3; }  /* gate before init/attach */
    if (!a->init_attach(ctx, obj)) { gh_stop(h, a, ctx); return -2; }
    h->state = GH_ATTACHED;
    if (a->interpose(ctx, obj) != 0) { gh_stop(h, a, ctx); return -4; }
    h->state = GH_INTERPOSED;
    if (!a->start(ctx, obj)) { gh_stop(h, a, ctx); return -5; }
    h->state = GH_STARTED;
    return 0;
}

void gh_stop(gh_host_t *h, const gh_adapter_t *a, void *ctx) {
    void *obj = h->obj;
    switch (h->state) {
    case GH_STARTED:    a->restore(ctx, obj); a->terminate(ctx, obj); a->release(ctx, obj); break;
    case GH_INTERPOSED: a->restore(ctx, obj); a->detach(ctx, obj);    a->release(ctx, obj); break;
    case GH_ATTACHED:   a->detach(ctx, obj);  a->release(ctx, obj); break;
    case GH_ALLOCED:    a->release(ctx, obj); break;
    case GH_IDLE: default: break;
    }
    h->state = GH_IDLE; h->obj = NULL;
}
