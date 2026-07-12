#include "../src/mt2_pipeline.h"
#include "test.h"

static void run_tests(void) {
    CHECK_EQ(mt2_settle_passed(0, 2500), 0);
    CHECK_EQ(mt2_settle_passed(2499, 2500), 0);
    CHECK_EQ(mt2_settle_passed(2500, 2500), 1);
    CHECK_EQ(mt2_settle_passed(9999, 2500), 1);

    mt2_frame f = {0};
    f.contact_count = 3;
    f.transducers[0].currentCoordinates.pressure = 25; f.transducers[0].currentCoordinates.x = 10;
    f.transducers[1].currentCoordinates.pressure = 0;  f.transducers[1].currentCoordinates.x = 20;
    f.transducers[2].currentCoordinates.pressure = 12; f.transducers[2].currentCoordinates.x = 30;
    mt2_drop_lifted(&f);
    CHECK_EQ(f.contact_count, 2);
    CHECK_EQ(f.transducers[0].currentCoordinates.x, 10);
    CHECK_EQ(f.transducers[1].currentCoordinates.x, 30);
    mt2_frame up = {0}; up.contact_count = 1; up.transducers[0].currentCoordinates.pressure = 0;
    mt2_drop_lifted(&up);
    CHECK_EQ(up.contact_count, 0);

    unsigned last = 0, mask = 0xdead;
    CHECK_EQ(mt2_button_edge(0, 0, &last, &mask), 0);
    CHECK_EQ(mt2_button_edge(1, 1, &last, &mask), 1); CHECK_EQ(mask, 0x1u);
    CHECK_EQ(mt2_button_edge(1, 1, &last, &mask), 0);
    CHECK_EQ(mt2_button_edge(0, 0, &last, &mask), 1); CHECK_EQ(mask, 0x0u);
    last = 0;
    CHECK_EQ(mt2_button_edge(1, 2, &last, &mask), 1); CHECK_EQ(mask, 0x2u);
    last = 0;  /* fallback: any finger count other than 2 -> primary 0x1 */
    CHECK_EQ(mt2_button_edge(1, 3, &last, &mask), 1); CHECK_EQ(mask, 0x1u);
    last = 0;
    CHECK_EQ(mt2_button_edge(1, 0, &last, &mask), 1); CHECK_EQ(mask, 0x1u);

    mt2_decel_t d;
    mt2_frame held = {0}; held.contact_count = 1; held.transducers[0].currentCoordinates.pressure = 20; held.transducers[0].currentCoordinates.x = 77;
    mt2_decel_arm(&d, &held);
    CHECK_EQ(d.step, 0);
    mt2_frame out; int has; uint32_t rearm;
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 1); CHECK_EQ(out.transducers[0].currentCoordinates.x, 77); CHECK_EQ(rearm, MT2_DECEL_MS);
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 1); CHECK_EQ(rearm, MT2_DECEL_MS);
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 1); CHECK_EQ(out.contact_count, 0); CHECK_EQ(rearm, 0u);
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 0); CHECK_EQ(rearm, 0u);
}
TEST_MAIN()
