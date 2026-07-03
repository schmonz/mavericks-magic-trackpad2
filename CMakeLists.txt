cmake_minimum_required(VERSION 3.10)

# Single source of truth for the product version. May carry a pre-release suffix
# (e.g. -pre.1 / -rc.1) that CMake's project(VERSION) can't hold, so keep the full
# string here and feed project() only the numeric part. The .pkg name + version and
# CI all derive from MT2_VERSION -- bump it here, tag v<MT2_VERSION> to match.
set(MT2_VERSION "0.1.0")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" MT2_VERSION_NUMERIC "${MT2_VERSION}")
project(mt2 VERSION ${MT2_VERSION_NUMERIC} LANGUAGES C CXX)

# Native 10.9 build for now; Project B adds the modern-SDK cross-build on top.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "" FORCE)
endif()

# 10.9 is Intel-only; force the single x86_64 slice unless the caller overrides.
# Without this, a modern arm64 host silently bumps the deployment target to 11.0
# and emits an arm64 binary that cannot run on Mavericks. On the native 10.9 dev
# box x86_64 is already the native arch, so this is a no-op there.
if(NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "" FORCE)
endif()

# Absolute path to a MacOSX10.9 SDK root, used ONLY to cross-build the kext from a
# modern host (the kext needs 10.9 kernel headers + libkmod, which the modern SDK
# does not provide). Fetch it with cmake/fetch_10_9_sdk.sh. Leave UNSET on the
# native 10.9 dev box -- the kext then falls back to CMAKE_OSX_SYSROOT (unchanged).
set(MT2_MACOSX_10_9_SDK "" CACHE PATH "MacOSX10.9 SDK root for kext cross-build (modern host only)")
# Path to a fetched Sparkle 1.27.3 Sparkle.framework (x86_64, 10.9) for the updater helper.
# Empty by default; set by the caller/CI (cmake -DMT2_SPARKLE_FRAMEWORK=$(sh cmake/fetch_sparkle.sh)).
set(MT2_SPARKLE_FRAMEWORK "" CACHE PATH "Fetched Sparkle.framework for MavericksTrackpad2Updater.app")
# Path to a fetched orlp/ed25519 src/ dir (the SAME lib Sparkle vendors) for the native EdDSA signer.
# Empty by default; set by the caller/CI (cmake -DMT2_ED25519_SRC=$(sh cmake/fetch_ed25519.sh)).
set(MT2_ED25519_SRC "" CACHE PATH "Fetched orlp/ed25519 src dir for the native mt2_sign_update signer")
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/sbin")

add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

enable_testing()

add_subdirectory(src)
add_subdirectory(tools)
add_subdirectory(kext-gesture)
add_subdirectory(tests)

include(cmake/mt2_pkg.cmake)
include(cmake/mt2_dev.cmake)
