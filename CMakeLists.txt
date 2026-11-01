cmake_minimum_required(VERSION 3.22)
project(leansip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LEANSIP_BUILD_TESTS "Build unit tests" ON)
option(LEANSIP_WERROR "Treat warnings as errors" ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Widgets)
# TOML parser for leansip.toml (Debian/Ubuntu: libtomlplusplus-dev), used header-only so the
# binaries do not depend on the toml++ shared library at run time.
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)
qt_standard_project_setup()

add_library(leansip_warnings INTERFACE)
target_compile_options(leansip_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    $<$<BOOL:${LEANSIP_WERROR}>:-Werror>)

add_subdirectory(src)

if(LEANSIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
