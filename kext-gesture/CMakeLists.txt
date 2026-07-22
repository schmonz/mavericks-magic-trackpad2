# VoodooInputMavericks.kext — built with the kext toolchain (-fapple-kext), then linked + bundled.
# Mirrors kext-gesture/Makefile exactly (see OBJS there). Two compilations of the shared
# src/*.c exist in this project: mt2core (userland, -std=c99) and HERE (kext, -x c++).

# Kext-local C++ sources (incl. mavericks_log + mavericks_diag, which are kext-only).
set(KEXT_LOCAL_CPP
  MavericksVoodooInputHost.cpp MavericksVoodooInputHostUserClient.cpp MT2BTReader.cpp MT2USBReader.cpp
  mavericks_log.cpp mavericks_diag.cpp mavericks_splice_kext.cpp
  MavericksVoodooInput.cpp MavericksTerminalBackend.cpp MavericksHIDShell.cpp MavericksAMDTerminal.cpp mavericks_amd_probe.cpp)

# Shared core from ../src, compiled UNDER the kext toolchain as C++ (-x c++), like the Makefile.
set(KEXT_SHARED_NAMES
  mt2_bt_decode mavericks_amd_terminal_encode mt2_usb_decode mt2_decode mavericks_pipeline mavericks_lifecycle
  mavericks_session mt2_geometry mt2_usb_bytes mavericks_connect_sm mavericks_conn_trace
  mavericks_genuine_host mavericks_coordinator mt2_battery mavericks_splice mavericks_synth_report mavericks_synth_teardown)
set(KEXT_SHARED_SRCS "")
foreach(c ${KEXT_SHARED_NAMES})
  list(APPEND KEXT_SHARED_SRCS ${CMAKE_SOURCE_DIR}/src/${c}.c)
endforeach()

# The kext as a NORMAL target: CMake compiles AND links it, tracking every source -> object -> link
# dependency automatically. That is the whole point here -- the old hand-rolled add_custom_command link
# did NOT auto-track $<TARGET_OBJECTS>, so a recompiled object silently skipped the relink and we loaded a
# stale kext (the "rm the binary to force relink" footgun). A real target has no such gap. The kext "link"
# is just the c++ driver with freestanding-kernel flags, expressed via target_link_options.
set(KEXT_LOCAL_CXX_ABS ${CMAKE_SOURCE_DIR}/src/mavericks_voodoo_translate.cpp)
add_executable(mt2gesture_kext ${KEXT_LOCAL_CPP} ${KEXT_SHARED_SRCS} ${KEXT_LOCAL_CXX_ABS})
set_target_properties(mt2gesture_kext PROPERTIES
  OUTPUT_NAME VoodooInputMavericks
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Project-specific include directories (source-local; kernel/IOKit dirs are added by mavericks_kext()).
target_include_directories(mt2gesture_kext PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/VoodooInput/VoodooInputMultitouch)

# Compile the shared .c as C++ (kext mangling must match the .cpp callers; some shared
# headers lack extern "C", so plain-C compilation would mismatch and fail to link).
set_source_files_properties(${KEXT_SHARED_SRCS} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;c++")

# KERNEL=1: MT2USBReader needs the kernel-only USB control API; conn_trace needs
# libkern's snprintf (the kernel has no <stdio.h>). MavericksVoodooInput + the voodoo
# translator need the KERNEL branch of voodoo_wire.h (real kernel OSTypes, not the
# host shim). All gate behavior on #if KERNEL.
set_source_files_properties(
  MT2USBReader.cpp ${CMAKE_SOURCE_DIR}/src/mavericks_conn_trace.c
  MavericksVoodooInput.cpp MavericksTerminalBackend.cpp ${CMAKE_SOURCE_DIR}/src/mavericks_voodoo_translate.cpp
  PROPERTIES COMPILE_DEFINITIONS KERNEL=1)

# Apply reusable kext build machinery: -fapple-kext toolchain flags, freestanding-kernel
# link options, Kernel/IOKit include dirs, Info.plist configure, and .kext bundle assembly.
# The public "kext" target name and ${CMAKE_BINARY_DIR}/VoodooInputMavericks.kext output path are
# established by mavericks_kext() and must not change (reload/pkg/characterize depend on them).
mavericks_kext(
  TARGET        mt2gesture_kext
  BUNDLE        VoodooInputMavericks
  INFO_PLIST_IN ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  PUBLIC_TARGET kext)

# Always characterize the freshly-built kext (sub-second; no need to remember).
# NATIVE 10.9 dev box (MT2_MACOSX_10_9_SDK unset): refresh the committed cross-build
# equivalence reference at dist/characterization/ so it stays current -- it just
# shows up in `git status` to commit before you tag a release. MODERN cross-build:
# the release workflow runs `characterize_build.sh compare` as the publish gate
# (.github/workflows/release.yml) instead of overwriting the native reference.
if(NOT MT2_MACOSX_10_9_SDK)
  add_custom_command(TARGET kext POST_BUILD
    COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/characterize_build.sh emit
            ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/dist/characterization
    COMMENT "Refreshing kext cross-build equivalence reference (dist/characterization)")
endif()

# Operational load/unload (ports kext-gesture/Makefile load/unload). The multi-step
# load (stage root:wheel in /tmp, Apple kexts first, then ours) lives in cmake/dev_kext.sh
# so the `|| true` tolerance + sequencing stay correct (CMake's COMMAND can't express
# `a && b || true` without the sh -c space-escaping bug found in Phase 0).
add_custom_target(kext-load
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_kext.sh load ${CMAKE_BINARY_DIR}/VoodooInputMavericks.kext
  COMMENT "Staging (root:wheel /tmp) + loading VoodooInputMavericks.kext")
add_dependencies(kext-load kext)

add_custom_target(kext-unload
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_kext.sh unload
  COMMENT "Unloading VoodooInputMavericks.kext")
