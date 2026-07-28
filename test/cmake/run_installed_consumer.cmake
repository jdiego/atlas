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
        "${CMAKE_COMMAND}"
        -S "${ATLAS_SOURCE_DIR}/test"
        -B "${ATLAS_CONSUMER_BINARY_DIR}"
        -G "${ATLAS_GENERATOR}"
        "-DCMAKE_BUILD_TYPE=${ATLAS_BUILD_CONFIG}"
        "-DCMAKE_CXX_FLAGS=${ATLAS_PARENT_CXX_FLAGS}"
        "-DCMAKE_PREFIX_PATH=${ATLAS_PACKAGE_STAGE}"
        -DTEST_USE_INSTALLED_VERSION=ON
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Atlas consumer configuration failed:\n${configure_output}\n${configure_error}"
    )
endif()

file(
    STRINGS "${ATLAS_CONSUMER_BINARY_DIR}/CMakeCache.txt"
    consumer_cxx_flags_entry
    REGEX "^CMAKE_CXX_FLAGS:STRING="
)
string(
    REGEX REPLACE "^CMAKE_CXX_FLAGS:STRING=" ""
    consumer_cxx_flags "${consumer_cxx_flags_entry}"
)
if(NOT consumer_cxx_flags STREQUAL ATLAS_PARENT_CXX_FLAGS)
    message(FATAL_ERROR
        "Installed consumer lost parent C++ flags:"
        " expected '${ATLAS_PARENT_CXX_FLAGS}', got '${consumer_cxx_flags}'"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --build "${ATLAS_CONSUMER_BINARY_DIR}"
        --target atlas_test_suite
        --config "${ATLAS_BUILD_CONFIG}"
        -j4
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Atlas consumer build failed:\n${build_output}\n${build_error}"
    )
endif()

execute_process(
    COMMAND
        "${ATLAS_CTEST_COMMAND}"
        --test-dir "${ATLAS_CONSUMER_BINARY_DIR}"
        --output-on-failure
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error
)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Atlas consumer tests failed:\n${test_output}\n${test_error}"
    )
endif()
