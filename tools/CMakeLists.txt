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
foreach(t mt2_bt_bounce)
  add_executable(${t} ${t}.m)
  target_compile_options(${t} PRIVATE -fobjc-arc)
  target_link_libraries(${t} "-framework Foundation" "-framework IOBluetooth")
endforeach()

# mt2_usb_bt_handoff — the no-click USB->BT handoff daemon. Same ObjC/IOBluetooth base as bt_bounce
# but also needs IOKit for the com_schmonz_MT2USBReader termination notification.
add_executable(mt2_usb_bt_handoff mt2_usb_bt_handoff.m)
target_compile_options(mt2_usb_bt_handoff PRIVATE -fobjc-arc)
target_link_libraries(mt2_usb_bt_handoff
  "-framework Foundation" "-framework IOBluetooth" "-framework IOKit" "-framework CoreFoundation")

# mt2_pane_watch — Branch A launch-watcher (own process, normal ObjC/ARC + AppKit):
# sends MT2x/load to System Preferences on launch so our osax loads (no SIMBL).
add_executable(mt2_pane_watch mt2_prefpane_refresh/mt2_pane_watch.m)
target_compile_options(mt2_pane_watch PRIVATE -fobjc-arc)
target_link_libraries(mt2_pane_watch
  "-framework Foundation" "-framework AppKit" "-framework ApplicationServices")

# The prefpane-refresh osax: a pure-C GC-neutral payload injected into System
# Preferences. MODULE = -bundle; objc is used via the runtime only (no GC image info).
set(OSAX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mt2_prefpane_refresh)
add_library(MT2PaneRefresh MODULE ${OSAX_DIR}/mt2_prefpane_refresh.c ${CMAKE_SOURCE_DIR}/src/mt2_pane_sm.c ${CMAKE_SOURCE_DIR}/src/mt2_single_load.c)
target_include_directories(MT2PaneRefresh PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The About tab's version label reads this at compile time (kept in lock-step with MT2_VERSION).
target_compile_definitions(MT2PaneRefresh PRIVATE MT2_VERSION_STR="${MT2_VERSION}")
set_target_properties(MT2PaneRefresh PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME MT2PaneRefresh)
target_link_libraries(MT2PaneRefresh "-framework IOKit" "-framework CoreFoundation" "-framework Carbon" objc)
# Assemble MT2PaneRefresh.osax/Contents/{MacOS,Info.plist}
set(OSAX_OUT ${CMAKE_BINARY_DIR}/MT2PaneRefresh.osax)
add_custom_command(TARGET MT2PaneRefresh POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OSAX_OUT}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MT2PaneRefresh> ${OSAX_OUT}/Contents/MacOS/MT2PaneRefresh
  COMMAND ${CMAKE_COMMAND} -E copy ${OSAX_DIR}/Info.plist ${OSAX_OUT}/Contents/Info.plist
  COMMENT "Assembling MT2PaneRefresh.osax")

