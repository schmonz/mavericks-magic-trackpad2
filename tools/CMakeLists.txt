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
