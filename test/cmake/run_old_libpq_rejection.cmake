file(REMOVE_RECURSE "${ATLAS_PACKAGE_STAGE}" "${ATLAS_CONSUMER_BINARY_DIR}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --install "${ATLAS_BINARY_DIR}"
        --prefix "${ATLAS_PACKAGE_STAGE}"
        --config "${ATLAS_BUILD_CONFIG}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Atlas install failed:\n${install_output}\n${install_error}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PKG_CONFIG_PATH=${ATLAS_FAKE_PKGCONFIG_DIR}"
        "PKG_CONFIG_LIBDIR=${ATLAS_FAKE_PKGCONFIG_DIR}"
        "${CMAKE_COMMAND}"
        -S "${ATLAS_CONSUMER_SOURCE_DIR}"
        -B "${ATLAS_CONSUMER_BINARY_DIR}"
        -G "${ATLAS_GENERATOR}"
        "-DCMAKE_PREFIX_PATH=${ATLAS_PACKAGE_STAGE}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)

if(configure_result EQUAL 0)
    message(FATAL_ERROR
        "Atlas accepted libpq 17.9 when PkgConfig::LIBPQ already existed"
    )
endif()

set(configure_log "${configure_output}\n${configure_error}")
if(NOT configure_log MATCHES "libpq.*18")
    message(FATAL_ERROR
        "Consumer configuration failed for the wrong reason:\n${configure_log}"
    )
endif()
