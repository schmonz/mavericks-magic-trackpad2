# cmake/mt2_pkg.cmake — flat-component installer via pkgbuild, mirroring the old
# Makefile `pkg` target. Unsigned kext -> /usr/local/lib/voodooinputmavericks (NOT /Library/Extensions,
# which enforces signing); the launchd wrapper kextloads it from there.
set(PKGROOT ${CMAKE_BINARY_DIR}/pkgroot)
# Version stamped into the pkg name + component: the single source of truth,
# MAVERICKS_VERSION (full string incl. any pre-release suffix). Still overridable via
# -DMAVERICKS_PKG_VERSION for one-off builds.
if(NOT MAVERICKS_PKG_VERSION)
  set(MAVERICKS_PKG_VERSION ${MAVERICKS_VERSION})
endif()
set(PKG_OUT ${CMAKE_BINARY_DIR}/voodooinputmavericks-${MAVERICKS_PKG_VERSION}.pkg)
# Updater app: staged into the pkg only when a Sparkle build is configured.
# _UPD_PKG_STAGE expands to the copy COMMAND at configure time; empty otherwise.
if(MT2_SPARKLE_FRAMEWORK)
  set(_UPD_PKG_STAGE
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_BINARY_DIR}/MavericksTrackpad2Updater.app
      ${PKGROOT}/usr/local/lib/voodooinputmavericks/MavericksTrackpad2Updater.app)
  set(_UPD_PKG_DEP MavericksTrackpad2Updater)
else()
  set(_UPD_PKG_STAGE "")
  set(_UPD_PKG_DEP "")
endif()
add_custom_target(pkg
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PKGROOT}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/lib/voodooinputmavericks
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/sbin
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchDaemons
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/VoodooInputMavericks.kext ${PKGROOT}/usr/local/lib/voodooinputmavericks/VoodooInputMavericks.kext
  ${_UPD_PKG_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_reenumerate ${PKGROOT}/usr/local/sbin/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/voodooinputmavericks-run        ${PKGROOT}/usr/local/sbin/
  COMMAND chmod +x ${PKGROOT}/usr/local/sbin/voodooinputmavericks-run
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/dev.modernmavericks.voodooinputmavericks.plist ${PKGROOT}/Library/LaunchDaemons/
  # Login-screen basic HID: the session-start trigger + the per-user LaunchAgent that touches it. The
  # voodooinputmavericks daemon WatchPaths this file; the agent touches it at login so voodooinputmavericks-run runs post-session (its
  # /dev/console guard makes the boot RunAtLoad a no-op). File is world-writable so any Aqua session can
  # fire it (low risk: it only triggers loading OUR own driver).
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/var/voodooinputmavericks
  COMMAND ${CMAKE_COMMAND} -E touch ${PKGROOT}/usr/local/var/voodooinputmavericks/session.trigger
  COMMAND chmod 666 ${PKGROOT}/usr/local/var/voodooinputmavericks/session.trigger
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchAgents
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/dev.modernmavericks.voodooinputmavericks.session.plist ${PKGROOT}/Library/LaunchAgents/
  # Prefpane live-refresh: shipped ONLY as a SIMBL plugin (staged below). The postinstall installs it into
  # the SIMBL Plugins dir; if the target has no SIMBL it points the user at mavericksforever.com/SIMBL.pkg.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/libexec
  # USB->BT handoff daemon (wakes BT on cable unplug, no click): its watcher binary + a root LaunchDaemon.
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/sbin/mt2_linkstated ${PKGROOT}/usr/local/libexec/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/dev.modernmavericks.voodooinputmavericks.linkstated.plist ${PKGROOT}/Library/LaunchDaemons/
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/Library/LaunchAgents
  # Scheduled background update check: per-user LaunchAgent that runs the updater in --background mode
  # (daily + at login). Only useful when the updater app is staged (Sparkle build), but the plist is a
  # static file with no build dependency, so always ship it; the postinstall loads it best-effort.
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/dev.modernmavericks.voodooinputmavericks.updatecheck.plist ${PKGROOT}/Library/LaunchAgents/
  # Stage the SIMBL plugin bundle in a holding area; postinstall installs it into the SIMBL Plugins dir.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/share/voodooinputmavericks
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/VoodooInputMavericksPane.bundle ${PKGROOT}/usr/local/share/voodooinputmavericks/VoodooInputMavericksPane.bundle
  # Build the flat component, then wrap it with productbuild --distribution so the
  # installer enforces the 10.9.5 floor (allowed-os-versions in dist/distribution.xml).
  # A bare pkgbuild product cannot express an OS floor; productbuild can.
  #
  # --analyze the staged root to get the bundle component list, then flip BundleIsVersionChecked
  # off on every entry BEFORE building. Without this, PackageKit version-gates each bundle and skips
  # any whose on-disk CFBundleVersion is >= the pkg's — which silently prevented the legacy-1.0.0
  # kext from ever updating (proven 2026-07-09, see cmake/pkg_no_version_check.sh). Force-install so
  # the pkg always places its own build.
  COMMAND pkgbuild --analyze --root ${PKGROOT} ${CMAKE_BINARY_DIR}/voodooinputmavericks-components.plist
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/pkg_no_version_check.sh ${CMAKE_BINARY_DIR}/voodooinputmavericks-components.plist
  COMMAND pkgbuild --root ${PKGROOT} --component-plist ${CMAKE_BINARY_DIR}/voodooinputmavericks-components.plist
          --scripts ${CMAKE_SOURCE_DIR}/dist/scripts
          --identifier dev.modernmavericks.voodooinputmavericks --version ${MAVERICKS_PKG_VERSION} --install-location /
          ${CMAKE_BINARY_DIR}/voodooinputmavericks-component.pkg
  COMMAND productbuild --distribution ${CMAKE_SOURCE_DIR}/dist/distribution.xml
          --resources ${CMAKE_SOURCE_DIR}/dist/resources
          --package-path ${CMAKE_BINARY_DIR} ${PKG_OUT}
  DEPENDS kext mt2_reenumerate VoodooInputMavericksPane_simbl mt2_linkstated ${_UPD_PKG_DEP}
  COMMENT "Building ${PKG_OUT} (productbuild, 10.9.5 floor)")

# Dev "install exactly what a user gets": build the release .pkg and install it locally through the SAME
# installer flow (scripts, component versions, BOTH prefpane loaders) a real user runs. Use THIS during dev
# instead of hand-copying individual pieces — that divergence is what left a STALE SIMBL payload (watching
# retired classes) running on-device while the repo source was current (2026-07-20). Requires sudo (the pkg
# writes /usr/local + /Library); the target owns its own privilege, mirroring kext-load.
add_custom_target(install-pkg
  COMMAND sudo installer -pkg ${PKG_OUT} -target /
  DEPENDS pkg
  COMMENT "Installing ${PKG_OUT} to / (release-identical local install)")
