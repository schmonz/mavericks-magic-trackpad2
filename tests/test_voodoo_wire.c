#include "voodoo_wire.h"
#include "test.h"

static void run_tests(void) {
    /* The three ABI facts our translation seam depends on. */
    CHECK_EQ(kIOMessageVoodooInputMessage, 12345);
    CHECK_EQ(VOODOO_INPUT_MAX_TRANSDUCERS, 10);

    /* The struct shape the satellite hands us (compile-time presence of each field). */
    VoodooInputEvent e; e.contact_count = 0; e.timestamp = 0;
    VoodooInputTransducer *t = &e.transducers[0];
    t->secondaryId = 1; t->isPhysicalButtonDown = true; t->isTransducerActive = true;
    t->currentCoordinates.x = 0; t->currentCoordinates.y = 0;
    t->currentCoordinates.pressure = 0; t->currentCoordinates.width = 0;
    CHECK(sizeof(e.transducers) / sizeof(e.transducers[0]) == 10);
}
TEST_MAIN()
