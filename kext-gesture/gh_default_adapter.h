#ifndef GH_DEFAULT_ADAPTER_H
#define GH_DEFAULT_ADAPTER_H
#include "../src/genuine_host.h"

/*
 * gh_default_adapter — the generic IOKit adapter callbacks shared by every genuine_host client. They
 * use ONLY h->cfg (the class names), h->obj (the hosted IOService), and h->provider — never the
 * device-specific h->ctx — so they are identical for any device. init_attach is ALSO a default: it
 * calls the device's cfg->build_props() then init+attach, so a reader supplies only a props builder
 * (in its gh_config_t) plus its two genuinely-specific seam callbacks: interpose (install its seam) and
 * restore (remove its seam).
 */
void *gh_default_alloc(gh_host_t *h);       /* allocClassWithName(cfg->driver_class) + OSDynamicCast */
bool  gh_default_class_ok(gh_host_t *h);    /* getClassName(obj) == cfg->safety_class */
bool  gh_default_init_attach(gh_host_t *h); /* init(cfg->build_props()) + attach(provider) */
bool  gh_default_start(gh_host_t *h);       /* obj->start(provider) */
void  gh_default_detach(gh_host_t *h);      /* obj->detach(provider) */
void  gh_default_terminate(gh_host_t *h);   /* obj->terminate() */
void  gh_default_release(gh_host_t *h);     /* obj->release() */

#endif
