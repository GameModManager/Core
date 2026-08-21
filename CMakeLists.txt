cmake_minimum_required(VERSION 3.20)

# macOS deployment target — must be set before project()/enable_language() so
# every compiler invocation and toolchain probe targets the same OS version.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment target")
endif()

# Single source of truth for the app version. Every consumer reads
# PROJECT_VERSION: the VERSION compile definition (PUBLIC on gmm_engine), the
# NSIS installer (package_installer passes -DVERSION=${PROJECT_VERSION}), and
# the plugins (GMM_VERSION, visible in the add_subdirectory'd external/plugins
# scope below).
project(GameModManager
    VERSION 0.4.1
    DESCRIPTION "Cross-platform game mod manager"
    LANGUAGES CXX C
)

set(GMM_VERSION "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# Platform detection
if(WIN32)
    add_compile_definitions(GMM_PLATFORM_WINDOWS)
    set(GMM_PLATFORM "windows")
elseif(APPLE)
    add_compile_definitions(GMM_PLATFORM_MACOS)
    set(GMM_PLATFORM "macos")
else()
    add_compile_definitions(GMM_PLATFORM_LINUX)
    set(GMM_PLATFORM "linux")
endif()

# Homebrew pkg-config discovery — must run before any find_package /
# pkg_check_modules call. Keg-only formulae (libarchive, sqlite3, curl, …)
# expose their .pc files only under ${HOMEBREW_PREFIX}/opt/<formula>/, so the
# generic prefix dirs alone are not enough on a stock Homebrew install.
if(APPLE)
    execute_process(COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _HOMEBREW_RESULT)
    if(_HOMEBREW_RESULT EQUAL 0 AND HOMEBREW_PREFIX)
        file(GLOB _HOMEBREW_PC_DIRS
            LIST_DIRECTORIES true
            "${HOMEBREW_PREFIX}/lib/pkgconfig"
            "${HOMEBREW_PREFIX}/share/pkgconfig"
            "${HOMEBREW_PREFIX}/opt/*/lib/pkgconfig"
            "${HOMEBREW_PREFIX}/opt/*/share/pkgconfig")
        list(JOIN _HOMEBREW_PC_DIRS ":" _HOMEBREW_PC_PATH)
        if(_HOMEBREW_PC_PATH)
            set(ENV{PKG_CONFIG_PATH} "${_HOMEBREW_PC_PATH}:$ENV{PKG_CONFIG_PATH}")
        endif()
    endif()
    unset(_HOMEBREW_RESULT)
    unset(_HOMEBREW_PC_DIRS)
    unset(_HOMEBREW_PC_PATH)
endif()

# Qt6 — required components plus optional modules
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Test LinguistTools)

# Optional QtMultimedia (video playback in the FileViewer). The video viewer
# page is compiled only when available; the container degrades to the
# "unsupported" page otherwise.
find_package(Qt6 COMPONENTS Multimedia MultimediaWidgets QUIET)

# QtKeychain OS keyring (Secret Service / KWallet / macOS Keychain / Windows
# Credential Locker) — pulled on every platform that ships it.
find_package(Qt6Keychain QUIET)

