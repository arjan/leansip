cmake_minimum_required(VERSION 3.22)
project(leansip VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LEANSIP_BUILD_TESTS "Build unit tests" ON)
option(LEANSIP_WERROR "Treat warnings as errors" ON)
# Which baresip leansipd runs when $LEANSIP_BARESIP is not set (see src/core/baresip_binary.h).
set(LEANSIP_EMBED_BARESIP_DIR "" CACHE PATH
    "Directory with a self-contained baresip and sounds/*.wav to embed in leansipd (static builds)")
set(LEANSIP_BARESIP_PATH "" CACHE FILEPATH "baresip to run instead of the one in PATH (e.g. Homebrew's libexec)")
set(LEANSIP_BARESIP_SOUNDS "" CACHE PATH "baresip's stock sounds for LEANSIP_BARESIP_PATH (default /usr/share/baresip)")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Widgets)
# TOML parser for leansip.toml (Debian/Ubuntu: libtomlplusplus-dev), used header-only so the
# binaries do not depend on the toml++ shared library at run time.
find_path(TOMLPLUSPLUS_INCLUDE_DIR NAMES toml++/toml.hpp toml++/toml.h REQUIRED) # .h before 3.4
qt_standard_project_setup()

add_library(leansip_warnings INTERFACE)
target_compile_options(leansip_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    $<$<BOOL:${LEANSIP_WERROR}>:-Werror>)

include(GNUInstallDirs)
add_subdirectory(src)

install(FILES packaging/leansip.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/leansip.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES README.md LICENSE docs/leansipctl.md docs/configuration.md docs/architecture.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# `cpack` in the build directory: a .deb for Debian/Ubuntu and a .tar.gz of the same files.
# Both use the system's Qt 6 and need baresip 1.1.0 or newer installed.
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_PACKAGE_NAME leansip)
set(CPACK_PACKAGE_VENDOR "Arjan Scherpenisse")
set(CPACK_PACKAGE_CONTACT "Arjan Scherpenisse <arjan@miraclethings.nl>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Keyboard-driven SIP dialer and agent CLI on top of baresip")
set(CPACK_PACKAGE_DESCRIPTION "leansip is a small Qt dialer that drives baresip. leansipd shares one baresip per profile between the dialer and leansipctl, a JSON command-line client for scripts and coding agents (calls, DTMF, recording, speech via ElevenLabs, call analysis).")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/arjan/leansip")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_FILE_NAME "leansip-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "baresip-core (>= 1.1.0)") # 1.0 has no call ids
include(CPack)

if(LEANSIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
