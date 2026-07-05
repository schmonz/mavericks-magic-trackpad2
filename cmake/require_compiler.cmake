# require_compiler.cmake — fail configuration FAST on a poorly-fitting compiler.
#
# Included right after project() (which populates CMAKE_<LANG>_COMPILER_ID/_VERSION).
#
# Why this exists: it must be hard to pick the wrong toolchain by accident — especially in an IDE
# (e.g. CLion), which discovers a compiler from PATH and can silently land on a pkgsrc/Homebrew gcc.
#
# The rule is uniform across both build hosts, because both build the SAME things — the 10.9 kext, the
# GC-neutral injected osax, and code against Apple's IOKit/IOBluetooth frameworks — which only Apple's
# clang + SDK produce loadable artifacts for. The two hosts differ ONLY in Xcode/AppleClang version:
#   * OS X 10.9 "Mavericks" (Darwin 13): Xcode 6 AppleClang, /usr/bin/clang.
#   * macOS 26 "Tahoe" (Darwin 25): the current Xcode's AppleClang.
# So: require CMAKE_<LANG>_COMPILER_ID == AppleClang, and reject everything else (gcc, Homebrew/pkgsrc
# LLVM clang, etc.). A functional blocks check backstops a mislabeled/misconfigured compiler.

foreach(_lang C CXX)
  if(NOT CMAKE_${_lang}_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR
      "MT2 requires Apple's clang (AppleClang) for ${_lang}, but got "
      "'${CMAKE_${_lang}_COMPILER_ID}' (${CMAKE_${_lang}_COMPILER}).\n"
      "Both build hosts use Apple's clang — Xcode 6's /usr/bin/clang on OS X 10.9, the current "
      "Xcode's clang on macOS 26 — because the kext, the GC-neutral osax, and the IOKit/IOBluetooth "
      "code only build correctly with Apple's clang + SDK. A gcc or a Homebrew/pkgsrc LLVM clang "
      "(usually pulled in by an IDE's PATH) produces artifacts that will not load.\n"
      "Fix: point the compiler at Apple's clang and re-configure a CLEAN build dir, e.g.:\n"
      "    cmake -S . -B build -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++\n"
      "In CLion: Settings > Build > CMake > Toolchain, set the C/C++ compiler to /usr/bin/clang "
      "(or the Xcode clang on macOS 26), and reset the CMake cache.")
  endif()
endforeach()

# Functional backstop: the injected osax uses Apple blocks (dispatch_source_set_event_handler et al.).
# A compiler that cannot build blocks is wrong regardless of what its id/version claims. Result is
# cached, so this try-compile runs once per build dir.
include(CheckCSourceCompiles)
set(_save_req_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-fblocks")
check_c_source_compiles("int main(void){ void (^b)(void) = ^{}; b(); return 0; }" MT2_CC_HAS_BLOCKS)
set(CMAKE_REQUIRED_FLAGS "${_save_req_flags}")
if(NOT MT2_CC_HAS_BLOCKS)
  message(FATAL_ERROR
    "The selected C compiler (${CMAKE_C_COMPILER_ID}, ${CMAKE_C_COMPILER}) cannot build Apple "
    "blocks (-fblocks); the injected osax needs them. Use Apple's clang.")
endif()

message(STATUS "MT2 compiler check OK: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} "
               "(${CMAKE_C_COMPILER}), host Darwin ${CMAKE_SYSTEM_VERSION}")
