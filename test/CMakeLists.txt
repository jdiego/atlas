cmake_minimum_required(VERSION 3.25...4.2)

set(MODULE_NAME atlas)
set(TEST_SUITE atlas_test_suite)

project(${TEST_SUITE} LANGUAGES CXX)

# ######################################################################################################################
# Options
# ######################################################################################################################
option(TEST_USE_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(CPM_USE_LOCAL_PACKAGES "Always try to use `find_package` to get dependencies" TRUE)


include(../cmake/Utils.cmake)
include(../cmake/PackageProject.cmake)
# ######################################################################################################################
# Dependencies
# ######################################################################################################################
include(../cmake/CPM.cmake)

CPMAddPackage(
    NAME ut
    GITHUB_REPOSITORY boost-ext/ut
    VERSION  2.3.0
)

CPMAddPackage("gh:TheLartians/Format.cmake@1.8.1")

if(TEST_USE_INSTALLED_VERSION)
    find_package(${MODULE_NAME} REQUIRED)
elseif(NOT TARGET ${MODULE_NAME})
    # When invoked standalone; when used via add_subdirectory the target already exists
    CPMAddPackage(NAME ${MODULE_NAME} SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/.. FORCE ON)
endif()

# ######################################################################################################################
# Adding unit test files
# ######################################################################################################################
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/async/*.cpp)
verbose_message("Adding ${TEST_SOURCES} to ${TEST_SUITE} test suite.")

# ######################################################################################################################
# Create binary
# ######################################################################################################################
add_executable(${TEST_SUITE} ${TEST_SOURCES})
#add_dependencies(${TEST_SUITE} ${MODULE_NAME})


get_target_property(ut_include_dirs Boost::ut INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(${TEST_SUITE} PRIVATE ${ut_SOURCE_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(${TEST_SUITE} PRIVATE Boost::ut ${MODULE_NAME}::${MODULE_NAME} PkgConfig::ATLAS_LIBPQ)


target_compile_features(${TEST_SUITE} PRIVATE cxx_std_23)

# Disable Boost.UT C++20 modules (CI compilers may enable modules automatically)
target_compile_definitions(${TEST_SUITE} PRIVATE BOOST_UT_DISABLE_MODULE)

# ######################################################################################################################
# Enable compiler warnings
# ######################################################################################################################
if(NOT TEST_USE_INSTALLED_VERSION)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${MODULE_NAME} PRIVATE -Wall -Wpedantic -Wextra -Werror)
        target_compile_options(${TEST_SUITE} PRIVATE -Wall -Wpedantic -Wextra -Werror)
    elseif(MSVC)
        target_compile_options(${MODULE_NAME} PRIVATE /W4 /WX)
        target_compile_options(${TEST_SUITE} PRIVATE /W4 /WX)
    endif()
endif()
# ######################################################################################################################
# ---- Enabling Tests ----
# ######################################################################################################################
enable_testing()
# Note: doctest and similar testing frameworks can automatically configure CMake tests.
# For other testing frameworks add the tests target instead:
# add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
add_test(NAME ${TEST_SUITE} COMMAND ${TEST_SUITE})
if(TEST_USE_INSTALLED_VERSION)
    set(atlas_package_config "${atlas_DIR}/atlasConfig.cmake")
else()
    set(atlas_package_config "${CMAKE_BINARY_DIR}/${MODULE_NAME}Config.cmake")
endif()
add_test(
    NAME atlas_package_config_libpq_floor
    COMMAND
        ${CMAKE_COMMAND}
        -DATLAS_PACKAGE_CONFIG=${atlas_package_config}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_libpq_floor.cmake
)

if(NOT TEST_USE_INSTALLED_VERSION)
    add_test(
        NAME atlas_package_rejects_old_libpq
        COMMAND
            ${CMAKE_COMMAND}
            -DATLAS_BINARY_DIR=${CMAKE_BINARY_DIR}
            -DATLAS_BUILD_CONFIG=${CMAKE_BUILD_TYPE}
            -DATLAS_CONSUMER_BINARY_DIR=${CMAKE_BINARY_DIR}/package-contract/old-libpq-consumer
            -DATLAS_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/cmake/old-libpq-consumer
            -DATLAS_FAKE_PKGCONFIG_DIR=${CMAKE_CURRENT_LIST_DIR}/cmake/fake-libpq
            -DATLAS_GENERATOR=${CMAKE_GENERATOR}
            -DATLAS_PACKAGE_STAGE=${CMAKE_BINARY_DIR}/package-contract/old-libpq-stage
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/run_old_libpq_rejection.cmake
    )
    add_test(
        NAME atlas_installed_consumer_contract
        COMMAND
            ${CMAKE_COMMAND}
            -DATLAS_BINARY_DIR=${CMAKE_BINARY_DIR}
            -DATLAS_BUILD_CONFIG=${CMAKE_BUILD_TYPE}
            -DATLAS_CONSUMER_BINARY_DIR=${CMAKE_BINARY_DIR}/package-contract/installed-consumer
            -DATLAS_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}
            -DATLAS_GENERATOR=${CMAKE_GENERATOR}
            "-DATLAS_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            -DATLAS_PACKAGE_STAGE=${CMAKE_BINARY_DIR}/package-contract/installed-stage
            -DATLAS_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/..
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/run_installed_consumer.cmake
    )
    add_test(
        NAME atlas_installed_consumer_rejects_invalid_jobs
        COMMAND
            ${CMAKE_COMMAND}
            -DATLAS_CONSUMER_SCRIPT=${CMAKE_CURRENT_LIST_DIR}/cmake/run_installed_consumer.cmake
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_consumer_build_jobs.cmake
    )
    set_tests_properties(
        atlas_package_rejects_old_libpq
        atlas_installed_consumer_contract
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 180
    )
endif()

# ######################################################################################################################
# Setup code coverage if enabled
# ######################################################################################################################
if((${MODULE_NAME}_ENABLE_CODE_COVERAGE OR ENABLE_CODE_COVERAGE) AND TARGET ${MODULE_NAME})
    get_target_property(module_target_type ${MODULE_NAME} TYPE)
    set(module_compile_scope PUBLIC)
    if(module_target_type STREQUAL "INTERFACE_LIBRARY")
        set(module_compile_scope INTERFACE)
    endif()

    # ---- code coverage ----
    target_compile_options(${MODULE_NAME} ${module_compile_scope} -O0 -g -fprofile-arcs -ftest-coverage)
    target_link_options(${MODULE_NAME} ${module_compile_scope} -fprofile-arcs -ftest-coverage)
endif()


message(STATUS "Finished adding unit tests for ${TEST_SUITE}.")