# Same payload as a DYLD_INSERT_LIBRARIES dylib (a loader we control at launch).
add_library(MT2PaneRefresh_dylib SHARED ${OSAX_DIR}/mt2_prefpane_refresh.c ${CMAKE_SOURCE_DIR}/src/mt2_pane_sm.c ${CMAKE_SOURCE_DIR}/src/mt2_single_load.c)
target_include_directories(MT2PaneRefresh_dylib PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(MT2PaneRefresh_dylib PRIVATE MT2_VERSION_STR="${MT2_VERSION}")
set_target_properties(MT2PaneRefresh_dylib PROPERTIES
  PREFIX "" OUTPUT_NAME MT2PaneRefresh SUFFIX ".dylib"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(MT2PaneRefresh_dylib "-framework IOKit" "-framework CoreFoundation" "-framework Carbon" objc)

# Same payload packaged as a SIMBL plugin bundle (SIMBLAgent injects it at launch —
# the current working loader). Same -bundle binary, SIMBL-Info.plist, .bundle dir.
add_library(MT2PaneRefresh_simbl MODULE ${OSAX_DIR}/mt2_prefpane_refresh.c ${CMAKE_SOURCE_DIR}/src/mt2_pane_sm.c ${CMAKE_SOURCE_DIR}/src/mt2_single_load.c)
target_include_directories(MT2PaneRefresh_simbl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(MT2PaneRefresh_simbl PRIVATE MT2_VERSION_STR="${MT2_VERSION}")
# Distinct output dir: MT2PaneRefresh (osax) also emits an unprefixed/unsuffixed
# "MT2PaneRefresh" in tools/ -- same path => a parallel-build race where one target's
# POST_BUILD bundle never gets assembled. Give SIMBL its own dir so the paths differ.
set_target_properties(MT2PaneRefresh_simbl PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME MT2PaneRefresh
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/simbl)
target_link_libraries(MT2PaneRefresh_simbl "-framework IOKit" "-framework CoreFoundation" "-framework Carbon" objc)
set(SIMBL_OUT ${CMAKE_BINARY_DIR}/MT2PaneRefresh.bundle)
add_custom_command(TARGET MT2PaneRefresh_simbl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIMBL_OUT}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MT2PaneRefresh_simbl> ${SIMBL_OUT}/Contents/MacOS/MT2PaneRefresh
  COMMAND ${CMAKE_COMMAND} -E copy ${OSAX_DIR}/SIMBL-Info.plist ${SIMBL_OUT}/Contents/Info.plist
  COMMENT "Assembling MT2PaneRefresh.bundle (SIMBL)")

# GC-neutral guard, BAKED IN (not a manual otool check): every payload injected into 10.9's ObjC-GC
# System Preferences must be free of __objc_imageinfo, or GC refuses to load it. Fail the build here so
# a stray Obj-C construct can never ship silently. See cmake/check_gc_neutral.sh.
foreach(_gc MT2PaneRefresh MT2PaneRefresh_dylib MT2PaneRefresh_simbl)
  add_custom_command(TARGET ${_gc} POST_BUILD
    COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/check_gc_neutral.sh $<TARGET_FILE:${_gc}>
    COMMENT "Verifying ${_gc} is GC-neutral (no __objc_imageinfo)"
    VERBATIM)
endforeach()

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
  # Force an OpenScripting additions re-scan so our (re)installed handler is registered before the
  # next System Preferences launch (avoids the "needs 2-3 relaunches" race). Native args only — the
  # redirection + `|| true` live in the script (see the NOTE above: an inline `sh -c "…"` gets
  # space-escaped by CMake and shreds the redirection, Error 127, aborting the make chain).
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/force_osa_rescan.sh
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

# The standalone-osax loader (Branch A): watcher binary + per-user LaunchAgent. The
# load/unload tolerance lives in cmake/dev_prefpane_watch.sh. Never touches SIMBL.
add_custom_target(prefpane-watch-install
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_prefpane_watch.sh install
          ${CMAKE_BINARY_DIR}/sbin/mt2_pane_watch ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2panewatch.plist
  COMMENT "Installing + loading the prefpane-refresh watcher LaunchAgent")
add_dependencies(prefpane-watch-install mt2_pane_watch)

# Full teardown of the standalone delivery: agent + watcher + osax (SIMBL untouched).
add_custom_target(prefpane-uninstall
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_prefpane_watch.sh uninstall
  COMMAND sudo rm -rf /Library/ScriptingAdditions/MT2PaneRefresh.osax
  COMMAND ${CMAKE_COMMAND} -E echo "Removed osax + watcher + LaunchAgent - SIMBL untouched"
  COMMENT "Uninstalling the prefpane-refresh delivery (osax + watcher + LaunchAgent)")

# USB->BT handoff LaunchDaemon: arms an IOKit termination notification on com_schmonz_MT2USBReader;
# on cable-unplug it wakes the deep-idle BT MT2 via openConnection (no physical click). Root daemon
# (runs regardless of GUI session). Reuses the mt2_usb_bt_handoff target (-> ${CMAKE_BINARY_DIR}/sbin).
add_custom_target(usb-bt-handoff-install
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_usb_bt_handoff.sh install
          ${CMAKE_BINARY_DIR}/sbin/mt2_usb_bt_handoff ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2usbbthandoff.plist
  COMMENT "Installing + loading the USB->BT handoff LaunchDaemon")
add_dependencies(usb-bt-handoff-install mt2_usb_bt_handoff)
add_custom_target(usb-bt-handoff-uninstall
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_usb_bt_handoff.sh uninstall
  COMMENT "Uninstalling the USB->BT handoff LaunchDaemon")

# ---- MavericksTrackpad2Updater.app (Sparkle 1.27 host) --------------------------------------------
# Built only when a fetched Sparkle framework path is provided (MT2_SPARKLE_FRAMEWORK); otherwise the
# rest of the tree still builds without the network fetch. Minimal-declaration main.m (no Sparkle
# headers) -> builds on clang 6.0 (dev) and modern clang (CI).
if(MT2_SPARKLE_FRAMEWORK)
  set(UPD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mt2_updater)
  set(UPD_APP ${CMAKE_BINARY_DIR}/MavericksTrackpad2Updater.app)
  configure_file(${UPD_DIR}/Info.plist.in ${CMAKE_BINARY_DIR}/MavericksTrackpad2Updater-Info.plist @ONLY)

  add_executable(MavericksTrackpad2Updater ${UPD_DIR}/main.m)
  set_target_properties(MavericksTrackpad2Updater PROPERTIES OUTPUT_NAME MavericksTrackpad2Updater)
  target_compile_options(MavericksTrackpad2Updater PRIVATE -fobjc-arc)
  target_link_libraries(MavericksTrackpad2Updater PRIVATE
    "-F${MT2_SPARKLE_FRAMEWORK}/.."
    "-framework Sparkle"
    "-framework Cocoa"
    "-Wl,-rpath,@executable_path/../Frameworks")

  add_custom_command(TARGET MavericksTrackpad2Updater POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UPD_APP}/Contents/MacOS
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UPD_APP}/Contents/Frameworks
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UPD_APP}/Contents/Resources
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MavericksTrackpad2Updater> ${UPD_APP}/Contents/MacOS/MavericksTrackpad2Updater
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/MavericksTrackpad2Updater-Info.plist ${UPD_APP}/Contents/Info.plist
    # Product .icns (CFBundleIconFile) so Sparkle's dialog shows the trackpad, not a generic icon.
    # Committed pre-built from Apple's TrackpadPicture.png (see the file's provenance) -> no CI dependency.
    COMMAND ${CMAKE_COMMAND} -E copy ${UPD_DIR}/MavericksTrackpad2Updater.icns ${UPD_APP}/Contents/Resources/MavericksTrackpad2Updater.icns
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${MT2_SPARKLE_FRAMEWORK} ${UPD_APP}/Contents/Frameworks/Sparkle.framework
    COMMENT "Assembling MavericksTrackpad2Updater.app")

  add_custom_target(updater-install
    COMMAND sudo ${CMAKE_COMMAND} -E rm -rf /usr/local/lib/mt2d/MavericksTrackpad2Updater.app
    COMMAND sudo ${CMAKE_COMMAND} -E copy_directory ${UPD_APP} /usr/local/lib/mt2d/MavericksTrackpad2Updater.app
    COMMAND ${CMAKE_COMMAND} -E echo "Installed /usr/local/lib/mt2d/MavericksTrackpad2Updater.app"
    COMMENT "Installing MavericksTrackpad2Updater.app")
  add_dependencies(updater-install MavericksTrackpad2Updater)
endif()

# ---- Native EdDSA signer + keygen (10.9-buildable Sparkle sign_update / generate_keys) -----------
# Built only when a fetched orlp/ed25519 src dir is provided (MT2_ED25519_SRC = $(sh
# cmake/fetch_ed25519.sh)). Uses the SAME ed25519 library Sparkle vendors, so signatures are
# byte-identical to the official Swift sign_update and the framework accepts them -- but this builds
# native on the 10.9 toolchain (plain C, no Swift). sign_update is habitual; keep it available locally.
if(MT2_ED25519_SRC)
  file(GLOB ED25519_SRCS "${MT2_ED25519_SRC}/*.c")
  add_executable(mt2_sign_update   mt2_sign_update/sign_update.c   ${ED25519_SRCS})
  add_executable(mt2_generate_keys mt2_sign_update/generate_keys.c ${ED25519_SRCS})
  foreach(t mt2_sign_update mt2_generate_keys)
    target_include_directories(${t} PRIVATE "${MT2_ED25519_SRC}" ${CMAKE_CURRENT_SOURCE_DIR}/mt2_sign_update)
  endforeach()
endif()
