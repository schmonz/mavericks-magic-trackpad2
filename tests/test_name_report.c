/* Regression lock for the 0x55 name Feature-report builder. The bug (2026-07-29): getReport emitted only
 * the 1+name-length prefix and left the caller's report buffer tail uninitialized, so a short name like
 * "Bo" read back as "Bo" + heap garbage. mt2_build_name_report must zero the WHOLE report and report the
 * full width so the caller writes all of it. */
#include "mt2_name_report.h"
#include <string.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else { printf("PASS: %s\n", msg); } } while (0)

int main(void) {
    /* 1. Short name over a POISONED buffer: the tail past the name must be zeroed (the leak fix). */
    {
        unsigned char out[65];
        memset(out, 0xAA, sizeof out);                 /* simulate the caller's uninitialized buffer */
        const unsigned char name[] = { 'B', 'o' };
        size_t w = mt2_build_name_report(0x55, name, 2, out, sizeof out);
        CHECK(w == sizeof out, "returns FULL report width, not 1+name-length");
        CHECK(out[0] == 0x55, "byte 0 is the report id");
        CHECK(out[1] == 'B' && out[2] == 'o', "name copied right after the id");
        int tail_clean = 1;
        for (size_t i = 3; i < sizeof out; i++) if (out[i] != 0) tail_clean = 0;
        CHECK(tail_clean, "tail past the name is ZERO (no leaked buffer bytes)");
    }
    /* 2. Over-long name clamps to fit after the id and still fully fills the report. */
    {
        unsigned char out[65];
        unsigned char name[200];
        memset(name, 'X', sizeof name);
        size_t w = mt2_build_name_report(0x55, name, sizeof name, out, sizeof out);
        CHECK(w == sizeof out, "over-long: still returns full width");
        CHECK(out[0] == 0x55, "over-long: id preserved");
        int all_x = 1;
        for (size_t i = 1; i < sizeof out; i++) if (out[i] != 'X') all_x = 0;
        CHECK(all_x, "over-long: name clamped to fill the whole data region");
    }
    /* 3. NULL / empty name: id followed by all zeros. */
    {
        unsigned char out[65];
        memset(out, 0xAA, sizeof out);
        size_t w = mt2_build_name_report(0x55, NULL, 0, out, sizeof out);
        CHECK(w == sizeof out, "null name: full width");
        CHECK(out[0] == 0x55, "null name: id present");
        int rest_zero = 1;
        for (size_t i = 1; i < sizeof out; i++) if (out[i] != 0) rest_zero = 0;
        CHECK(rest_zero, "null name: everything after the id is zero");
    }
    /* 4. Degenerate: zero-capacity buffer must be refused, not written. */
    {
        unsigned char out[1] = { 0x11 };
        size_t w = mt2_build_name_report(0x55, (const unsigned char *)"x", 1, out, 0);
        CHECK(w == 0, "out_cap==0 returns 0 (no write)");
        CHECK(out[0] == 0x11, "out_cap==0 leaves the buffer untouched");
    }

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
