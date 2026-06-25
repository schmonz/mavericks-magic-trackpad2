#include "mt2_geometry.h"

/* Calibrated to the GENUINE Magic Trackpad 2 — values ground-truthed 2026-06-25 from a real MT2 via
 * ioreg (docs/mt-stack/reference.md). Family 129, Rows 22, Cols 30, Surface 15600x11040, PLUS the
 * device's real Sensor Region Descriptor/Param and Surface Descriptor bytes. We previously seeded a
 * HALF-RESOLUTION grid (16x13) + ZEROED region, which shrank the recognizer's position-normalization
 * rectangle to ~half the pad -> the perpendicular-axis edge dead zones. Surface dims live in
 * mt2_geometry.h (Family/Rows/Cols/Surface live there — shared with the genuine-USB init-dict seed). */

/* Genuine raw descriptor bytes (exactly what a real MT2 reports — the ioreg property values). Declared
 * extern in mt2_geometry.h so the genuine-USB init dict seeds the SAME bytes (one source of truth). */
const unsigned char mt2_surface_desc_tail[8] =   /* the 8 bytes after Surface Width/Height */
    { 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26 };
const unsigned char mt2_region_descriptor[15] =
    { 0x02,0x01,0x00,0x14,0x01,0x00,0x1e,0x00,0x02,0x14,0x02,0x01,0x0e,0x02,0x00 };
const unsigned char mt2_region_param[6] =
    { 0x00,0x00,0x05,0x00,0xfe,0x01 };

static void put_u32_le(unsigned char *o, unsigned int v) {
    o[0] = v & 0xff; o[1] = (v >> 8) & 0xff;
    o[2] = (v >> 16) & 0xff; o[3] = (v >> 24) & 0xff;
}

int mt2_fill_geometry_report(unsigned char reportId, unsigned char *out, unsigned int *outLen) {
    unsigned int i;
    switch (reportId) {
    case 0x7f:                                   /* rCRITICAL_ERRORS: none */
        out[0] = out[1] = out[2] = out[3] = 0; *outLen = 4; return MT2_GEO_OK;
    case 0xd1:                                   /* Family ID */
        out[0] = MT2_FAMILY_ID; *outLen = 1; return MT2_GEO_OK;
    case 0xd3:                                   /* Endianness, Rows, Cols, bcdVersion(BE) */
        out[0] = 0x01; out[1] = MT2_SENSOR_ROWS; out[2] = MT2_SENSOR_COLS;
        out[3] = 0x01; out[4] = 0x00; *outLen = 5; return MT2_GEO_OK;
    case 0xd9:                                   /* Surface Width/Height (u32 LE) + Surface Descriptor */
        put_u32_le(out,     MT2_SURFACE_WIDTH);
        put_u32_le(out + 4, MT2_SURFACE_HEIGHT);
        for (i = 0; i < 8; i++) out[8 + i] = mt2_surface_desc_tail[i];
        *outLen = 16; return MT2_GEO_OK;
    case 0xd0:                                   /* Sensor Region Descriptor (genuine raw bytes) */
        for (i = 0; i < sizeof(mt2_region_descriptor); i++) out[i] = mt2_region_descriptor[i];
        *outLen = (unsigned int)sizeof(mt2_region_descriptor); return MT2_GEO_OK;
    case 0xa1:                                   /* Sensor Region Param (genuine raw bytes) */
        for (i = 0; i < sizeof(mt2_region_param); i++) out[i] = mt2_region_param[i];
        *outLen = (unsigned int)sizeof(mt2_region_param); return MT2_GEO_OK;
    case 0xdb:                                   /* Multitouch ID: let the driver skip */
        return MT2_GEO_UNSUPPORTED;
    default:
        return MT2_GEO_NOT_GEOMETRY;
    }
}
