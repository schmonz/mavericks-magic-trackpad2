#include "mt2_geometry.h"

/* Calibrated to a real Magic Trackpad 2 (see RE findings):
 *   Family ID 0x80(128)  Rows 13  Cols 16  Surface 13000 x 11300 (device units). */
#define MT2_FAMILY_ID      0x80
#define MT2_SENSOR_ROWS    0x0d
#define MT2_SENSOR_COLS    0x10
#define MT2_SURFACE_WIDTH  13000u
#define MT2_SURFACE_HEIGHT 11300u

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
    case 0xd9:                                   /* Surface Width/Height, u32 LE each */
        put_u32_le(out,     MT2_SURFACE_WIDTH);
        put_u32_le(out + 4, MT2_SURFACE_HEIGHT);
        *outLen = 8; return MT2_GEO_OK;
    case 0xd0:                                   /* Sensor Region Descriptor */
    case 0xa1:                                   /* Sensor Region Param */
        for (i = 0; i < 16; i++) out[i] = 0; *outLen = 16; return MT2_GEO_OK;
    case 0xdb:                                   /* Multitouch ID: let the driver skip */
        return MT2_GEO_UNSUPPORTED;
    default:
        return MT2_GEO_NOT_GEOMETRY;
    }
}
