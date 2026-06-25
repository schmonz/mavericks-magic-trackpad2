#include "test.h"
#include "mt2_geometry.h"
#include <string.h>

static void run_tests(void) {
    unsigned char buf[64];
    unsigned int n;

    /* Genuine MT2 geometry (ground-truthed 2026-06-25 via ioreg; docs/mt-stack/reference.md). The
     * earlier seeds (Family 0x80, Rows 13/Cols 16, zeroed region, no Surface Descriptor) were a
     * half-resolution under-model -> the edge dead zones. */

    /* 0xD1 Family ID = 129 (0x81) */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD1, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 1); CHECK_EQ(buf[0], 0x81);

    /* 0xD3 Endianness, Rows 22 (0x16), Cols 30 (0x1e), bcdVersion */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD3, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 5);
    CHECK_EQ(buf[0],0x01); CHECK_EQ(buf[1],0x16); CHECK_EQ(buf[2],0x1e);
    CHECK_EQ(buf[3],0x01); CHECK_EQ(buf[4],0x00);

    /* 0xD9 Surface Width 15600 / Height 11040 (u32 LE) + the genuine 8-byte Surface Descriptor tail
     * (total len 16; the recognizer reads the Surface Descriptor only when len >= 16). */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD9, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 16);
    CHECK_EQ((unsigned)(buf[0]|(buf[1]<<8)|(buf[2]<<16)|((unsigned)buf[3]<<24)), 15600u);
    CHECK_EQ((unsigned)(buf[4]|(buf[5]<<8)|(buf[6]<<16)|((unsigned)buf[7]<<24)), 11040u);
    CHECK_EQ(buf[8], 0x44); CHECK_EQ(buf[15], 0x26);   /* Surface Descriptor tail */

    /* 0xD0 Sensor Region Descriptor = genuine 15 bytes (0x1e=30=Cols at [6]) */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD0, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 15); CHECK_EQ(buf[0],0x02); CHECK_EQ(buf[6],0x1e); CHECK_EQ(buf[14],0x00);
    /* 0xA1 Sensor Region Param = genuine 6 bytes */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xA1, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 6); CHECK_EQ(buf[2],0x05); CHECK_EQ(buf[5],0x01);

    /* 0x7F critical errors = 4 zero bytes */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0x7F, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 4); CHECK_EQ(buf[0],0); CHECK_EQ(buf[3],0);

    /* 0xDB Multitouch ID -> unsupported (driver should skip) */
    CHECK_EQ(mt2_fill_geometry_report(0xDB, buf, &n), MT2_GEO_UNSUPPORTED);

    /* anything else -> not geometry (caller handles) */
    CHECK_EQ(mt2_fill_geometry_report(0x00, buf, &n), MT2_GEO_NOT_GEOMETRY);
    CHECK_EQ(mt2_fill_geometry_report(0xC8, buf, &n), MT2_GEO_NOT_GEOMETRY);
}
TEST_MAIN()
