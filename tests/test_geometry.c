#include "test.h"
#include "mt2_geometry.h"
#include <string.h>

static void run_tests(void) {
    unsigned char buf[64];
    unsigned int n;

    /* 0xD1 Family ID = 0x80 */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD1, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 1); CHECK_EQ(buf[0], 0x80);

    /* 0xD3 Endianness,Rows,Cols,bcdVersion */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD3, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 5);
    CHECK_EQ(buf[0],0x01); CHECK_EQ(buf[1],0x0d); CHECK_EQ(buf[2],0x10);
    CHECK_EQ(buf[3],0x01); CHECK_EQ(buf[4],0x00);

    /* 0xD9 Surface Width 13000 / Height 11300, u32 LE each */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD9, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 8);
    CHECK_EQ((unsigned)(buf[0]|(buf[1]<<8)|(buf[2]<<16)|((unsigned)buf[3]<<24)), 13000u);
    CHECK_EQ((unsigned)(buf[4]|(buf[5]<<8)|(buf[6]<<16)|((unsigned)buf[7]<<24)), 11300u);

    /* 0xD0 / 0xA1 Sensor Region = 16 zero bytes */
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xD0, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 16); CHECK_EQ(buf[0],0); CHECK_EQ(buf[15],0);
    memset(buf, 0xAA, sizeof buf); n = 999;
    CHECK_EQ(mt2_fill_geometry_report(0xA1, buf, &n), MT2_GEO_OK);
    CHECK_EQ(n, 16); CHECK_EQ(buf[7],0);

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
