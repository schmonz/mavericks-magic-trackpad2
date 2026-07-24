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
# pkgbuild --scripts wants ONE dir holding every install script. dist/scripts is the source of ours,
# but stage_updater.sh generates one more (agent-load.sh), so assemble the dir in the build tree
# rather than writing generated files back into the checkout.
set(PKG_SCRIPTS ${CMAKE_BINARY_DIR}/pkg-scripts)
# Updater app + its update-check LaunchAgent + the agent-load snippet our postinstall sources: all
# staged by mavericks-shared-cmake, so this product renders the same plist and runs the same load
# logic as every sibling. Only in a Sparkle build -- a build with no updater must also ship no agent,
# or login gets an agent pointing at an app that was never installed.
# The updater app + its update-check LaunchAgent + the agent-load.sh snippet the postinstall sources, all
# staged into the payload by the shared stage_updater.sh. The updater is now built UNCONDITIONALLY (see
# tools/CMakeLists.txt; mavericks_add_updater_app self-fetches Sparkle), so its staging is unconditional too.
# NB (2026-07-24): this was gated on if(MT2_SPARKLE_FRAMEWORK) -- a cache var the shared-cmake refactor
# DELETED -- so the gate was always FALSE and the pkg silently shipped with NO updater app and NO
# update-check agent. That is why 0.5.0's "Check for Updates" did nothing. check_pkg_payload.sh (run at the
# end of this target) now fails the build if the updater app is ever absent from the payload again.
set(_UPD_PKG_STAGE
  COMMAND sh ${MavericksSharedCMake_SCRIPTS}/stage_updater.sh
    --stage ${PKGROOT}
    --app ${CMAKE_BINARY_DIR}/Trackpad2Updater.app
    --app-dir "/Library/Application Support/ModernMavericks"
    --agent-label dev.modernmavericks.voodooinputmavericks.updatecheck
    --snippet-out ${PKG_SCRIPTS}/agent-load.sh)
set(_UPD_PKG_DEP Trackpad2Updater)
add_custom_target(pkg
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PKGROOT}
  # Fresh scripts dir first: _UPD_PKG_STAGE drops the generated agent-load.sh into it below.
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PKG_SCRIPTS}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/dist/scripts ${PKG_SCRIPTS}
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
  # voodooinputmavericks daemon WatchPaths this file (WatchPaths-only, no RunAtLoad); the agent touches it at login so
  # voodooinputmavericks-run runs post-session — the sole path that loads. File is world-writable so any Aqua session can
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
  # Stage the SIMBL plugin bundle in a holding area; postinstall installs it into the SIMBL Plugins dir.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PKGROOT}/usr/local/share/voodooinputmavericks
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/VoodooInputMavericksPane.bundle ${PKGROOT}/usr/local/share/voodooinputmavericks/VoodooInputMavericksPane.bundle
  # Connect/disconnect bezel OSD: our BezelServices plugin (plist only — art is copied from Apple's own
  # co-resident plugin by the postinstall). Installs to the standard scan dir; takes effect next login.
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PKGROOT}/Library/Application Support/Apple/BezelServices"
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/dist/MavericksMultitouch.plugin "${PKGROOT}/Library/Application Support/Apple/BezelServices/MavericksMultitouch.plugin"
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
          --scripts ${PKG_SCRIPTS}
          --identifier dev.modernmavericks.voodooinputmavericks --version ${MAVERICKS_PKG_VERSION} --install-location /
          ${CMAKE_BINARY_DIR}/voodooinputmavericks-component.pkg
  COMMAND productbuild --distribution ${CMAKE_SOURCE_DIR}/dist/distribution.xml
          --resources ${CMAKE_SOURCE_DIR}/dist/resources
          --package-path ${CMAKE_BINARY_DIR} ${PKG_OUT}
  # Fail-closed payload check: assert the finished .pkg actually contains every install root a working
  # product needs (updater app + update-check agent, kext, loader, pane bundle, daemons). Catches a
  # staging step that silently drops out -- e.g. the dead if(MT2_SPARKLE_FRAMEWORK) gate that shipped 0.5.0
  # with no updater. Runs on the real pkg, so it guards the local `pkg` build AND the CI release build.
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/check_pkg_payload.sh ${PKG_OUT}
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
