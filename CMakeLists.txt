cmake_minimum_required(VERSION 3.10)
project(mt2 VERSION 1.0.0 LANGUAGES C CXX)

# Native 10.9 build for now; Project B adds the modern-SDK cross-build on top.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "" FORCE)
endif()
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
