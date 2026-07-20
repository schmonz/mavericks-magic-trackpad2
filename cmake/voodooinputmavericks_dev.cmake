# cmake/mt2_dev.cmake — developer hot-reload target (ports the Makefile `reload`).
# Spans tools (the bounce binaries) + kext, so it lives at root scope after both
# subdirs are added. The multi-step shell (with a drain loop) lives in dev_reload.sh
# to keep the sequencing readable and faithful to the old recipe.
add_custom_target(reload
  COMMAND sh ${CMAKE_SOURCE_DIR}/cmake/dev_reload.sh
          ${CMAKE_BINARY_DIR}/VoodooInputMavericks.kext
          $<TARGET_FILE:mt2_bt_bounce> $<TARGET_FILE:mt2_reenumerate>
  COMMENT "Reload: unload -> drain -> load -> bounce present transport")
add_dependencies(reload kext mt2_bt_bounce mt2_reenumerate)
