# Plain C tools (IOKit + CoreFoundation).
foreach(t vhid_probe mt2_reenumerate mt2_usb_enable)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} "-framework IOKit" "-framework CoreFoundation")
endforeach()
# vhid_probe also needs the shared vhid helper (mavericks_vhid_mt1.c lives in mt2core).
target_link_libraries(vhid_probe mt2core)

# Shipped-userland 10.9 guard: only mt2_reenumerate ships (vhid_probe / mt2_usb_enable are dev-only).
mavericks_compat_guard(mt2_reenumerate)

# voodooinputmavericks_pane_arm lives under the prefpane dir; ApplicationServices + CoreFoundation.
add_executable(voodooinputmavericks_pane_arm voodooinputmavericks_prefpane/voodooinputmavericks_pane_arm.c)
target_link_libraries(voodooinputmavericks_pane_arm "-framework ApplicationServices" "-framework CoreFoundation")

# ObjC tools (ARC, Foundation + IOBluetooth).
foreach(t mt2_bt_bounce)
  add_executable(${t} ${t}.m)
  target_compile_options(${t} PRIVATE -fobjc-arc)
  target_link_libraries(${t} "-framework Foundation" "-framework IOBluetooth")
endforeach()

# mt2_usb_bt_handoff — the no-click USB->BT handoff daemon. Same ObjC/IOBluetooth base as bt_bounce,
# now driven by the shared presence observer (mavericks_presence_observer + the pure mavericks_presence SM) so it
# watches the same canonical USB/BT services as the prefpane.
add_executable(mt2_usb_bt_handoff mt2_usb_bt_handoff.m
  ${CMAKE_CURRENT_SOURCE_DIR}/voodooinputmavericks_prefpane/mavericks_presence_observer.c
  ${CMAKE_SOURCE_DIR}/src/mavericks_presence.c)
target_compile_options(mt2_usb_bt_handoff PRIVATE -fobjc-arc)
target_include_directories(mt2_usb_bt_handoff PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mt2_usb_bt_handoff
  "-framework Foundation" "-framework IOBluetooth" "-framework IOKit" "-framework CoreFoundation")
mavericks_compat_guard(mt2_usb_bt_handoff)

# voodooinputmavericks_pane_watch — Branch A launch-watcher (own process, normal ObjC/ARC + AppKit):
# sends MT2x/load to System Preferences on launch so our osax loads (no SIMBL).
add_executable(voodooinputmavericks_pane_watch voodooinputmavericks_prefpane/voodooinputmavericks_pane_watch.m)
target_compile_options(voodooinputmavericks_pane_watch PRIVATE -fobjc-arc)
target_link_libraries(voodooinputmavericks_pane_watch
  "-framework Foundation" "-framework AppKit" "-framework ApplicationServices")
mavericks_compat_guard(voodooinputmavericks_pane_watch)

# The prefpane-refresh osax: a pure-C GC-neutral payload injected into System
# Preferences. MODULE = -bundle; objc is used via the runtime only (no GC image info).
set(OSAX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/voodooinputmavericks_prefpane)
add_library(VoodooInputMavericksPane MODULE ${OSAX_DIR}/voodooinputmavericks_prefpane_refresh.c ${OSAX_DIR}/mavericks_presence_observer.c ${CMAKE_SOURCE_DIR}/src/mavericks_presence.c ${CMAKE_SOURCE_DIR}/src/mavericks_single_load.c)
target_include_directories(VoodooInputMavericksPane PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The About tab's version label reads this at compile time (kept in lock-step with MAVERICKS_VERSION).
target_compile_definitions(VoodooInputMavericksPane PRIVATE MAVERICKS_VERSION_STR="${MAVERICKS_VERSION}")
set_target_properties(VoodooInputMavericksPane PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME VoodooInputMavericksPane)
target_link_libraries(VoodooInputMavericksPane "-framework IOKit" "-framework CoreFoundation" "-framework Carbon" objc)
# Assemble VoodooInputMavericksPane.osax/Contents/{MacOS,Info.plist}
set(OSAX_OUT ${CMAKE_BINARY_DIR}/VoodooInputMavericksPane.osax)
add_custom_command(TARGET VoodooInputMavericksPane POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OSAX_OUT}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:VoodooInputMavericksPane> ${OSAX_OUT}/Contents/MacOS/VoodooInputMavericksPane
  COMMAND ${CMAKE_COMMAND} -E copy ${OSAX_DIR}/Info.plist ${OSAX_OUT}/Contents/Info.plist
  COMMENT "Assembling VoodooInputMavericksPane.osax")

