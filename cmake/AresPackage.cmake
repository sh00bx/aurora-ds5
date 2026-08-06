execute_process(COMMAND ares-package "${CPACK_TEMPORARY_DIRECTORY}" -o "${CPACK_PACKAGE_DIRECTORY}"
        -e include
        -e cmake
        -e "libmbedtls[.].*"
        -e "lib/static"
        -e "lib/pkgconfig"
        COMMAND_ERROR_IS_FATAL ANY
)

find_program(GEN_MANIFEST webosbrew-gen-manifest)
if (NOT GEN_MANIFEST)
    message(STATUS "Manifest generator not found, skipping manifest generation")
    return()
endif ()

execute_process(COMMAND ${GEN_MANIFEST} -p "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.ipk"
        -o "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_NAME}.manifest.json"
        -i "https://raw.githubusercontent.com/sh00bx/aurora-ds5/main/deploy/webos/icon.png"
        -l "https://github.com/sh00bx/aurora-ds5"
        # "optional", not "true": without root the app runs exactly like upstream
        # Aurora — ds5_acl_tx.c stays on the daemon-free hidraw path. Root (i.e. a
        # Homebrew Channel that can elevate our bundled service) is what unlocks
        # the raw-ACL transport for DualSense audio and haptics.
        -r optional
        COMMAND_ERROR_IS_FATAL ANY
)