# Ensures the cxxbridge symlinks exist after the libloot cargo build.
#
# cargo's incremental build may skip the cxx-build build.rs when the Rust
# source hasn't changed, leaving stale or missing symlinks in target/cxxbridge/.
# This script finds the real files under target/release/build/ and creates
# the symlinks that libloot_cpp's target_include_directories expects.
#
# Invoked as a POST_BUILD step on the libloot target.

if(NOT DEFINED LIBLOOT_CXXBRIDGE_DIR)
    message(FATAL_ERROR "libloot_ensure_cxxbridge: LIBLOOT_CXXBRIDGE_DIR is not set")
endif()

if(NOT DEFINED LIBLOOT_TARGET_PATH)
    message(FATAL_ERROR "libloot_ensure_cxxbridge: LIBLOOT_TARGET_PATH is not set")
endif()

# --- rust/cxx.h ---
set(_CXX_H_LINK "${LIBLOOT_CXXBRIDGE_DIR}/rust/cxx.h")
if(NOT EXISTS "${_CXX_H_LINK}")
    # cxx-build places cxx.h in the build output alongside the bridge files.
    file(GLOB _CXX_H_CANDIDATES "${LIBLOOT_TARGET_PATH}/release/build/cxx-*/out/cxxbridge/include/rust/cxx.h")
    if(NOT _CXX_H_CANDIDATES)
        file(GLOB _CXX_H_CANDIDATES "${LIBLOOT_TARGET_PATH}/release/build/libloot-cpp-*/out/cxxbridge/include/rust/cxx.h")
    endif()
    if(_CXX_H_CANDIDATES)
        list(GET _CXX_H_CANDIDATES 0 _CXX_H_SRC)
        file(MAKE_DIRECTORY "${LIBLOOT_CXXBRIDGE_DIR}/rust")
        file(CREATE_LINK "${_CXX_H_SRC}" "${_CXX_H_LINK}" SYMBOLIC)
        message(STATUS "Created cxx.h symlink -> ${_CXX_H_SRC}")
    else()
        message(WARNING "libloot_ensure_cxxbridge: cannot locate cxx.h for symlink")
    endif()
endif()

# --- libloot-cpp/src/lib.rs.{h,cc} ---
set(_LIBRS_H_LINK "${LIBLOOT_CXXBRIDGE_DIR}/libloot-cpp/src/lib.rs.h")
set(_LIBRS_CC_LINK "${LIBLOOT_CXXBRIDGE_DIR}/libloot-cpp/src/lib.rs.cc")

if(NOT EXISTS "${_LIBRS_H_LINK}" OR NOT EXISTS "${_LIBRS_CC_LINK}")
    file(GLOB _LIBRS_H_CANDIDATES "${LIBLOOT_TARGET_PATH}/release/build/libloot-cpp-*/out/cxxbridge/include/libloot-cpp/src/lib.rs.h")
    file(GLOB _LIBRS_CC_CANDIDATES "${LIBLOOT_TARGET_PATH}/release/build/libloot-cpp-*/out/cxxbridge/sources/libloot-cpp/src/lib.rs.cc")

    if(_LIBRS_H_CANDIDATES AND _LIBRS_CC_CANDIDATES)
        list(GET _LIBRS_H_CANDIDATES 0 _LIBRS_H_SRC)
        list(GET _LIBRS_CC_CANDIDATES 0 _LIBRS_CC_SRC)
        file(MAKE_DIRECTORY "${LIBLOOT_CXXBRIDGE_DIR}/libloot-cpp/src")
        if(NOT EXISTS "${_LIBRS_H_LINK}")
            file(CREATE_LINK "${_LIBRS_H_SRC}" "${_LIBRS_H_LINK}" SYMBOLIC)
            message(STATUS "Created lib.rs.h symlink -> ${_LIBRS_H_SRC}")
        endif()
        if(NOT EXISTS "${_LIBRS_CC_LINK}")
            file(CREATE_LINK "${_LIBRS_CC_SRC}" "${_LIBRS_CC_LINK}" SYMBOLIC)
            message(STATUS "Created lib.rs.cc symlink -> ${_LIBRS_CC_SRC}")
        endif()
    else()
        message(WARNING "libloot_ensure_cxxbridge: cannot locate libloot-cpp cxxbridge outputs")
    endif()
endif()
