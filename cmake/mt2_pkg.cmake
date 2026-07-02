# cmake/mt2_pkg.cmake — flat-component installer via pkgbuild, mirroring the old
# Makefile `pkg` target. Unsigned kext -> /usr/local/lib/mt2d (NOT /Library/Extensions,
# which enforces signing); the launchd wrapper kextloads it from there.
set(PKGROOT ${CMAKE_BINARY_DIR}/pkgroot)
# Version stamped into the pkg name + component. Defaults to the project version;
# CI overrides it from the git tag with -DMT2_PKG_VERSION=<x> so the artifact is
# named mt2d-<tag>.pkg directly (no post-build rename).
if(NOT MT2_PKG_VERSION)
  set(MT2_PKG_VERSION ${PROJECT_VERSION})
endif()
set(PKG_OUT ${CMAKE_BINARY_DIR}/mt2d-${MT2_PKG_VERSION}.pkg)
add_custom_target(pkg
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PKGROOT}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/lib/mt2d
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/sbin
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchDaemons
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/MT2Gesture.kext ${PKGROOT}/usr/local/lib/mt2d/MT2Gesture.kext
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_reenumerate ${PKGROOT}/usr/local/sbin/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_set_btname  ${PKGROOT}/usr/local/sbin/
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
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchAgents
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/com.schmonz.mt2panewatch.plist ${PKGROOT}/Library/LaunchAgents/
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
          --package-path ${CMAKE_BINARY_DIR} ${PKG_OUT}
  DEPENDS kext mt2_reenumerate mt2_set_btname MT2PaneRefresh MT2PaneRefresh_simbl mt2_pane_watch
  COMMENT "Building ${PKG_OUT} (productbuild, 10.9.5 floor)")
