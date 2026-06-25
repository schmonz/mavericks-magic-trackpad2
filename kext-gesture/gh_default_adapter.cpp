/* gh_default_adapter — generic IOKit adapter callbacks shared by all genuine_host clients. See the
 * header. These are kernel-only (they touch IOService/OSMetaClass), which is why they live here and not
 * in the pure-C, host-tested src/genuine_host.c. */
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSDictionary.h>
#include <string.h>
#include "gh_default_adapter.h"

void *gh_default_alloc(gh_host_t *h) {
    OSObject *o = OSMetaClass::allocClassWithName(h->cfg->driver_class);
    IOService *s = OSDynamicCast(IOService, o);
    if (!s && o) o->release();
    return s;
}

bool gh_default_class_ok(gh_host_t *h) {
    const char *cls = ((IOService *)h->obj)->getMetaClass()->getClassName();
    return cls && strcmp(cls, h->cfg->safety_class) == 0;
}

bool gh_default_init_attach(gh_host_t *h) {
    OSDictionary *initp = h->cfg->build_props ? (OSDictionary *)h->cfg->build_props() : 0;
    bool ok = initp && ((IOService *)h->obj)->init(initp)
                    && ((IOService *)h->obj)->attach((IOService *)h->provider);
    if (initp) initp->release();
    return ok;
}

bool gh_default_start(gh_host_t *h)     { return ((IOService *)h->obj)->start((IOService *)h->provider); }
void gh_default_detach(gh_host_t *h)    { ((IOService *)h->obj)->detach((IOService *)h->provider); }
void gh_default_terminate(gh_host_t *h) { ((IOService *)h->obj)->terminate(); }
void gh_default_release(gh_host_t *h)   { ((IOService *)h->obj)->release(); }
