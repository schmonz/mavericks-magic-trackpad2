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

# Same payload as a DYLD_INSERT_LIBRARIES dylib (a loader we control at launch).
add_library(MT2PaneRefresh_dylib SHARED ${OSAX_DIR}/mt2_prefpane_refresh.c)
set_target_properties(MT2PaneRefresh_dylib PROPERTIES
  PREFIX "" OUTPUT_NAME MT2PaneRefresh SUFFIX ".dylib"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(MT2PaneRefresh_dylib "-framework IOKit" "-framework CoreFoundation" objc)

# Same payload packaged as a SIMBL plugin bundle (SIMBLAgent injects it at launch —
# the current working loader). Same -bundle binary, SIMBL-Info.plist, .bundle dir.
add_library(MT2PaneRefresh_simbl MODULE ${OSAX_DIR}/mt2_prefpane_refresh.c)
set_target_properties(MT2PaneRefresh_simbl PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME MT2PaneRefresh)
target_link_libraries(MT2PaneRefresh_simbl "-framework IOKit" "-framework CoreFoundation" objc)
set(SIMBL_OUT ${CMAKE_BINARY_DIR}/MT2PaneRefresh.bundle)
add_custom_command(TARGET MT2PaneRefresh_simbl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIMBL_OUT}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MT2PaneRefresh_simbl> ${SIMBL_OUT}/Contents/MacOS/MT2PaneRefresh
  COMMAND ${CMAKE_COMMAND} -E copy ${OSAX_DIR}/SIMBL-Info.plist ${SIMBL_OUT}/Contents/Info.plist
  COMMENT "Assembling MT2PaneRefresh.bundle (SIMBL)")

# Aggregate convenience target (mirrors the Makefile `prefpane-refresh`: osax + dylib + arm).
add_custom_target(prefpane-refresh DEPENDS MT2PaneRefresh MT2PaneRefresh_dylib mt2_pane_arm)
add_custom_target(prefpane-refresh-simbl DEPENDS MT2PaneRefresh_simbl)

# Operational install/uninstall (privilege handled here; fully reversible).
# NOTE: each step is its own COMMAND with NATIVE args (CMake quotes each arg, incl.
# the space-containing SIMBL path). Do NOT wrap in `sh -c "a && b"` — CMake space-
# escapes that single string and breaks the && / quoting (Phase-0 found this bug).
# The standalone osax ships in /Library/ScriptingAdditions (Phase 0: loads there via
# our MT2x/load event, same as /System/Library; /Library is the less-invasive choice).
add_custom_target(prefpane-refresh-install
  COMMAND sudo rm -rf /Library/ScriptingAdditions/MT2PaneRefresh.osax
  COMMAND sudo cp -R ${OSAX_OUT} /Library/ScriptingAdditions/
  COMMAND ${CMAKE_COMMAND} -E echo "Installed /Library/ScriptingAdditions/MT2PaneRefresh.osax"
  COMMENT "Installing MT2PaneRefresh.osax")
add_dependencies(prefpane-refresh-install MT2PaneRefresh)
add_custom_target(prefpane-refresh-uninstall
  COMMAND sudo rm -rf /Library/ScriptingAdditions/MT2PaneRefresh.osax
  COMMAND ${CMAKE_COMMAND} -E echo "Removed /Library/ScriptingAdditions/MT2PaneRefresh.osax"
  COMMENT "Removing MT2PaneRefresh.osax")
add_custom_target(prefpane-refresh-simbl-install
  COMMAND sudo rm -rf "/Library/Application Support/SIMBL/Plugins/MT2PaneRefresh.bundle"
  COMMAND sudo cp -R ${SIMBL_OUT} "/Library/Application Support/SIMBL/Plugins/"
  COMMAND ${CMAKE_COMMAND} -E echo "Installed into SIMBL Plugins - relaunch System Preferences to inject"
  COMMENT "Installing MT2PaneRefresh.bundle (SIMBL)")
add_dependencies(prefpane-refresh-simbl-install MT2PaneRefresh_simbl)
add_custom_target(prefpane-refresh-simbl-uninstall
  COMMAND sudo rm -rf "/Library/Application Support/SIMBL/Plugins/MT2PaneRefresh.bundle"
  COMMAND ${CMAKE_COMMAND} -E echo "Removed MT2PaneRefresh.bundle from SIMBL Plugins"
  COMMENT "Removing MT2PaneRefresh.bundle (SIMBL)")
