cmake_minimum_required(VERSION 3.25)

project(atlas_old_libpq_consumer LANGUAGES CXX)

# Reproduces the bypass: a consumer dependency may already have created this
# conventional target before Atlas is loaded.
add_library(PkgConfig::LIBPQ INTERFACE IMPORTED)

find_package(atlas CONFIG REQUIRED)
