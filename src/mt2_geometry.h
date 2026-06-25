#ifndef MT2_GEOMETRY_H
#define MT2_GEOMETRY_H
/* Pure, transport-neutral source of the Magic Trackpad 2 sensor-geometry D-report
 * payloads. Both the fDevice get-report handler (kext) and the full-BNB transport
 * override answer the recognizer's geometry queries from here, so the constants live
 * in exactly one place. Byte layouts reversed from
 * AppleMultitouchDevice::decodeDeviceProperty (see the RE findings). */

/* Sensor SURFACE dimensions — the GENUINE MT2's reported values (15600 × 11040 = 156.0 × 110.4 mm),
 * ground-truthed 2026-06-25 from a real MT2 via ioreg (docs/mt-stack/reference.md). Exposed here so the
 * BT geometry report AND the genuine-USB init dict seed ONE value (no drift). NOTE: the surface dims are
 * NOT the edge-dead-zone fix (that's the Sensor Rows/Cols + Region in mt2_geometry.c); these are just
 * the correct surface numbers. */
#define MT2_SURFACE_WIDTH  15600u
#define MT2_SURFACE_HEIGHT 11040u

/* Genuine MT2 sensor grid + Family (ground-truthed via ioreg). Half these (16x13, Family 0x80) was the
 * edge-dead-zone root cause. */
#define MT2_FAMILY_ID      0x81u   /* 129 */
#define MT2_SENSOR_ROWS    0x16u   /* 22 */
#define MT2_SENSOR_COLS    0x1eu   /* 30 */

#ifdef __cplusplus
extern "C" {
#endif

/* Genuine MT2 raw descriptor bytes — ONE source of truth, shared by the BT geometry reports
 * (mt2_geometry.c: 0xd0/0xa1/0xd9) and the genuine-USB init dict (usb_build_init_props). The Surface
 * Descriptor's first 8 bytes are Width/Height (= MT2_SURFACE_WIDTH/HEIGHT) u32 LE; mt2_surface_desc_tail
 * is the 8 bytes after them. See docs/mt-stack/reference.md. */
extern const unsigned char mt2_region_descriptor[15];
extern const unsigned char mt2_region_param[6];
extern const unsigned char mt2_surface_desc_tail[8];

enum {
    MT2_GEO_OK            = 0,  /* filled out[0..*outLen) for a geometry report id   */
    MT2_GEO_UNSUPPORTED   = 1,  /* a known id the driver must SKIP (0xDB Multitouch ID) */
    MT2_GEO_NOT_GEOMETRY  = 2   /* not a geometry id; caller decides what to do      */
};

/* reportId: the MT D-report id. out: caller buffer (>= 16 bytes). outLen: set to the
 * payload length on MT2_GEO_OK. Returns one of the enum values above. */
int mt2_fill_geometry_report(unsigned char reportId, unsigned char *out, unsigned int *outLen);

#ifdef __cplusplus
}
#endif
#endif
