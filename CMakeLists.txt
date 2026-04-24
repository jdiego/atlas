cmake_minimum_required(VERSION 3.25...4.2)
# ##############################################################################
# Define the Project Name and Description
# ##############################################################################
project(
    atlas
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Atlas is modern C++ client library for PostgreSQL, built on top of Boost.Asio and libpq."
)

# ##############################################################################
# Include guards: Prevent building in the source directory
# ##############################################################################
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds not allowed. "
        "Please make a new directory (called a build directory) and "
        "run CMake from there.\n"
    )
endif()
message(STATUS "Started CMake for ${PROJECT_NAME} v${PROJECT_VERSION}...\n")

include(CMakeDependentOption)
include(CheckIncludeFileCXX)
include(CheckCXXCompilerFlag)
include(CheckTypeSize)
include(CMakePrintHelpers)

option(${PROJECT_NAME}_USE_ALT_NAMES
    "Use alternative names for the project, such as naming the include directory all lowercase."
    ON
)

if(${PROJECT_NAME}_USE_ALT_NAMES)
    string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWERCASE)
    string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPERCASE)
else()
    set(PROJECT_NAME_LOWERCASE ${PROJECT_NAME})
    set(PROJECT_NAME_UPPERCASE ${PROJECT_NAME})
endif()

# ##############################################################################
# Setup project
# ##############################################################################
include(cmake/StandardSettings.cmake)
include(cmake/Utils.cmake)
include(cmake/CompilerWarnings.cmake)

##############################################################################
# Global Project Settings
# only activate for top level project
##############################################################################
##############################################################################
# Global Project Settings
# only activate for top level project
##############################################################################
if(PROJECT_IS_TOP_LEVEL)
    # Put the libraries and binaries that get built into directories at the
    # top of the build tree rather than in hard-to-find leaf directories.
    # This simplifies manual testing and the use of the build tre.
    set(MAINFOLDER ${PROJECT_SOURCE_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    # Windows DLLs are "runtime" for CMake.
    # Output them to "bin" like the Visual Studio projects do.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    # Let's nicely support folders in IDE's
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    file(CREATE_LINK
        ${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
        COPY_ON_ERROR SYMBOLIC
    )
    include(cmake/StaticAnalyzers.cmake)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Debug")
    endif()

    if(${PROJECT_NAME_UPPERCASE}_ENABLE_DOXYGEN OR ENABLE_DOXYGEN)
        include(cmake/Doxygen.cmake)
    endif()
endif()

##############################################################################
# Add dependencies
##############################################################################

# ---- Add dependencies via CPM ----
# see https://github.com/TheLartians/CPM.cmake for more info
include(cmake/CPM.cmake)

CPMAddPackage("gh:TheLartians/Format.cmake@1.8.1")
# PackageProject.cmake will be used to make our target installable
include(cmake/PackageProject.cmake)

# Collect the built libraries and include directories
# A semicolon separated list of the project's dependencies
set(${PROJECT_NAME}_DEPENDENCIES "")

CPMAddPackage(
    NAME spdlog
    GITHUB_REPOSITORY gabime/spdlog
    VERSION 1.15.0
    OPTIONS "SPDLOG_INSTALL YES" "SPDLOG_USE_STD_FORMAT YES"
)
list(APPEND ${PROJECT_NAME}_DEPENDENCIES "spdlog 1.15.0")

find_package(Boost QUIET CONFIG)
if(TARGET Boost::headers AND NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_property(TARGET Boost::boost PROPERTY
        INTERFACE_LINK_LIBRARIES Boost::headers
    )
endif()

if(NOT TARGET Boost::boost)
    find_path(Boost_INCLUDE_DIR
        NAMES boost/asio.hpp
        DOC "Boost include directory"
    )

    if(NOT Boost_INCLUDE_DIR)
        message(FATAL_ERROR
            "Boost headers were not found. Install the Boost development package "
            "or set Boost_INCLUDE_DIR/CMAKE_PREFIX_PATH to a location that "
            "contains boost/asio.hpp."
        )
    endif()

    set(Boost_INCLUDE_DIRS "${Boost_INCLUDE_DIR}")
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
    )
elseif(NOT Boost_INCLUDE_DIRS)
    get_target_property(Boost_INCLUDE_DIRS Boost::boost INTERFACE_INCLUDE_DIRECTORIES)
endif()

set(_atlas_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES ${Boost_INCLUDE_DIRS})
check_include_file_cxx(boost/asio.hpp ATLAS_HAS_BOOST_ASIO_HPP)
set(CMAKE_REQUIRED_INCLUDES "${_atlas_saved_required_includes}")
unset(_atlas_saved_required_includes)

if(NOT ATLAS_HAS_BOOST_ASIO_HPP)
    message(FATAL_ERROR
        "Boost was found, but the header 'boost/asio.hpp' is not available. "
        "Check your Boost installation and include paths."
    )
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)

if(TARGET PkgConfig::LIBPQ AND NOT TARGET PostgreSQL::PostgreSQL)
    add_library(PostgreSQL::PostgreSQL ALIAS PkgConfig::LIBPQ)
endif()



set(CMAKE_VERIFY_INTERFACE_HEADER_SETS ${PROJECT_IS_TOP_LEVEL})


# ---- Add source files ----
# Note: globbing sources is considered bad practice as CMake's generators may
# not detect new files automatically. Keep that in mind when changing files,
# or explicitly mention them here.
file(
    GLOB_RECURSE headers
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)
file(
    GLOB_RECURSE sources
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
# IDEs should put the headers in a nice place
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PREFIX "Header Files"
    FILES ${headers}
)

##############################################################################
# ---- Create library ----
##############################################################################

# #############################################################################
# Add version header: the location where the project's version header will be
# placed should match the project's regular header paths
# #############################################################################
set(VERSION_HEADER_LOCATION "${PROJECT_NAME_LOWERCASE}/version.hpp")
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/cmake/version.hpp.in
    ${VERSION_HEADER_LOCATION}
    @ONLY
)
string(TOLOWER ${PROJECT_NAME}/version.hpp VERSION_HEADER_LOCATION)

if(${PROJECT_NAME_UPPERCASE}_BUILD_HEADERS_ONLY)
    message(FATAL_ERROR
        "${PROJECT_NAME} cannot be built as a header-only library. "
        "The PostgreSQL client API uses PIMPL and links libpq as a private implementation dependency."
    )
    add_library(${PROJECT_NAME_LOWERCASE} INTERFACE)
    target_compile_features(${PROJECT_NAME_LOWERCASE} INTERFACE cxx_std_23)
    target_link_libraries(
        ${PROJECT_NAME_LOWERCASE}
        INTERFACE Boost::boost
    )

    target_sources(
        ${PROJECT_NAME_LOWERCASE}
        INTERFACE FILE_SET public_headers
        TYPE HEADERS BASE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}
        FILES
            ${headers}
            ${CMAKE_CURRENT_BINARY_DIR}/${VERSION_HEADER_LOCATION}
    )

    target_compile_options(
        ${PROJECT_NAME_LOWERCASE}
        INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->"
    )
