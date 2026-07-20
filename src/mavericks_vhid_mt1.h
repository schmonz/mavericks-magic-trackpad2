#ifndef MAVERICKS_VHID_MT1_H
#define MAVERICKS_VHID_MT1_H
#include <stdint.h>
#include <stddef.h>

typedef struct vhid vhid_t;

/* Create the virtual Magic Trackpad 1. Returns NULL on failure (e.g. not root). */
vhid_t *vhid_create(void);

/* Feed one HID input report (including leading report-ID byte). 0 on success. */
int vhid_send(vhid_t *v, const uint8_t *report, size_t len);

void vhid_destroy(vhid_t *v);

#endif
