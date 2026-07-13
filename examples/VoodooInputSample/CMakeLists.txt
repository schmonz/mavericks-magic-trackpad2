add_executable(voodoo_sample_kext VoodooInputSample.cpp vinput_demo_path.c)
set_target_properties(voodoo_sample_kext PROPERTIES OUTPUT_NAME VoodooInputSample
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(voodoo_sample_kext PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/VoodooInput/VoodooInputMultitouch)
# voodoo_wire.h takes the real-kernel-types branch under KERNEL; both wire-touching TUs get it.
# vinput_demo_path.c is compiled as C++ into the kext (matching the main kext's shared-C pattern:
# kext mangling must match the .cpp callers; extern-C guard in the header ensures correct linkage).
set_source_files_properties(VoodooInputSample.cpp vinput_demo_path.c
  PROPERTIES COMPILE_DEFINITIONS KERNEL=1)
set_source_files_properties(vinput_demo_path.c
  PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;c++")
mavericks_kext(TARGET voodoo_sample_kext BUNDLE VoodooInputSample
  INFO_PLIST_IN ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in PUBLIC_TARGET voodoo_sample)