# macOS Keychain backing frameworks — QtKeychain talks to Security (via
# CoreFoundation); link them explicitly so keyring code resolves regardless of
# how the Qt6Keychain package was built. Applies to targets defined below.
if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    if(SECURITY_FRAMEWORK AND COREFOUNDATION_FRAMEWORK)
        link_libraries(${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    endif()
endif()

# KSyntaxHighlighting (KDE) — OPTIONAL syntax highlighting for the Mod Info
# text/config editors. There is no KF6 for Windows or macOS, so when absent
# the editors degrade to plain text.
find_package(KF6SyntaxHighlighting QUIET)

# LOOT load-order sorting. gmm_lootcli links libloot, a Rust crate built from
# source via its own cpp/CMakeLists.txt (invokes cargo). Optional: when cargo
# is absent GMM_WITH_LOOT defaults OFF and the engine's LootSorter surfaces a
# "binary missing" error at runtime.
find_program(CARGO_EXECUTABLE cargo)
set(GMM_WITH_LOOT_DEFAULT OFF)
if(CARGO_EXECUTABLE)
    set(GMM_WITH_LOOT_DEFAULT ON)
endif()
option(GMM_WITH_LOOT "Build the gmm_lootcli LOOT sort subprocess (links libloot)" ${GMM_WITH_LOOT_DEFAULT})
if(GMM_WITH_LOOT AND NOT CARGO_EXECUTABLE)
    message(WARNING
        "GMM_WITH_LOOT=ON but cargo was not found on PATH. libloot is a Rust "
        "crate; building it requires a Rust toolchain. The engine will report "
        "gmm_lootcli as missing at runtime.")
endif()

# System dependencies — PkgConfig on Linux/macOS, find_path/find_library on
# Windows. Every dependency is exposed as a PkgConfig::* imported target so
# consumers see one name on every platform.
if(WIN32)
    find_path(LIBARCHIVE_INCLUDE_DIR archive.h)
    find_library(LIBARCHIVE_LIBRARY NAMES archive)
    find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
    find_library(SQLITE3_LIBRARY NAMES sqlite3)
    find_path(YAMLCPP_INCLUDE_DIR yaml-cpp/yaml.h)
    find_library(YAMLCPP_LIBRARY NAMES yaml-cpp)
    find_path(CURL_INCLUDE_DIR curl/curl.h)
    find_library(CURL_LIBRARY NAMES libcurl curllib)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
    find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp)
    find_path(ZLIB_INCLUDE_DIR zlib.h)
    find_library(ZLIB_LIBRARY NAMES z zlib)
    find_path(LZ4_INCLUDE_DIR lz4.h)
    find_library(LZ4_LIBRARY NAMES lz4)

    # Imported targets for consistency with the Linux pkg-config path
    add_library(PkgConfig::LIBARCHIVE UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::LIBARCHIVE PROPERTIES
        IMPORTED_LOCATION "${LIBARCHIVE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBARCHIVE_INCLUDE_DIR}")

    add_library(PkgConfig::SQLITE3 UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::SQLITE3 PROPERTIES
        IMPORTED_LOCATION "${SQLITE3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}")

    add_library(PkgConfig::YAMLCPP UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::YAMLCPP PROPERTIES
        IMPORTED_LOCATION "${YAMLCPP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${YAMLCPP_INCLUDE_DIR}")

    add_library(PkgConfig::CURL UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::CURL PROPERTIES
        IMPORTED_LOCATION "${CURL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")

    add_library(PkgConfig::NLOHMANN_JSON INTERFACE IMPORTED)
    set_target_properties(PkgConfig::NLOHMANN_JSON PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_DIR}")

    # toml++ is header-only by default; the INTERFACE target keeps the Windows
    # build free of a toml++ DLL (the Linux/macOS pkg-config module links the
    # distro's shared build instead).
    add_library(PkgConfig::TOMLPLUSPLUS INTERFACE IMPORTED)
    set_target_properties(PkgConfig::TOMLPLUSPLUS PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${TOMLPLUSPLUS_INCLUDE_DIR}")

    add_library(PkgConfig::ZLIB UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}")

    add_library(PkgConfig::LZ4 UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::LZ4 PROPERTIES
        IMPORTED_LOCATION "${LZ4_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(NLOHMANN_JSON REQUIRED IMPORTED_TARGET nlohmann_json)
    pkg_check_modules(TOMLPLUSPLUS REQUIRED IMPORTED_TARGET tomlplusplus)
    pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
    pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
    # fuse3 is Linux-only (macOS ships no pkg-config fuse3); every consumer
    # guards on FUSE3_FOUND and the VFS deploy strategy degrades without it.
    if(NOT APPLE)
        pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3)
    endif()
endif()

# Python embedding via pybind11
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
)
FetchContent_MakeAvailable(pybind11)

# Test suite — OFF by default. When OFF, no test executables are created and
# none are registered, so `cmake --build build` produces only the app, the
# plugins, and the LD_PRELOAD intercept libraries. build.sh passes
# -DGMM_BUILD_TESTS=ON only for its --tests mode.
option(GMM_BUILD_TESTS "Build and register the test suite" OFF)
if(GMM_BUILD_TESTS)
    enable_testing()

    # Catch2 v3 test framework — fetched only when the test suite is enabled
    # (same FetchContent setup as the Thirdparty repo). Catch2WithMain provides
    # the runner main(); the Catch module's catch_discover_tests() registers
    # every TEST_CASE as its own CTest test.
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.9.1
    )
    FetchContent_MakeAvailable(Catch2)
    include(Catch)
endif()

# Build game plugins as shared libraries (loaded via dlopen at runtime)
add_subdirectory(external/plugins)

# Core sources — per-directory libraries (engine, runtime, platform, ui, cli)
# plus the gamemodmanager app executable (src/CMakeLists.txt)
add_subdirectory(src)

# Test suite
if(GMM_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# LOOT sort subprocess (gmm_lootcli) — plain C++20, no Qt. Links libloot,
# whose C++ wrapper is a Rust crate built from source (its own cpp/CMakeLists.txt
# invokes `cargo build`; Rust is build-time only). 0.29.6 ships no root
# CMakeLists.txt, so populate and add the cpp/ subdir directly. The engine
# LootSorter is compiled regardless of this option and reports the missing
# binary as a clear error.
if(GMM_WITH_LOOT)
    # 0.29.6's undefined_group_error.h uses std::string without including
    # <string>; GCC hides this (transitive include), Apple Clang does not.
    # cmake/libloot_string_patch.cmake inserts it — idempotent, and portable
    # where sed -i is not (macOS vs GNU syntax).
    FetchContent_Declare(
        libloot
        GIT_REPOSITORY https://github.com/loot/libloot.git
        GIT_TAG        0.29.6
        PATCH_COMMAND
            ${CMAKE_COMMAND}
            -DSRC_FILE=<SOURCE_DIR>/cpp/include/loot/exception/undefined_group_error.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libloot_string_patch.cmake
    )
    FetchContent_Populate(libloot)
    set(LIBLOOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBLOOT_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    # libloot is populated into the build tree (_deps/libloot-src), so its
    # cpp/ subdir is out-of-tree relative to this source dir — the binary
    # directory must be given explicitly.
    add_subdirectory(${libloot_SOURCE_DIR}/cpp ${libloot_BINARY_DIR}/cpp)

    add_executable(gmm_lootcli
        tools/gmm_lootcli/src/main.cpp
    )
    target_include_directories(gmm_lootcli PRIVATE tools/gmm_lootcli/src)
    target_link_libraries(gmm_lootcli PRIVATE libloot)
endif()

# -- Windows installer -------------------------------------------------------
# `package_installer` builds the NSIS installer from packaging/windows/installer.nsi
# with the version injected from this CMakeLists' project(VERSION ...) — the
# installer never needs a manual version edit.
if(WIN32)
    find_program(MAKENSIS_EXECUTABLE makensis)
    if(MAKENSIS_EXECUTABLE)
        add_custom_target(package_installer
            COMMAND ${MAKENSIS_EXECUTABLE} -DVERSION=${PROJECT_VERSION} installer.nsi
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows
            COMMENT "Building GameModManager-${PROJECT_VERSION}-Setup.exe"
            VERBATIM
        )
    endif()
endif()
