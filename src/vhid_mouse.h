#ifndef VHID_MOUSE_H
#define VHID_MOUSE_H
#include <stdint.h>

typedef struct vhid_mouse vhid_mouse_t;

/* A kextless virtual mouse with buttons + relative X/Y + vertical/horizontal
 * wheel. The system drives the cursor and scrolls from its reports. */
vhid_mouse_t *vhid_mouse_create(void);

/* Send one relative-pointer event. buttons: bit0 left, bit1 right, bit2 middle.
 * dx/dy/wheel/hwheel are signed [-127,127]. Returns 0 on success. */
int vhid_mouse_send(vhid_mouse_t *v, unsigned buttons, int dx, int dy, int wheel, int hwheel);

void vhid_mouse_destroy(vhid_mouse_t *v);

#endif
