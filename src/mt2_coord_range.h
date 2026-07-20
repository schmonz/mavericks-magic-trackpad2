#ifndef MT2_COORD_RANGE_H
#define MT2_COORD_RANGE_H
/* MT2 device-unit coordinate bounds (from Linux hid-magicmouse.c). The encoder's INPUT
 * range and the translator's OUTPUT range — one source of truth. */
#define MT2_MIN_X (-3678)
#define MT2_MAX_X  3934
#define MT2_MIN_Y (-2478)
#define MT2_MAX_Y  2587

/* Full device-unit span per axis. A VoodooInput satellite that advertises these as its
 * Logical Max makes the mux's fixed-point rescale an EXACT inverse of the emit rescale
 * (wire = mt2 - min; mux: min + wire). One source of truth for the BT satellite's advertised
 * dimensions AND mavericks_voodoo_from_frame's output scaling. */
#define MT2_SPAN_X (MT2_MAX_X - MT2_MIN_X)   /* 7612 */
#define MT2_SPAN_Y (MT2_MAX_Y - MT2_MIN_Y)   /* 5065 */
#endif
