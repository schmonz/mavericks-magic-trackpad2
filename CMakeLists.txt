cmake_minimum_required(VERSION 3.13)  # kext link uses target_link_options (added in 3.13)

# 10.9 target + x86_64 arch MUST be set BEFORE project() to retarget the whole
# build (userland + kext + updater sub-builds). Without the arch pin, a modern
# arm64 CI host silently builds arm64 binaries that can't run on Mavericks and
# can't link the x86_64-thinned Sparkle -- caught by CI, since a native x86_64
# 10.9 box masks it. include(Mavericks) below only backstops these as defaults
# (its if(NOT ...) guards no-op here); the authoritative set is here, pre-project,
# mirroring mavericks-1password.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Target architectures")

# Single source of truth for the product version. May carry a pre-release suffix
# (e.g. -pre.1 / -rc.1) that CMake's project(VERSION) can't hold, so keep the full
# string here and feed project() only the numeric part. The .pkg name + version and
# CI all derive from MT2_VERSION -- bump it here, tag v<MT2_VERSION> to match.
set(MT2_VERSION "0.4.5")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" MT2_VERSION_NUMERIC "${MT2_VERSION}")
project(mt2 VERSION ${MT2_VERSION_NUMERIC} LANGUAGES C CXX)

# Shared mavericks-* build conventions: deployment target 10.9 / x86_64 (10.9 is
# Intel-only; without the arch pin a modern arm64 host silently bumps the target
# to 11.0 and emits a binary that can't run on Mavericks), the AppleClang gate
# (reject gcc / Homebrew / pkgsrc clang picked up by an IDE — both build hosts use
# Apple's clang, differing only in Xcode version), native-vs-cross mode detection,
# and the 10.9 compat helpers. Install the mavericks-shared-cmake package once
# (`cmake --install`, which self-registers in the user package registry); then it
# is found with no CMAKE_PREFIX_PATH. See github.com/schmonz/mavericks-shared-cmake.
# Must come after project() so CMAKE_<LANG>_COMPILER_ID is populated for the gate.
find_package(MavericksSharedCMake REQUIRED)
set(MAVERICKS_REQUIRE_LANGS C CXX)   # the kext (C++/IOKit) + userland (C) both must be AppleClang
set(MAVERICKS_REQUIRE_BLOCKS ON)     # the injected osax uses Apple blocks (dispatch handlers)
include(Mavericks)

# Post-10.9 IOKit renames that must never leak on a modern cross-build:
# _IORegistryEntryCopyPath (10.11), _IOMainPort / _kIOMainPortDefault (12.0, the renamed
# IOMasterPort). mavericks_compat_guard() forwards this to the guard for every shipped
# userland target (see tools/CMakeLists.txt). Must be set before add_subdirectory(tools).
set(MAVERICKS_POST_10_9_SYMBOLS "_IORegistryEntryCopyPath|_IOMainPort|_kIOMainPortDefault")

# Absolute path to a MacOSX10.9 SDK root, used ONLY to cross-build the kext from a
# modern host (the kext needs 10.9 kernel headers + libkmod, which the modern SDK
# does not provide). Fetch it with the shared fetch_10_9_sdk.sh (or the
# mavericks_fetch_10_9_sdk() helper). Leave UNSET on the native 10.9 dev box --
# the kext then falls back to CMAKE_OSX_SYSROOT (unchanged).
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
include(${CMAKE_SOURCE_DIR}/cmake/mavericks_kext.cmake)
add_subdirectory(kext-gesture)
add_subdirectory(examples/VoodooInputSample)
add_subdirectory(tests)

include(cmake/mt2_pkg.cmake)
include(cmake/mt2_dev.cmake)
