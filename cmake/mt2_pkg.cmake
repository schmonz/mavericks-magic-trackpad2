# cmake/mt2_pkg.cmake — flat-component installer via pkgbuild, mirroring the old
# Makefile `pkg` target. Unsigned kext -> /usr/local/lib/mt2d (NOT /Library/Extensions,
# which enforces signing); the launchd wrapper kextloads it from there.
set(PKGROOT ${CMAKE_BINARY_DIR}/pkgroot)
set(PKG_OUT ${CMAKE_BINARY_DIR}/mt2d-${PROJECT_VERSION}.pkg)
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
  COMMAND pkgbuild --root ${PKGROOT} --scripts ${CMAKE_SOURCE_DIR}/dist/scripts
          --identifier com.schmonz.mt2d --version ${PROJECT_VERSION} --install-location / ${PKG_OUT}
  DEPENDS kext mt2_reenumerate mt2_set_btname
  COMMENT "Building ${PKG_OUT}")
