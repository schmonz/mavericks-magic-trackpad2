# Plain C tools (IOKit + CoreFoundation).
foreach(t vhid_probe mt2_reenumerate mt2_usb_enable)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} "-framework IOKit" "-framework CoreFoundation")
endforeach()
# vhid_probe also needs the shared vhid helper (vhid_mt1.c lives in mt2core).
target_link_libraries(vhid_probe mt2core)

# mt2_pane_arm lives under the prefpane dir; ApplicationServices + CoreFoundation.
add_executable(mt2_pane_arm mt2_prefpane_refresh/mt2_pane_arm.c)
target_link_libraries(mt2_pane_arm "-framework ApplicationServices" "-framework CoreFoundation")

# ObjC tools (ARC, Foundation + IOBluetooth).
foreach(t mt2_set_btname mt2_bt_bounce)
  add_executable(${t} ${t}.m)
  target_compile_options(${t} PRIVATE -fobjc-arc)
  target_link_libraries(${t} "-framework Foundation" "-framework IOBluetooth")
endforeach()

# The prefpane-refresh osax: a pure-C GC-neutral payload injected into System
# Preferences. MODULE = -bundle; objc is used via the runtime only (no GC image info).
set(OSAX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mt2_prefpane_refresh)
add_library(MT2PaneRefresh MODULE ${OSAX_DIR}/mt2_prefpane_refresh.c)
set_target_properties(MT2PaneRefresh PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME MT2PaneRefresh)
target_link_libraries(MT2PaneRefresh "-framework IOKit" "-framework CoreFoundation" objc)
# Assemble MT2PaneRefresh.osax/Contents/{MacOS,Info.plist}
set(OSAX_OUT ${CMAKE_BINARY_DIR}/MT2PaneRefresh.osax)
add_custom_command(TARGET MT2PaneRefresh POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OSAX_OUT}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MT2PaneRefresh> ${OSAX_OUT}/Contents/MacOS/MT2PaneRefresh
  COMMAND ${CMAKE_COMMAND} -E copy ${OSAX_DIR}/Info.plist ${OSAX_OUT}/Contents/Info.plist
  COMMENT "Assembling MT2PaneRefresh.osax")
