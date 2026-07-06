# cmake/mt2_pkg.cmake — flat-component installer via pkgbuild, mirroring the old
# Makefile `pkg` target. Unsigned kext -> /usr/local/lib/mt2d (NOT /Library/Extensions,
# which enforces signing); the launchd wrapper kextloads it from there.
set(PKGROOT ${CMAKE_BINARY_DIR}/pkgroot)
# Version stamped into the pkg name + component: the single source of truth,
# MT2_VERSION (full string incl. any pre-release suffix). Still overridable via
# -DMT2_PKG_VERSION for one-off builds.
if(NOT MT2_PKG_VERSION)
  set(MT2_PKG_VERSION ${MT2_VERSION})
endif()
set(PKG_OUT ${CMAKE_BINARY_DIR}/mt2d-${MT2_PKG_VERSION}.pkg)
# Updater app: staged into the pkg only when a Sparkle build is configured.
# _UPD_PKG_STAGE expands to the copy COMMAND at configure time; empty otherwise.
if(MT2_SPARKLE_FRAMEWORK)
  set(_UPD_PKG_STAGE
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_BINARY_DIR}/MavericksTrackpad2Updater.app
      ${PKGROOT}/usr/local/lib/mt2d/MavericksTrackpad2Updater.app)
  set(_UPD_PKG_DEP MavericksTrackpad2Updater)
else()
  set(_UPD_PKG_STAGE "")
  set(_UPD_PKG_DEP "")
endif()
add_custom_target(pkg
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PKGROOT}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/lib/mt2d
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/sbin
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchDaemons
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/MT2Gesture.kext ${PKGROOT}/usr/local/lib/mt2d/MT2Gesture.kext
  ${_UPD_PKG_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_reenumerate ${PKGROOT}/usr/local/sbin/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/mt2d-run        ${PKGROOT}/usr/local/sbin/
  COMMAND chmod +x ${PKGROOT}/usr/local/sbin/mt2d-run
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2d.plist ${PKGROOT}/Library/LaunchDaemons/
  # Prefpane live-refresh: ship BOTH deliveries; the postinstall picks at install time based on the
  # TARGET machine (the build machine's SIMBL is irrelevant). If the user already has SIMBL, install
  # the SIMBL plugin and let their SIMBLAgent inject it (one less dependency to load); otherwise use
  # our standalone GC-neutral osax + the per-user launch-watcher LaunchAgent. Both are staged; only
  # one is activated (never both -> no double-swizzle).
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/ScriptingAdditions
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/MT2PaneRefresh.osax ${PKGROOT}/Library/ScriptingAdditions/MT2PaneRefresh.osax
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/libexec
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_pane_watch ${PKGROOT}/usr/local/libexec/
  # USB->BT handoff daemon (wakes BT on cable unplug, no click): its watcher binary + a root LaunchDaemon.
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_usb_bt_handoff ${PKGROOT}/usr/local/libexec/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2usbbthandoff.plist ${PKGROOT}/Library/LaunchDaemons/
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchAgents
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2panewatch.plist ${PKGROOT}/Library/LaunchAgents/
  # Scheduled background update check: per-user LaunchAgent that runs the updater in --background mode
  # (daily + at login). Only useful when the updater app is staged (Sparkle build), but the plist is a
  # static file with no build dependency, so always ship it; the postinstall loads it best-effort.
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2updatecheck.plist ${PKGROOT}/Library/LaunchAgents/
  # Stage the SIMBL plugin bundle in a holding area; postinstall copies it into the SIMBL Plugins
  # dir only if SIMBL is present on the target.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/share/mt2d
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/MT2PaneRefresh.bundle ${PKGROOT}/usr/local/share/mt2d/MT2PaneRefresh.bundle
  # Build the flat component, then wrap it with productbuild --distribution so the
  # installer enforces the 10.9.5 floor (allowed-os-versions in dist/distribution.xml).
  # A bare pkgbuild product cannot express an OS floor; productbuild can.
  COMMAND pkgbuild --root ${PKGROOT} --scripts ${CMAKE_SOURCE_DIR}/dist/scripts
          --identifier com.schmonz.mt2d --version ${MT2_PKG_VERSION} --install-location /
          ${CMAKE_BINARY_DIR}/mt2d-component.pkg
  COMMAND productbuild --distribution ${CMAKE_SOURCE_DIR}/dist/distribution.xml
          --resources ${CMAKE_SOURCE_DIR}/dist/resources
          --package-path ${CMAKE_BINARY_DIR} ${PKG_OUT}
  DEPENDS kext mt2_reenumerate MT2PaneRefresh MT2PaneRefresh_simbl mt2_pane_watch mt2_usb_bt_handoff ${_UPD_PKG_DEP}
  COMMENT "Building ${PKG_OUT} (productbuild, 10.9.5 floor)")
