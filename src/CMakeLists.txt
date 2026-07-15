# mt2core: the shared C logic used by the userland tools + the ctest unit tests.
# (The kext compiles its OWN copies of these sources with -fapple-kext in Task 6;
#  mt2_log / gh_default_adapter are kext-only C++ and live in kext-gesture/, not here.)
add_library(mt2core STATIC
  mt2_decode.c mt2_usb_decode.c mt2_bt_decode.c mt1_encode.c
  mt2_pipeline.c mt2_lifecycle.c mt2_session.c mt2_geometry.c
  mt2_usb_bytes.c mt2_connect_sm.c mt2_presence.c conn_trace.c genuine_host.c
  mt2_coordinator.c vhid_mt1.c mt2_battery.c mt2_single_load.c mt2_splice.c
  mt2_synth_report.c mt2_synth_teardown.c)
# kext-gesture/ is PUBLIC because mt2_splice.h includes vtable_clone.h from there;
# consumers (e.g. test_splice) that pull in mt2_splice.h inherit the path.
target_include_directories(mt2core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/kext-gesture)
