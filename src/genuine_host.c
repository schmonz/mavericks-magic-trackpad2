#include "genuine_host.h"
#include <stddef.h>

int gh_start(gh_host_t *h, const gh_config_t *cfg, const gh_adapter_t *a, void *ctx, void *provider) {
    h->state = GH_IDLE; h->obj = NULL; h->ctx = ctx; h->provider = provider; h->cfg = cfg;
    void *obj = a->alloc(h);
    if (!obj) return -1;
    h->obj = obj; h->state = GH_ALLOCED;
    if (!a->class_ok(h)) { gh_stop(h, a); return -3; }  /* gate before init/attach */
    if (!a->init_attach(h)) { gh_stop(h, a); return -2; }
    h->state = GH_ATTACHED;
    if (a->interpose(h) != 0) { gh_stop(h, a); return -4; }
    h->state = GH_INTERPOSED;
    if (!a->start(h)) { gh_stop(h, a); return -5; }
    h->state = GH_STARTED;
    return 0;
}

void gh_stop(gh_host_t *h, const gh_adapter_t *a) {
    switch (h->state) {
    case GH_STARTED:    a->restore(h); a->terminate(h); a->release(h); break;
    case GH_INTERPOSED: a->restore(h); a->detach(h);    a->release(h); break;
    case GH_ATTACHED:   a->detach(h);  a->release(h); break;
    case GH_ALLOCED:    a->release(h); break;
    case GH_IDLE: default: break;
    }
    h->state = GH_IDLE; h->obj = NULL;
}
