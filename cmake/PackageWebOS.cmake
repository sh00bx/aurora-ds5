find_program(AWK NAMES gawk awk REQUIRED)

# Copy manifest
configure_file(deploy/webos/appinfo.json ./appinfo.json @ONLY)

# Copy all files under deploy/webos/ to package root
install(DIRECTORY deploy/webos/ DESTINATION . USE_SOURCE_PERMISSIONS PATTERN ".*" EXCLUDE PATTERN "*.in" EXCLUDE
        PATTERN "appinfo.json" EXCLUDE)
install(FILES "${CMAKE_BINARY_DIR}/appinfo.json" DESTINATION .)

# The DS5 raw-ACL transport daemon travels inside the IPK (deploy/webos/services/
# is picked up by ares-package and becomes usr/palm/services/<id>/), so a user
# only ever installs Aurora — the daemon used to be scp'd to /var/lib/webosbrew
# and launched from a hand-placed boot hook.
#
# Built by hand rather than with add_executable() on purpose: the goal is to
# reproduce the exact binary that has been running on the TV, and the app's own
# build type would add -g and change it. The compile runs from the source
# directory with a bare file name because gcc stores the path it was given in
# STT_FILE, and an absolute path would change the output. See
# src/daemon/ds5_txd/README.md for the reference md5.
set(DS5_TXD_DIR "${CMAKE_SOURCE_DIR}/src/daemon/ds5_txd")
set(DS5_TXD_BIN "${CMAKE_BINARY_DIR}/ds5_txd")
add_custom_command(OUTPUT "${DS5_TXD_BIN}"
        COMMAND "${CMAKE_C_COMPILER}" -O2 -Wall -Wextra ds5_txd.c -o "${DS5_TXD_BIN}" -lpthread
        WORKING_DIRECTORY "${DS5_TXD_DIR}"
        DEPENDS "${DS5_TXD_DIR}/ds5_txd.c"
        COMMENT "Building ds5_txd (DS5 raw-ACL transport daemon)"
        VERBATIM)
add_custom_target(ds5-txd ALL DEPENDS "${DS5_TXD_BIN}")
add_dependencies(moonlight ds5-txd)
install(PROGRAMS "${DS5_TXD_BIN}" DESTINATION services/com.aurora.ds5.txd)

# The game-mode script travels with the app for the same reason the daemon does:
# it needs root, and root work used to mean a hand-placed boot hook under
# /var/lib/webosbrew that polled for the app instead of being told by it. The
# app runs this through Homebrew Channel's exec at stream start and stop; see
# src/app/platform/webos/tv_game_mode.c.
install(PROGRAMS "${CMAKE_SOURCE_DIR}/tools/gamemode.sh" DESTINATION tools)

# Generate translations
foreach (I18N_LOCALE ${I18N_LOCALES})
    string(REPLACE "-" "/" I18N_JSON_DIR "resources/${I18N_LOCALE}")
    install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/${I18N_JSON_DIR}\")"
            CODE "execute_process(COMMAND ${AWK} -f scripts/webos/po2json.awk src/i18n/${I18N_LOCALE}/messages.po
                OUTPUT_FILE \"\${CMAKE_INSTALL_PREFIX}/${I18N_JSON_DIR}/cstrings.json\"
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)")
endforeach ()

# Generation gamepad mapping
install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/assets\")"
        CODE "execute_process(COMMAND scripts/webos/gen_gamecontrollerdb.sh
            OUTPUT_FILE \"\${CMAKE_INSTALL_PREFIX}/assets/gamecontrollerdb.txt\"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)")

# Fake library for cURL ABI issue
add_dependencies(moonlight commons-curl-abi-fix)
install(TARGETS commons-curl-abi-fix LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP)

set(CPACK_PACKAGE_NAME "${WEBOS_APPINFO_ID}")
set(CPACK_GENERATOR "External")
set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/AresPackage.cmake")
set(CPACK_EXTERNAL_ENABLE_STAGING TRUE)
set(CPACK_MONOLITHIC_INSTALL TRUE)
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${TARGET_WEBOS_ARCH}")
set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/CleanupNameLink.cmake")

configure_file("${CMAKE_SOURCE_DIR}/cmake/CPackConfig.webOS.cmake.in" CPackConfig.webOS.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.webOS.cmake")

if (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND CPACK_PRE_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/ArchiveDebugSymbols.cmake")
endif ()

# Will use all cores on CMake 3.20+
set(CPACK_THREADS 0)

# P6: strip the release binary — roughly halves the packaged IPK size and
# drops debug symbols not useful on-device. Debug builds keep symbols via
# CMAKE_BUILD_TYPE, this only affects the packaged (release) artifact.
set(CPACK_STRIP_FILES TRUE)

add_custom_target(webos-package-aurora COMMAND cpack DEPENDS moonlight)

if (NOT ENV{CI})
    add_custom_target(webos-verify-aurora COMMAND webosbrew-ipk-verify -S -d "${CPACK_PACKAGE_FILE_NAME}.ipk"
            WORKING_DIRECTORY ${CPACK_PACKAGE_DIRECTORY}
            DEPENDS webos-package-aurora)
    if (ENV{ARES_DEVICE})
        set(ares_arguments "-d" $ENV{ARES_DEVICE})
    endif ()
    add_custom_target(webos-install-aurora COMMAND ares-install "${CPACK_PACKAGE_FILE_NAME}.ipk" ${ares_arguments}
            WORKING_DIRECTORY ${CPACK_PACKAGE_DIRECTORY}
            DEPENDS webos-package-aurora)
    add_custom_target(webos-launch-aurora COMMAND ares-launch "${WEBOS_APPINFO_ID}" ${ares_arguments}
            DEPENDS webos-install-aurora)
endif ()

include(CPack)