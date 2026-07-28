if(NOT EXISTS "${ATLAS_PACKAGE_CONFIG}")
    message(FATAL_ERROR "Generated package config not found: ${ATLAS_PACKAGE_CONFIG}")
endif()

file(READ "${ATLAS_PACKAGE_CONFIG}" package_config)
string(
    FIND
    "${package_config}"
    "pkg_check_modules(ATLAS_LIBPQ REQUIRED IMPORTED_TARGET \"libpq>=18\")"
    libpq_floor
)

if(libpq_floor EQUAL -1)
    message(FATAL_ERROR "Installed package config does not require libpq>=18")
endif()