# Same payload as a DYLD_INSERT_LIBRARIES dylib (a loader we control at launch).
add_library(VoodooInputMavericksPane_dylib SHARED ${OSAX_DIR}/voodooinputmavericks_prefpane_refresh.c ${OSAX_DIR}/mavericks_presence_observer.c ${CMAKE_SOURCE_DIR}/src/mavericks_presence.c ${CMAKE_SOURCE_DIR}/src/mavericks_single_load.c)
target_include_directories(VoodooInputMavericksPane_dylib PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(VoodooInputMavericksPane_dylib PRIVATE MAVERICKS_VERSION_STR="${MAVERICKS_VERSION}")
set_target_properties(VoodooInputMavericksPane_dylib PROPERTIES
  PREFIX "" OUTPUT_NAME VoodooInputMavericksPane SUFFIX ".dylib"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(VoodooInputMavericksPane_dylib "-framework IOKit" "-framework CoreFoundation" "-framework Carbon" objc)

# Same payload packaged as a SIMBL plugin bundle (SIMBLAgent injects it at launch —
# the current working loader). Same -bundle binary, SIMBL-Info.plist, .bundle dir.
add_library(VoodooInputMavericksPane_simbl MODULE ${OSAX_DIR}/voodooinputmavericks_prefpane_refresh.c ${OSAX_DIR}/mavericks_presence_observer.c ${CMAKE_SOURCE_DIR}/src/mavericks_presence.c ${CMAKE_SOURCE_DIR}/src/mavericks_single_load.c)
target_include_directories(VoodooInputMavericksPane_simbl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(VoodooInputMavericksPane_simbl PRIVATE MAVERICKS_VERSION_STR="${MAVERICKS_VERSION}")
# Distinct output dir: VoodooInputMavericksPane (osax) also emits an unprefixed/unsuffixed
# "VoodooInputMavericksPane" in tools/ -- same path => a parallel-build race where one target's
# POST_BUILD bundle never gets assembled. Give SIMBL its own dir so the paths differ.
set_target_properties(VoodooInputMavericksPane_simbl PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME VoodooInputMavericksPane
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/simbl)
target_link_libraries(VoodooInputMavericksPane_simbl "-framework IOKit" "-framework CoreFoundation" "-framework Carbon" objc)
set(SIMBL_OUT ${CMAKE_BINARY_DIR}/VoodooInputMavericksPane.bundle)
add_custom_command(TARGET VoodooInputMavericksPane_simbl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIMBL_OUT}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:VoodooInputMavericksPane_simbl> ${SIMBL_OUT}/Contents/MacOS/VoodooInputMavericksPane
  COMMAND ${CMAKE_COMMAND} -E copy ${OSAX_DIR}/SIMBL-Info.plist ${SIMBL_OUT}/Contents/Info.plist
  COMMENT "Assembling VoodooInputMavericksPane.bundle (SIMBL)")

# GC-neutral guard, BAKED IN (not a manual otool check): every payload injected into 10.9's ObjC-GC
# System Preferences must be free of __objc_imageinfo, or GC refuses to load it. Fail the build here so
# a stray Obj-C construct can never ship silently. See cmake/check_gc_neutral.sh.
foreach(_gc VoodooInputMavericksPane VoodooInputMavericksPane_dylib VoodooInputMavericksPane_simbl)
  add_custom_command(TARGET ${_gc} POST_BUILD
    COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/check_gc_neutral.sh $<TARGET_FILE:${_gc}>
    COMMENT "Verifying ${_gc} is GC-neutral (no __objc_imageinfo)"
    VERBATIM)
endforeach()

# 10.9 compat guard for the two SHIPPED payloads (osax + SIMBL bundle). VoodooInputMavericksPane_dylib is
# a dev-only injection variant and is NOT shipped, so it is not guarded.
foreach(_cg VoodooInputMavericksPane VoodooInputMavericksPane_simbl)
  mavericks_compat_guard(${_cg})
endforeach()

# Aggregate convenience target: ALL prefpane-loader variants (osax + dylib + SIMBL bundle) + arm. Building
# just one loader while a stale copy of another stays deployed is the dual-loader footgun (a stale SIMBL
# bundle watching retired classes bit us 2026-07-20) — so this rebuilds every variant together. For a
# release-identical on-device install use the `install-pkg` target, not hand-copied pieces.
add_custom_target(prefpane-refresh DEPENDS VoodooInputMavericksPane VoodooInputMavericksPane_dylib VoodooInputMavericksPane_simbl voodooinputmavericks_pane_arm)
add_custom_target(prefpane-refresh-simbl DEPENDS VoodooInputMavericksPane_simbl)  # kept: back-compat alias

# Operational install/uninstall (privilege handled here; fully reversible).
# NOTE: each step is its own COMMAND with NATIVE args (CMake quotes each arg, incl.
# the space-containing SIMBL path). Do NOT wrap in `sh -c "a && b"` — CMake space-
# escapes that single string and breaks the && / quoting (Phase-0 found this bug).
# The standalone osax ships in /Library/ScriptingAdditions (Phase 0: loads there via
# our MT2x/load event, same as /System/Library; /Library is the less-invasive choice).
add_custom_target(prefpane-refresh-install
  COMMAND sudo rm -rf /Library/ScriptingAdditions/VoodooInputMavericksPane.osax
  COMMAND sudo cp -R ${OSAX_OUT} /Library/ScriptingAdditions/
  # Force an OpenScripting additions re-scan so our (re)installed handler is registered before the
  # next System Preferences launch (avoids the "needs 2-3 relaunches" race). Native args only — the
  # redirection + `|| true` live in the script (see the NOTE above: an inline `sh -c "…"` gets
  # space-escaped by CMake and shreds the redirection, Error 127, aborting the make chain).
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/force_osa_rescan.sh
  COMMAND ${CMAKE_COMMAND} -E echo "Installed /Library/ScriptingAdditions/VoodooInputMavericksPane.osax"
  COMMENT "Installing VoodooInputMavericksPane.osax")
add_dependencies(prefpane-refresh-install VoodooInputMavericksPane)
add_custom_target(prefpane-refresh-uninstall
  COMMAND sudo rm -rf /Library/ScriptingAdditions/VoodooInputMavericksPane.osax
  COMMAND ${CMAKE_COMMAND} -E echo "Removed /Library/ScriptingAdditions/VoodooInputMavericksPane.osax"
  COMMENT "Removing VoodooInputMavericksPane.osax")
add_custom_target(prefpane-refresh-simbl-install
  COMMAND sudo rm -rf "/Library/Application Support/SIMBL/Plugins/VoodooInputMavericksPane.bundle"
  COMMAND sudo cp -R ${SIMBL_OUT} "/Library/Application Support/SIMBL/Plugins/"
  COMMAND ${CMAKE_COMMAND} -E echo "Installed into SIMBL Plugins - relaunch System Preferences to inject"
  COMMENT "Installing VoodooInputMavericksPane.bundle (SIMBL)")
add_dependencies(prefpane-refresh-simbl-install VoodooInputMavericksPane_simbl)
add_custom_target(prefpane-refresh-simbl-uninstall
  COMMAND sudo rm -rf "/Library/Application Support/SIMBL/Plugins/VoodooInputMavericksPane.bundle"
  COMMAND ${CMAKE_COMMAND} -E echo "Removed VoodooInputMavericksPane.bundle from SIMBL Plugins"
  COMMENT "Removing VoodooInputMavericksPane.bundle (SIMBL)")

# The standalone-osax loader (Branch A): watcher binary + per-user LaunchAgent. The
# load/unload tolerance lives in cmake/dev_prefpane_watch.sh. Never touches SIMBL.
add_custom_target(prefpane-watch-install
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_prefpane_watch.sh install
          ${CMAKE_BINARY_DIR}/sbin/voodooinputmavericks_pane_watch ${CMAKE_SOURCE_DIR}/dist/com.schmonz.voodooinputmavericks.panewatch.plist
  COMMENT "Installing + loading the prefpane-refresh watcher LaunchAgent")
add_dependencies(prefpane-watch-install voodooinputmavericks_pane_watch)

# Full teardown of the standalone delivery: agent + watcher + osax (SIMBL untouched).
add_custom_target(prefpane-uninstall
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_prefpane_watch.sh uninstall
  COMMAND sudo rm -rf /Library/ScriptingAdditions/VoodooInputMavericksPane.osax
  COMMAND ${CMAKE_COMMAND} -E echo "Removed osax + watcher + LaunchAgent - SIMBL untouched"
  COMMENT "Uninstalling the prefpane-refresh delivery (osax + watcher + LaunchAgent)")

# USB->BT handoff LaunchDaemon: arms an IOKit termination notification on com_schmonz_MT2USBReader;
# on cable-unplug it wakes the deep-idle BT MT2 via openConnection (no physical click). Root daemon
# (runs regardless of GUI session). Reuses the mt2_usb_bt_handoff target (-> ${CMAKE_BINARY_DIR}/sbin).
add_custom_target(usb-bt-handoff-install
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_usb_bt_handoff.sh install
          ${CMAKE_BINARY_DIR}/sbin/mt2_usb_bt_handoff ${CMAKE_SOURCE_DIR}/dist/com.schmonz.voodooinputmavericks.usbbthandoff.plist
  COMMENT "Installing + loading the USB->BT handoff LaunchDaemon")
add_dependencies(usb-bt-handoff-install mt2_usb_bt_handoff)
add_custom_target(usb-bt-handoff-uninstall
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_usb_bt_handoff.sh uninstall
  COMMENT "Uninstalling the USB->BT handoff LaunchDaemon")

# ---- MavericksTrackpad2Updater.app (shared MavericksSparkle) --------------------------------------
# Built only when a fetched Sparkle framework path is provided (MT2_SPARKLE_FRAMEWORK); otherwise the
# rest of the tree still builds without the network fetch. The updater host now lives in
# mavericks-shared-cmake (templated main.m/Info.plist); we just configure it per product here.
if(MT2_SPARKLE_FRAMEWORK)
  include(MavericksSparkle)
  mavericks_add_updater_app(
    NAME MavericksTrackpad2Updater
    PRODUCT_NAME "Mavericks Trackpad 2"
    BUNDLE_ID com.schmonz.MavericksTrackpad2Updater
    FEED_URL "https://github.com/schmonz/mavericks-magic-trackpad2/releases/latest/download/appcast.xml"
    ED_PUBKEY "E8ZTStxLUfSWtTDg24bwnanLII20R27NVm+RxZtElGs="
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/voodooinputmavericks_updater/MavericksTrackpad2Updater.icns"
    VERSION "${MAVERICKS_VERSION}"
    AUTO_CHECK false
    SPARKLE_FRAMEWORK "${MT2_SPARKLE_FRAMEWORK}"
    LOG_PREFIX "MT2Updater"
    CONFIRM_TITLE "Mavericks Trackpad 2 updated"
    CONFIRM_BODY "The trackpad driver was updated and is ready to use."
    RELAUNCH_MARKER "/tmp/.mt2updater-relaunched"
    PANE_HINT_KEY "MT2AvailableUpdateVersion")

  mavericks_compat_guard(MavericksTrackpad2Updater)

  add_custom_target(updater-install
    COMMAND sudo ${CMAKE_COMMAND} -E rm -rf /usr/local/lib/voodooinputmavericks/MavericksTrackpad2Updater.app
    COMMAND sudo ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/MavericksTrackpad2Updater.app /usr/local/lib/voodooinputmavericks/MavericksTrackpad2Updater.app
    COMMAND ${CMAKE_COMMAND} -E echo "Installed /usr/local/lib/voodooinputmavericks/MavericksTrackpad2Updater.app"
    COMMENT "Installing MavericksTrackpad2Updater.app")
  add_dependencies(updater-install MavericksTrackpad2Updater)
endif()

# ---- Native EdDSA signer + keygen (shared MavericksSparkle) --------------------------------------
# Built only when a fetched orlp/ed25519 src dir is provided (MT2_ED25519_SRC). The signer/keygen
# sources now live in mavericks-shared-cmake; this builds mavericks_sign_update + mavericks_generate_keys
# (into ${CMAKE_BINARY_DIR}/sbin via the project-wide CMAKE_RUNTIME_OUTPUT_DIRECTORY).
if(MT2_ED25519_SRC)
  include(MavericksSparkle)
  mavericks_add_signer(ED25519_SRC "${MT2_ED25519_SRC}")
endif()
