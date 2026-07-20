# Unit tests. Each test compiles its own .c and links mt2core (the static lib carries
# every shared src/*.c; the linker pulls only the members each test references — same
# result as the Makefile's per-test source lists, but DRY). No test #includes a .c.
function(mt2_ctest name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} mt2core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

foreach(t test_model test_decode test_bt_decode test_encode test_pipeline test_lifecycle
          test_session test_session_characterization test_connect_sm test_presence test_conn_trace test_usb_bytes test_splice test_genuine_host test_coordinator
          test_battery test_single_load test_synth_report test_synth_teardown)
  mt2_ctest(${t})
endforeach()

# test_reader_characterization also exercises the BT satellite wire round-trip. The test stays pure C
# (links mt2core cleanly); the round-trip is behind the extern "C" shim char_bt_wire.cpp, which is the
# only unit that sees the C++ wire headers (+ the translate source it calls).
add_executable(test_reader_characterization test_reader_characterization.c
  char_bt_wire.cpp
  ${CMAKE_SOURCE_DIR}/src/mavericks_voodoo_translate.cpp)
target_link_libraries(test_reader_characterization mt2core)
target_include_directories(test_reader_characterization PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/VoodooInput/VoodooInputMultitouch)
add_test(NAME test_reader_characterization COMMAND test_reader_characterization)

# test_geometry needs the src/ include dir (Makefile: -Isrc).
add_executable(test_geometry test_geometry.c)
target_link_libraries(test_geometry mt2core)
target_include_directories(test_geometry PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_geometry COMMAND test_geometry)

# test_vtable_clone needs the kext-gesture include dir; no src deps (Makefile: -Ikext-gesture).
add_executable(test_vtable_clone test_vtable_clone.c)
target_include_directories(test_vtable_clone PRIVATE ${CMAKE_SOURCE_DIR}/kext-gesture)
add_test(NAME test_vtable_clone COMMAND test_vtable_clone)

# test_cod_match needs the tools/ include dir for the shared CoD predicate; no other deps.
add_executable(test_cod_match test_cod_match.c)
target_include_directories(test_cod_match PRIVATE ${CMAKE_SOURCE_DIR}/tools)
add_test(NAME test_cod_match COMMAND test_cod_match)

# test_reconnect_policy: the connection-keeper's page decision (idempotency invariant).
add_executable(test_reconnect_policy test_reconnect_policy.c)
target_include_directories(test_reconnect_policy PRIVATE ${CMAKE_SOURCE_DIR}/tools)
add_test(NAME test_reconnect_policy COMMAND test_reconnect_policy)

# Vendored VoodooInput wire ABI (no mt2core; standalone header assertions).
# The upstream headers are C++ (tag-less struct/enum); compile as C++ without
# editing the pristine vendored files.
set_source_files_properties(test_voodoo_wire.c PROPERTIES LANGUAGE CXX)
add_executable(test_voodoo_wire test_voodoo_wire.c)
target_include_directories(test_voodoo_wire PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/VoodooInput/VoodooInputMultitouch)
add_test(NAME test_voodoo_wire COMMAND test_voodoo_wire)

# VoodooInput translation seam. Compiled as C++ (wire headers are C++-only); no mt2core deps.
add_executable(test_voodoo_translate test_voodoo_translate.c
  ${CMAKE_SOURCE_DIR}/src/mavericks_voodoo_translate.cpp)
set_source_files_properties(test_voodoo_translate.c PROPERTIES LANGUAGE CXX)
target_include_directories(test_voodoo_translate PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/VoodooInput/VoodooInputMultitouch)
add_test(NAME test_voodoo_translate COMMAND test_voodoo_translate)

# VoodooInput integration proof: standard-fields event -> engine -> MakeTouch-first MT1.
# C++ (wire headers) linking C mt2core (headers wrapped extern "C" in the test).
add_executable(test_voodoo_integration test_voodoo_integration.c
  ${CMAKE_SOURCE_DIR}/src/mavericks_voodoo_translate.cpp)
set_source_files_properties(test_voodoo_integration.c PROPERTIES LANGUAGE CXX)
target_include_directories(test_voodoo_integration PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/VoodooInput/VoodooInputMultitouch)
target_link_libraries(test_voodoo_integration mt2core)
add_test(NAME test_voodoo_integration COMMAND test_voodoo_integration)

# Shell + bats suites (run from the source tree).
add_test(NAME test_mt2d_run COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_mt2d_run.sh)
add_test(NAME test_conn_trace_parser COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_conn_trace_parser.sh)
add_test(NAME test_appcast_notes COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_appcast_notes.sh)

# Build-artifact invariant: shipped Info.plists lint clean + updater has SUFeedURL/SUPublicEDKey.
# (The 10.9-safety compat guard is now a per-target POST_BUILD via mavericks_compat_guard() in
# tools/CMakeLists.txt -- it fails `cmake --build` directly, so it is no longer a ctest here.)
add_test(NAME test_plists COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/check_plists.sh ${CMAKE_BINARY_DIR})

# Branch A watcher inject-decision (first ObjC ctest). Links the watcher source for
# mt2_should_inject; -DMT2_PANE_WATCH_TEST excludes the watcher's main (no duplicate main).
add_executable(test_pane_watch test_pane_watch.m
  ${CMAKE_SOURCE_DIR}/tools/mt2_prefpane_refresh/mt2_pane_watch.m)
target_compile_options(test_pane_watch PRIVATE -fobjc-arc -DMT2_PANE_WATCH_TEST)
target_include_directories(test_pane_watch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pane_watch
  "-framework Foundation" "-framework AppKit" "-framework ApplicationServices")
add_test(NAME test_pane_watch COMMAND test_pane_watch)

find_program(BATS_EXE bats)
if(BATS_EXE)
  add_test(NAME test_re_bats COMMAND ${BATS_EXE} ${CMAKE_SOURCE_DIR}/tests/test_re.bats)
else()
  add_test(NAME test_re_bats COMMAND ${CMAKE_COMMAND} -E echo "bats not installed")
  set_tests_properties(test_re_bats PROPERTIES SKIP_RETURN_CODE 0)
endif()

# Native EdDSA signer round-trip -- only when the signer is built (gated on MT2_ED25519_SRC, like the
# tool targets in tools/CMakeLists.txt). Generator expressions resolve the built binary paths.
if(MT2_ED25519_SRC)
  add_test(NAME test_signer_roundtrip
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_signer_roundtrip.sh
            $<TARGET_FILE:mavericks_sign_update> $<TARGET_FILE:mavericks_generate_keys>)
endif()
