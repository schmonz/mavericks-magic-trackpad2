# Plain C tools (IOKit + CoreFoundation).
foreach(t vhid_probe mt2_reenumerate mt2_usb_enable)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} "-framework IOKit" "-framework CoreFoundation")
endforeach()
# vhid_probe also needs the shared vhid helper (mavericks_vhid_mt1.c lives in mt2core).
target_link_libraries(vhid_probe mt2core)

# Shipped-userland 10.9 guard: only mt2_reenumerate ships (vhid_probe / mt2_usb_enable are dev-only).
mavericks_compat_guard(mt2_reenumerate)

# ObjC tools (ARC, Foundation + IOBluetooth).
foreach(t mt2_bt_bounce)
  add_executable(${t} ${t}.m)
  target_compile_options(${t} PRIVATE -fobjc-arc)
  target_link_libraries(${t} "-framework Foundation" "-framework IOBluetooth")
endforeach()

# mt2_linkstated — the MT2's transport link-state coordinator (reconnect keeper + USB-removal handoff +
# USB-appear yield + USB-appear hidd gesture-kick + shutdown quiesce; see the file header). Same
# ObjC/IOBluetooth base as bt_bounce, driven by the shared presence observer (mavericks_presence_observer +
# the pure mavericks_presence SM) so it reads the same canonical USB/BT transport truth as the prefpane.
add_executable(mt2_linkstated mt2_linkstated.m
  ${CMAKE_CURRENT_SOURCE_DIR}/voodooinputmavericks_prefpane/mavericks_presence_observer.c
  ${CMAKE_SOURCE_DIR}/src/mavericks_presence.c)
target_compile_options(mt2_linkstated PRIVATE -fobjc-arc)
target_include_directories(mt2_linkstated PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mt2_linkstated
  "-framework Foundation" "-framework IOBluetooth" "-framework IOKit" "-framework CoreFoundation")
mavericks_compat_guard(mt2_linkstated)

# The prefpane-refresh payload: a pure-C GC-neutral bundle injected into System Preferences, shipped ONLY
# as a SIMBL plugin. OSAX_DIR is the shared source dir (used by the SIMBL target below).
set(OSAX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/voodooinputmavericks_prefpane)

# The payload packaged as a SIMBL plugin bundle (SIMBLAgent injects it at launch —
# the current working loader). Same -bundle binary, SIMBL-Info.plist, .bundle dir.
add_library(VoodooInputMavericksPane_simbl MODULE ${OSAX_DIR}/voodooinputmavericks_prefpane_refresh.c ${OSAX_DIR}/mavericks_presence_observer.c ${CMAKE_SOURCE_DIR}/src/mavericks_presence.c)
target_include_directories(VoodooInputMavericksPane_simbl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(VoodooInputMavericksPane_simbl PRIVATE MAVERICKS_VERSION_STR="${MAVERICKS_VERSION}")
# Own output subdir: the bundle binary is unprefixed/unsuffixed "VoodooInputMavericksPane"; keep it
# under simbl/ so the raw MODULE output never collides with the assembled .bundle path.
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
add_custom_command(TARGET VoodooInputMavericksPane_simbl POST_BUILD
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/check_gc_neutral.sh $<TARGET_FILE:VoodooInputMavericksPane_simbl>
  COMMENT "Verifying VoodooInputMavericksPane_simbl is GC-neutral (no __objc_imageinfo)"
  VERBATIM)

# 10.9 compat guard for the shipped SIMBL bundle.
mavericks_compat_guard(VoodooInputMavericksPane_simbl)

# Aggregate convenience target: the SIMBL plugin (the one shipped loader). For a release-identical
# on-device install use the `install-pkg` target, not hand-copied pieces.
add_custom_target(prefpane-refresh DEPENDS VoodooInputMavericksPane_simbl)

# Operational SIMBL-plugin install/uninstall (privilege handled here; fully reversible).
# NOTE: each step is its own COMMAND with NATIVE args (CMake quotes each arg, incl. the space-containing
# SIMBL path). Do NOT wrap in `sh -c "a && b"` — CMake space-escapes that single string and breaks the
# && / quoting (Phase-0 found this bug).
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

# Transport link-state LaunchDaemon (dev install/uninstall): the resident root daemon that keeps the MT2's
# link in sync with the current transport — reconnect keeper, USB-removal handoff, USB-appear yield,
# USB-appear hidd gesture-kick, shutdown quiesce (see the mt2_linkstated.m header). Reuses the target.
add_custom_target(linkstate-install
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_linkstated.sh install
          ${CMAKE_BINARY_DIR}/sbin/mt2_linkstated ${CMAKE_SOURCE_DIR}/dist/dev.modernmavericks.voodooinputmavericks.linkstated.plist
  COMMENT "Installing + loading the mt2_linkstated LaunchDaemon")
add_dependencies(linkstate-install mt2_linkstated)
add_custom_target(linkstate-uninstall
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_linkstated.sh uninstall
  COMMENT "Uninstalling the mt2_linkstated LaunchDaemon")

# ---- Trackpad2Updater.app (shared MavericksSparkle) --------------------------------------
# Built only when a fetched Sparkle framework path is provided (MT2_SPARKLE_FRAMEWORK); otherwise the
# rest of the tree still builds without the network fetch. The updater host now lives in
# mavericks-shared-cmake (templated main.m/Info.plist); we just configure it per product here.
if(MT2_SPARKLE_FRAMEWORK)
  include(MavericksSparkle)
  mavericks_add_updater_app(
    NAME Trackpad2Updater
    PRODUCT_NAME "Mavericks Trackpad 2"
    BUNDLE_ID dev.modernmavericks.Trackpad2Updater
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

  mavericks_compat_guard(Trackpad2Updater)

  add_custom_target(updater-install
    COMMAND sudo ${CMAKE_COMMAND} -E rm -rf "/Library/Application Support/ModernMavericks/Trackpad2Updater.app"
    COMMAND sudo ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/Trackpad2Updater.app "/Library/Application Support/ModernMavericks/Trackpad2Updater.app"
    COMMAND ${CMAKE_COMMAND} -E echo "Installed /Library/Application Support/ModernMavericks/Trackpad2Updater.app"
    COMMENT "Installing Trackpad2Updater.app")
  add_dependencies(updater-install Trackpad2Updater)
endif()

# ---- Native EdDSA signer + keygen (shared MavericksSparkle) --------------------------------------
# Built only when a fetched orlp/ed25519 src dir is provided (MT2_ED25519_SRC). The signer/keygen
# sources now live in mavericks-shared-cmake; this builds mavericks_sign_update + mavericks_generate_keys
# (into ${CMAKE_BINARY_DIR}/sbin via the project-wide CMAKE_RUNTIME_OUTPUT_DIRECTORY).
if(MT2_ED25519_SRC)
  include(MavericksSparkle)
  mavericks_add_signer(ED25519_SRC "${MT2_ED25519_SRC}")
endif()
