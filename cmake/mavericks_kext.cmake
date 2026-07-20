# mavericks_kext() — reusable kext build machinery.
#
# Encapsulates the -fapple-kext toolchain flags, freestanding-kernel link options,
# 10.9 SDK resolution, and .kext bundle assembly that every kext in this project
# needs.  The caller (e.g. kext-gesture/CMakeLists.txt) keeps its source list,
# add_executable, per-file COMPILE_DEFINITIONS, and any project-specific
# include directories / POST_BUILD steps.
#
# Usage:
#   mavericks_kext(
#     TARGET        <cmake-target>          # add_executable target name
#     BUNDLE        <bundle-stem>           # e.g. VoodooInputMavericks -> VoodooInputMavericks.kext
#     INFO_PLIST_IN <path/to/Info.plist.in> # configure_file source (@VAR@ tokens)
#     PUBLIC_TARGET <public-target-name>    # the ALL target that drives the build
#   )
#
# After the call the TARGET has the kext compile/link options applied, the
# Info.plist is configured into ${CMAKE_CURRENT_BINARY_DIR}/Info.plist, and the
# .kext bundle is assembled under ${CMAKE_BINARY_DIR}/${K_BUNDLE}.kext.

function(mavericks_kext)
  cmake_parse_arguments(K "" "TARGET;BUNDLE;INFO_PLIST_IN;PUBLIC_TARGET" "" ${ARGN})

  # Resolve the 10.9 SDK root for the kext.  On a modern host MT2_MACOSX_10_9_SDK
  # is set to a fetched SDK; on the native 10.9 dev box it is empty and we fall
  # back to CMAKE_OSX_SYSROOT (the live system root, which already carries the
  # right kernel headers).
  if(MT2_MACOSX_10_9_SDK)
    set(sysroot_flags -isysroot ${MT2_MACOSX_10_9_SDK})
    set(khdrs
      ${MT2_MACOSX_10_9_SDK}/System/Library/Frameworks/Kernel.framework/Headers
      ${MT2_MACOSX_10_9_SDK}/System/Library/Frameworks/IOKit.framework/Headers)
    set(libdir -L${MT2_MACOSX_10_9_SDK}/usr/lib)
  else()
    set(sysroot_flags)
    set(khdrs
      ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/Kernel.framework/Headers
      ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/IOKit.framework/Headers)
    set(libdir)
  endif()

  # -fapple-kext toolchain: no RTTI/exceptions, C++11, freestanding kernel ABI.
  target_compile_options(${K_TARGET} PRIVATE
    ${sysroot_flags}
    -mmacosx-version-min=10.9 -fapple-kext -fno-rtti -fno-exceptions
    -std=c++11 -Wno-deprecated-register)

  # Kernel.framework + IOKit headers from the resolved SDK root.
  target_include_directories(${K_TARGET} PRIVATE ${khdrs})

  # Freestanding-kernel link: no CRT/stdlib, emit a KEXTBUNDLE Mach-O, link
  # libkmod(c++).  CMake owns the link (and thus the object dependencies), so
  # a recompiled object always triggers a relink — no stale-kext footgun.
  target_link_options(${K_TARGET} PRIVATE
    -arch x86_64 -mmacosx-version-min=10.9 ${sysroot_flags}
    -nostdlib -Wl,-kext -Wl,-no_uuid ${libdir} -lkmodc++ -lkmod)

  # Stamp version tokens (@MAVERICKS_VERSION@, @MAVERICKS_VERSION_NUMERIC@) into Info.plist.
  # @ONLY so only @VAR@ tokens are touched, not ${VAR} CMake variables.
  configure_file(${K_INFO_PLIST_IN} ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

  # Assemble the .kext bundle as a TRACKED output: it depends on the target's
  # binary file so it regenerates whenever the binary relinks OR the bundle goes
  # missing.
  set(kext_bundle ${CMAKE_BINARY_DIR}/${K_BUNDLE}.kext)
  add_custom_command(OUTPUT ${kext_bundle}/Contents/MacOS/${K_BUNDLE}
    DEPENDS ${K_TARGET} $<TARGET_FILE:${K_TARGET}> ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
    COMMAND ${CMAKE_COMMAND} -E make_directory ${kext_bundle}/Contents/MacOS
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${K_TARGET}> ${kext_bundle}/Contents/MacOS/${K_BUNDLE}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/Info.plist ${kext_bundle}/Contents/Info.plist
    COMMENT "Assembling ${K_BUNDLE}.kext")

  # The PUBLIC_TARGET (e.g. "kext") is what reload / pkg / characterize depend on.
  add_custom_target(${K_PUBLIC_TARGET} ALL
    DEPENDS ${kext_bundle}/Contents/MacOS/${K_BUNDLE})
endfunction()
