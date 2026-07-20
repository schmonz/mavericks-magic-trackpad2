/* extern "C" shim for test_reader_characterization: run one MavericksTouchFrame through the VoodooInput wire
 * round-trip in place, mirroring what the BT reader (now a satellite) emits to the mux. Isolated in
 * a .cpp so the C++ wire headers (VoodooInputEvent) stay out of the pure-C characterization test. */
#include "../src/mavericks_voodoo_translate.h"
#include "../src/mt2_coord_range.h"

extern "C" void mt2_bt_wire_roundtrip(MavericksTouchFrame *f) {
    VoodooInputEvent w = mavericks_voodoo_from_frame(f, MT2_SPAN_X, MT2_SPAN_Y);
    *f = mavericks_frame_from_voodoo(&w, MT2_SPAN_X, MT2_SPAN_Y);
}
