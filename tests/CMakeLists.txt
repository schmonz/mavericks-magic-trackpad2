# Unit tests. Each test compiles its own .c and links mt2core (the static lib carries
# every shared src/*.c; the linker pulls only the members each test references — same
# result as the Makefile's per-test source lists, but DRY). No test #includes a .c.
function(mt2_ctest name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} mt2core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

foreach(t test_model test_decode test_bt_decode test_encode test_pipeline test_lifecycle
          test_session test_session_characterization test_connect_sm test_pane_sm test_conn_trace test_usb_bytes test_splice test_genuine_host test_coordinator
          test_battery test_single_load test_reader_characterization)
  mt2_ctest(${t})
endforeach()

# test_geometry needs the src/ include dir (Makefile: -Isrc).
add_executable(test_geometry test_geometry.c)
target_link_libraries(test_geometry mt2core)
target_include_directories(test_geometry PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_geometry COMMAND test_geometry)

# test_vtable_clone needs the kext-gesture include dir; no src deps (Makefile: -Ikext-gesture).
add_executable(test_vtable_clone test_vtable_clone.c)
target_include_directories(test_vtable_clone PRIVATE ${CMAKE_SOURCE_DIR}/kext-gesture)
add_test(NAME test_vtable_clone COMMAND test_vtable_clone)

# Shell + bats suites (run from the source tree).
add_test(NAME test_mt2d_run COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_mt2d_run.sh)
add_test(NAME test_conn_trace_parser COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_conn_trace_parser.sh)
add_test(NAME test_appcast_notes COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_appcast_notes.sh)

# Build-artifact invariants: fail the build if a shipped artifact violates a 10.9 rule.
# These run against ${CMAKE_BINARY_DIR}, so they assume `cmake --build` has already
# assembled the osax/.bundle/.kext (POST_BUILD of `all` targets) into the build dir.
#   test_compat_guard: userland Mach-Os are x86_64, min-OS 10.9, no post-10.9 imports.
#   test_plists: shipped Info.plists lint clean + updater has SUFeedURL/SUPublicEDKey.
add_test(NAME test_compat_guard COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/compat_guard.sh ${CMAKE_BINARY_DIR})
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
            $<TARGET_FILE:mt2_sign_update> $<TARGET_FILE:mt2_generate_keys>)
endif()
