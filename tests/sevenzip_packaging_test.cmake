# Guards the two halves of the 7-Zip backend's shipped layout: the app's RUNPATH
# and the directory the library installs into. They have to agree on lib/ for a
# binary copied off the build tree to load lib7zip.so, and neither is observable
# from a unit test of the archive code itself.
#
# Driven by tests/CMakeLists.txt, which passes GMM_BUILD_DIR.

if(NOT GMM_BUILD_DIR)
    message(FATAL_ERROR "GMM_BUILD_DIR is required")
endif()

set(_app "${GMM_BUILD_DIR}/gamemodmanager")
if(NOT EXISTS "${_app}")
    message(FATAL_ERROR "gamemodmanager is not built at ${_app}")
endif()

find_program(READELF_EXECUTABLE readelf)
if(NOT READELF_EXECUTABLE)
    message(WARNING "readelf not found - skipping the RUNPATH check")
    return()
endif()

execute_process(
    COMMAND "${READELF_EXECUTABLE}" -d "${_app}"
    OUTPUT_VARIABLE _dynamic
    RESULT_VARIABLE _readelf_result
    ERROR_VARIABLE _readelf_error
)
if(NOT _readelf_result EQUAL 0)
    message(FATAL_ERROR "readelf -d ${_app} failed: ${_readelf_error}")
endif()

# CMake emits RUNPATH; the older DT_RPATH spelling is accepted too. Exactly one
# must be present - a missing one is its own failure, so dropping the property
# cannot pass as "no absolute path found".
string(REGEX MATCHALL "Library (runpath|rpath): \\[([^]]*)\\]" _rpath_hits "${_dynamic}")
list(LENGTH _rpath_hits _rpath_count)
if(_rpath_count EQUAL 0)
    message(FATAL_ERROR
        "${_app} carries no RUNPATH, so it cannot find lib7zip.so beside "
        "itself once it is copied off the build tree.")
endif()

set(_entries)
foreach(_hit IN LISTS _rpath_hits)
    string(REGEX REPLACE "^Library (runpath|rpath): \\[(.*)\\]$" "\\2" _joined "${_hit}")
    string(REPLACE ":" ";" _split "${_joined}")
    list(APPEND _entries ${_split})
endforeach()

foreach(_entry IN LISTS _entries)
    if(IS_ABSOLUTE "${_entry}")
        message(FATAL_ERROR
            "RUNPATH entry '${_entry}' is an absolute path. It names a build "
            "directory on the machine that produced the binary, so the binary "
            "fails to start anywhere else and the builder's directory layout "
            "ships inside the artifact.")
    endif()
endforeach()

if(NOT "$ORIGIN/lib" IN_LIST _entries)
    string(REPLACE ";" " " _shown "${_entries}")
    message(FATAL_ERROR
        "RUNPATH is [${_shown}] but must contain $ORIGIN/lib - the directory the "
        "bundler fills and the one lib7zip.so installs into.")
endif()

# Install into a fresh scratch prefix under the build tree and check the library
# lands in the lib/ the RUNPATH above resolves against. The prefix has to be new
# on every run: cmake --install overwrites in place without clearing, so reusing
# one would let a lib7zip.so from an earlier green run satisfy the check after the
# install destination changed.
string(RANDOM LENGTH 10 ALPHABET 0123456789abcdef _nonce)
set(_prefix "${GMM_BUILD_DIR}/sevenzip_packaging_check/${_nonce}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${GMM_BUILD_DIR}" --prefix "${_prefix}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error
)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed: ${_install_error}")
endif()

file(GLOB _installed_lib "${_prefix}/lib/lib7zip.so*")
if(NOT _installed_lib)
    message(FATAL_ERROR
        "cmake --install produced no lib/lib7zip.so under ${_prefix}. The "
        "RUNPATH resolves against lib/, so a library installed anywhere else "
        "leaves the binary unable to load it.")
endif()
