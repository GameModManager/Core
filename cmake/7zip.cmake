# Vendors 7-Zip 26.03 and builds it as a shared library.
#
# 7-Zip publishes no prebuilt shared library: every release asset is the 7zz
# CLI. The library has to be built, and the build system for it already exists
# as the `ports/7zip` CMake shim in microsoft/vcpkg, copied verbatim into
# cmake/7zip/CMakeLists.txt.
#
# The source is pinned by SHA512, so the fetch is reproducible rather than a
# hand download. Neither upstream vcpkg patch is applied: sort-asm.diff only
# fixes a MASM include path in Asm/x86/Sort.asm, and
# fix_timespec_get_broken_on_android.patch is Android-only.
#
# Licence: LGPL-2.1-or-later, with the unRAR restriction on
# CPP/7zip/Compress/Rar*. We link the compiled LGPL files and only *decompress*
# RAR, which is expressly permitted; the unRAR restriction forbids using these
# sources to build a RAR-compatible compressor. Dynamic linking is deliberate:
# static linking would additionally trigger LGPL-2.1 section 6's relinking
# obligation. See cmake/7zip/README.md.

include(FetchContent)

set(GMM_7ZIP_VERSION "26.03")

# The unRAR restriction makes the notice a condition of distribution: the
# RAR-derived code "may not be used to develop a RAR (WinRAR) compatible
# archiver" and that has to be clearly stated in the documentation. The notice
# in the About dialog and the vendored licence file below are the two places it
# lives, so assert the file is still here and still carries both terms. A bump
# that re-copies cmake/7zip/ without it fails here rather than at release time.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/7zip/LICENSE.txt" _gmm_7zip_license)
foreach(_gmm_7zip_term "GNU LGPL" "unRAR license restriction")
    string(FIND "${_gmm_7zip_license}" "${_gmm_7zip_term}" _gmm_7zip_found)
    if(_gmm_7zip_found EQUAL -1)
        message(FATAL_ERROR
            "cmake/7zip/LICENSE.txt no longer contains \"${_gmm_7zip_term}\". "
            "The LGPL text and the unRAR restriction must ship with the "
            "vendored 7-Zip sources.")
    endif()
endforeach()

FetchContent_Declare(
    sevenzip
    URL      "https://github.com/ip7z/7zip/archive/${GMM_7ZIP_VERSION}.tar.gz"
    URL_HASH "SHA512=3d205b1a8fb91a622eaf27cbf81b5eb1e7c96fd3f044779405a6794ccea644b1017e64bdc973f9629567529aa625ec846a0d0265c5ea13357d485b5d9197e4eb"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# The 7-Zip tree ships no top-level CMakeLists.txt, so populate and then add
# the vendored shim explicitly - same shape as the libloot fetch above.
FetchContent_GetProperties(sevenzip)
if(NOT sevenzip_POPULATED)
    FetchContent_Populate(sevenzip)
endif()

# The shim keys its assembly and optimiser choices off vcpkg's variables. The
# names stay vcpkg's so the copied file keeps working unmodified.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(VCPKG_TARGET_ARCHITECTURE "arm64" CACHE STRING "" FORCE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(VCPKG_TARGET_ARCHITECTURE "x64" CACHE STRING "" FORCE)
else()
    set(VCPKG_TARGET_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
endif()
set(VCPKG_TARGET_IS_LINUX TRUE CACHE BOOL "" FORCE)
# Free the assembly optimisers: without them every decoder and hash falls back to
# the portable C in C/, which is what this repo wants - it means no x86-only
# assembler, no -march/-mavx/-msse, and one source path for x86-64 and aarch64.
set(USE_NO_ASM ON CACHE BOOL "" FORCE)

# The shim lists its sources by paths relative to its own directory
# ("CPP/7zip/Compress/Rar5Decoder.cpp"), so it has to sit at the root of the
# 7-Zip tree - which is why vcpkg's portfile copies it there rather than
# add_subdirectory'ing it from the port directory. COPYONLY keeps the vendored
# file byte-identical to upstream, and configure_file only rewrites when the
# content differs, so this is idempotent across reconfigures.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/7zip/CMakeLists.txt"
               "${sevenzip_SOURCE_DIR}/CMakeLists.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/7zip/7zip-config.cmake.in"
               "${sevenzip_SOURCE_DIR}/7zip-config.cmake.in" COPYONLY)

# `add_library(7zip)` in the shim has no type, so it follows BUILD_SHARED_LIBS.
# Scoped to the subdirectory; nothing else in this build is affected.
set(BUILD_SHARED_LIBS ON)
add_subdirectory(
    "${sevenzip_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/7zip"
)

# The shim declares only INSTALL_INTERFACE include dirs, which are useless
# in-tree. 7-Zip's headers use relative includes ("../Common/MyWindows.h"), so
# the source root is the only include directory a consumer needs. BUILD_INTERFACE
# because FetchContent extracts under the build tree, and because an installed
# consumer wants the staged headers, not the source checkout.
#
# The shim's own INSTALL_INTERFACE entries are dropped rather than appended to.
# They name a relative "include" prefix, so with no install layout configured
# they evaluate to "/7zip/CPP" and "/7zip/C", and CMake refuses the target
# outright the moment something actually links it. Nothing here installs 7-Zip
# - it is linked in-tree like every other dependency in this project - so the
# build interface above is the only one that is ever consumed.
get_target_property(_gmm_7zip_includes 7zip INTERFACE_INCLUDE_DIRECTORIES)
list(FILTER _gmm_7zip_includes EXCLUDE REGEX "^\\$<INSTALL_INTERFACE:")
set_target_properties(7zip PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_gmm_7zip_includes}")
target_include_directories(7zip INTERFACE "$<BUILD_INTERFACE:${sevenzip_SOURCE_DIR}>")

# Put the library in the build tree's lib/ so the build tree mirrors the shipped
# layout - <prefix>/gamemodmanager next to <prefix>/lib/*.so - which is the one
# the Linux bundler in .github/workflows/build-linux.yml fills and
# projects/Packaging/assemble_portable.sh preserves. That mirroring is what lets
# the app's $ORIGIN/lib RUNPATH resolve both in-tree and after the binary is
# copied elsewhere with its lib/ directory, and it keeps `ldd` on the build
# binary resolving to an absolute path so the bundler still collects the library.
set_target_properties(7zip PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# The shim's own install() block installs the library to lib/ - the same
# directory the app's $ORIGIN/lib RUNPATH resolves against, and the one the
# bundler writes into. It is deliberately not repeated here: a second
# install(TARGETS) for the same target is a silent double-install. The
# sevenzip_packaging_test CTest case runs cmake --install and fails if the
# library stops landing in lib/, so a shim bump that drops the rule is caught
# there rather than by a second copy of it here.

# Same name and shape as every other dependency in this project, so consumers
# read identically on every platform (see the if(WIN32) branch in the top-level
# CMakeLists.txt, which declares the same name against a system install).
add_library(PkgConfig::SEVENZIP INTERFACE IMPORTED)
set_target_properties(PkgConfig::SEVENZIP PROPERTIES
    INTERFACE_LINK_LIBRARIES 7zip
    INTERFACE_INCLUDE_DIRECTORIES "${sevenzip_SOURCE_DIR}")
