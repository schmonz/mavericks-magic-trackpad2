# MT2Gesture.kext — built with the kext toolchain (-fapple-kext), then linked + bundled.
# Mirrors kext-gesture/Makefile exactly (see OBJS there). Two compilations of the shared
# src/*.c exist in this project: mt2core (userland, -std=c99) and HERE (kext, -x c++).

# Kext-local C++ sources (incl. mt2_log + mt2_diag + gh_default_adapter, which are kext-only).
set(KEXT_LOCAL_CPP
  MT2Gesture.cpp MT2GestureUserClient.cpp MT2BTReader.cpp MT2USBReader.cpp
  mt2_log.cpp mt2_diag.cpp gh_default_adapter.cpp)

# Shared core from ../src, compiled UNDER the kext toolchain as C++ (-x c++), like the Makefile.
set(KEXT_SHARED_NAMES
  mt2_bt_decode mt1_encode mt2_usb_decode mt2_decode mt2_pipeline mt2_lifecycle
  mt2_session mt2_geometry mt2_usb_reframe mt2_connect_sm conn_trace
  genuine_host mt2_coordinator mt2_battery)
set(KEXT_SHARED_SRCS "")
foreach(c ${KEXT_SHARED_NAMES})
  list(APPEND KEXT_SHARED_SRCS ${CMAKE_SOURCE_DIR}/src/${c}.c)
endforeach()

# On a modern host the kext must see the 10.9 kernel headers + libkmod, NOT the
# live system / modern SDK (whose Kernel.framework describes a different kernel
# ABI -- wrong vtable slots + struct sizes for the RE'd 10.9.5 hijack). When
# MT2_MACOSX_10_9_SDK is set, isysroot to it and take Kernel/IOKit headers + libs
# from there. UNSET (native 10.9 dev box) -> current CMAKE_OSX_SYSROOT behavior.
if(MT2_MACOSX_10_9_SDK)
  set(MT2_KEXT_SYSROOT_FLAGS -isysroot ${MT2_MACOSX_10_9_SDK})
  set(MT2_KEXT_KHDRS
    ${MT2_MACOSX_10_9_SDK}/System/Library/Frameworks/Kernel.framework/Headers
    ${MT2_MACOSX_10_9_SDK}/System/Library/Frameworks/IOKit.framework/Headers)
  set(MT2_KEXT_LIBDIR -L${MT2_MACOSX_10_9_SDK}/usr/lib)
else()
  set(MT2_KEXT_SYSROOT_FLAGS)
  set(MT2_KEXT_KHDRS
    ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/Kernel.framework/Headers
    ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/IOKit.framework/Headers)
  set(MT2_KEXT_LIBDIR)
endif()

# The kext as a NORMAL target: CMake compiles AND links it, tracking every source -> object -> link
# dependency automatically. That is the whole point here -- the old hand-rolled add_custom_command link
# did NOT auto-track $<TARGET_OBJECTS>, so a recompiled object silently skipped the relink and we loaded a
# stale kext (the "rm the binary to force relink" footgun). A real target has no such gap. The kext "link"
# is just the c++ driver with freestanding-kernel flags, expressed via target_link_options.
add_executable(mt2gesture_kext ${KEXT_LOCAL_CPP} ${KEXT_SHARED_SRCS})
set_target_properties(mt2gesture_kext PROPERTIES
  OUTPUT_NAME MT2Gesture
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(mt2gesture_kext PRIVATE
  ${MT2_KEXT_SYSROOT_FLAGS}
  -mmacosx-version-min=10.9 -fapple-kext -fno-rtti -fno-exceptions
  -std=c++11 -Wno-deprecated-register)
target_include_directories(mt2gesture_kext PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
  ${MT2_KEXT_KHDRS})
# Freestanding-kernel link: no CRT/stdlib, emit a KEXTBUNDLE Mach-O, link libkmod(c++). Same flags the
# old custom_command used; now CMake owns the link (and thus the object dependencies).
target_link_options(mt2gesture_kext PRIVATE
  -arch x86_64 -mmacosx-version-min=10.9 ${MT2_KEXT_SYSROOT_FLAGS}
  -nostdlib -Wl,-kext -Wl,-no_uuid ${MT2_KEXT_LIBDIR} -lkmodc++ -lkmod)

# Compile the shared .c as C++ (kext mangling must match the .cpp callers; some shared
# headers lack extern "C", so plain-C compilation would mismatch and fail to link).
set_source_files_properties(${KEXT_SHARED_SRCS} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;c++")

# KERNEL=1: MT2USBReader needs the kernel-only USB control API; conn_trace needs
# libkern's snprintf (the kernel has no <stdio.h>). Both gate behavior on #if KERNEL.
set_source_files_properties(
  MT2USBReader.cpp ${CMAKE_SOURCE_DIR}/src/conn_trace.c
  PROPERTIES COMPILE_DEFINITIONS KERNEL=1)

# Assemble the .kext bundle as a TRACKED output (not a fire-once POST_BUILD): it DEPENDS on the target's
# binary file, so it regenerates whenever the binary relinks OR the bundle goes missing. CMake relinks the
# binary whenever any source/object changed (the dependency it now tracks for us), so the bundle stays in
# lockstep -- no stale bundle, no "deleted it and nothing rebuilt it" gap.
set(KEXT_BUNDLE ${CMAKE_BINARY_DIR}/MT2Gesture.kext)
add_custom_command(OUTPUT ${KEXT_BUNDLE}/Contents/MacOS/MT2Gesture
  DEPENDS mt2gesture_kext $<TARGET_FILE:mt2gesture_kext> ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
  COMMAND ${CMAKE_COMMAND} -E make_directory ${KEXT_BUNDLE}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mt2gesture_kext> ${KEXT_BUNDLE}/Contents/MacOS/MT2Gesture
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist ${KEXT_BUNDLE}/Contents/Info.plist
  COMMENT "Assembling MT2Gesture.kext")

# `kext` stays the public target name (reload / kext-load / pkg / characterize all depend on it).
add_custom_target(kext ALL DEPENDS ${KEXT_BUNDLE}/Contents/MacOS/MT2Gesture)

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
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_kext.sh load ${KEXT_BUNDLE}
  COMMENT "Staging (root:wheel /tmp) + loading MT2Gesture.kext")
add_dependencies(kext-load kext)

add_custom_target(kext-unload
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_kext.sh unload
  COMMENT "Unloading MT2Gesture.kext")