else()
    add_library(${PROJECT_NAME_LOWERCASE})
    target_compile_features(${PROJECT_NAME_LOWERCASE} PUBLIC cxx_std_23)
    verbose_message("Added all header and implementation files.\n")

    target_sources(
        ${PROJECT_NAME_LOWERCASE}
        PRIVATE ${sources}
        PUBLIC FILE_SET public_headers
        TYPE HEADERS BASE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}
        FILES
            ${headers}
            ${CMAKE_CURRENT_BINARY_DIR}/${VERSION_HEADER_LOCATION}
    )

    target_compile_options(
        ${PROJECT_NAME_LOWERCASE}
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->"
    )

    find_package(Threads REQUIRED)
    target_link_libraries(
        ${PROJECT_NAME_LOWERCASE}
        PUBLIC
            Boost::boost
        PRIVATE
            PostgreSQL::PostgreSQL
            Threads::Threads
            spdlog::spdlog
    )
endif()

if(CMAKE_SKIP_INSTALL_RULES)
    add_library(
        ${PROJECT_NAME_LOWERCASE}::${PROJECT_NAME_LOWERCASE}
        ALIAS ${PROJECT_NAME_LOWERCASE}
    )
    return()
endif()


##############################################################################
# Add uninstall target
##############################################################################
include(cmake/AddUninstallTarget.cmake)

packageProject(
    NAME ${PROJECT_NAME}
    VERSION ${PROJECT_VERSION}
    NAMESPACE ${PROJECT_NAME}
    BINARY_DIR ${PROJECT_BINARY_DIR}
    # Install the `include` directory.
    # NOTE: should match the target's INSTALL_INTERFACE include directory
    INCLUDE_DESTINATION include/${PROJECT_NAME}-${PROJECT_VERSION}
    # (optional) create a header containing the version info
    # Note: that the path to headers should be lowercase
    VERSION_HEADER "${VERSION_HEADER_LOCATION}"
    # (optional) define the project's version compatibility,
    # defaults to `AnyNewerVersion` supported values:
    # `AnyNewerVersion|SameMajorVersion|SameMinorVersion|ExactVersion`
    COMPATIBILITY SameMajorVersion
    RUNTIME_DESTINATION /
    # semicolon separated list of the project's dependencies
    DEPENDENCIES "${${PROJECT_NAME}_DEPENDENCIES}"
    HEADER_SETS public_headers
)


##############################################################################
# Subprojects (only when this is the top-level build)
##############################################################################
if(PROJECT_IS_TOP_LEVEL)
    if(${PROJECT_NAME_UPPERCASE}_ENABLE_TESTING)
        if(${PROJECT_NAME_UPPERCASE}_BUILD_HEADERS_ONLY)
            message(STATUS "Skipping unit tests for ${PROJECT_NAME} (header-only mode).\n")
        else()
            message(STATUS "Building unit tests for ${PROJECT_NAME}.\n")
            enable_testing()
            add_subdirectory(test)
        endif()
    endif()

    if(${PROJECT_NAME_UPPERCASE}_ENABLE_STANDALONE)
        if(${PROJECT_NAME_UPPERCASE}_BUILD_HEADERS_ONLY)
            message(STATUS "Skipping standalone app for ${PROJECT_NAME} (header-only mode).\n")
        else()
            add_subdirectory(standalone)
        endif()
    endif()
endif()

# ############################################################################
# INFO
# ############################################################################
if(PROJECT_IS_TOP_LEVEL)
    print_project_configuration()
endif()
