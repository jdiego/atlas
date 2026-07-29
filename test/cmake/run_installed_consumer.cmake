if(NOT DEFINED ATLAS_CONSUMER_BUILD_JOBS)
    set(ATLAS_CONSUMER_BUILD_JOBS 1)
endif()
if(NOT ATLAS_CONSUMER_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "ATLAS_CONSUMER_BUILD_JOBS must be a positive integer")
endif()
if(ATLAS_VALIDATE_CONSUMER_BUILD_JOBS_ONLY)
    return()
endif()

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

file(
    GLOB_RECURSE atlas_installed_target_files
    "${ATLAS_PACKAGE_STAGE}/*[Tt]argets.cmake"
)
if(NOT atlas_installed_target_files)
    message(FATAL_ERROR "Atlas install produced no CMake target export")
endif()

foreach(atlas_target_file IN LISTS atlas_installed_target_files)
    file(READ "${atlas_target_file}" atlas_target_contents)
    if(atlas_target_contents MATCHES "(^|[ ;\"])(-Werror|/WX)([ ;\"]|$)")
        message(FATAL_ERROR
            "Installed Atlas target exports maintainer warnings-as-errors: "
            "${atlas_target_file}"
        )
    endif()
endforeach()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${ATLAS_SOURCE_DIR}/test"
        -B "${ATLAS_CONSUMER_BINARY_DIR}"
        -G "${ATLAS_GENERATOR}"
        "-DCMAKE_BUILD_TYPE=${ATLAS_BUILD_CONFIG}"
        "-DCMAKE_CXX_FLAGS=${ATLAS_PARENT_CXX_FLAGS}"
        "-DCMAKE_CXX_COMPILER=${ATLAS_PARENT_CXX_COMPILER}"
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

file(
    STRINGS "${ATLAS_CONSUMER_BINARY_DIR}/CMakeCache.txt"
    consumer_cxx_compiler_entry
    REGEX "^CMAKE_CXX_COMPILER:(FILEPATH|STRING)="
)
string(
    REGEX REPLACE "^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=" ""
    consumer_cxx_compiler "${consumer_cxx_compiler_entry}"
)
file(REAL_PATH "${ATLAS_PARENT_CXX_COMPILER}" parent_cxx_compiler)
file(REAL_PATH "${consumer_cxx_compiler}" resolved_consumer_cxx_compiler)
if(NOT resolved_consumer_cxx_compiler STREQUAL parent_cxx_compiler)
    message(FATAL_ERROR
        "Installed consumer changed the C++ compiler:"
        " expected '${parent_cxx_compiler}', got '${resolved_consumer_cxx_compiler}'"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --build "${ATLAS_CONSUMER_BINARY_DIR}"
        --target atlas_test_suite
        --config "${ATLAS_BUILD_CONFIG}"
        --parallel "${ATLAS_CONSUMER_BUILD_JOBS}"
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
