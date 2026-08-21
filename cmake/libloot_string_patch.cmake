# Idempotent patch for vendored libloot 0.29.6.
#
# cpp/include/loot/exception/undefined_group_error.h uses std::string without
# including <string>. GCC's <stdexcept> pulls <string> in transitively, so
# Linux builds pass; Apple Clang's libc++ does not, and the header fails to
# compile on macOS. Insert the missing include after <string_view>.
#
# Invoked from the libloot FetchContent_Declare PATCH_COMMAND:
#   cmake -DSRC_FILE=<SOURCE_DIR>/cpp/include/loot/exception/undefined_group_error.h
#         -P cmake/libloot_string_patch.cmake

if(NOT DEFINED SRC_FILE)
    message(FATAL_ERROR "libloot_string_patch: SRC_FILE is not set")
endif()

file(READ "${SRC_FILE}" contents)

# Already patched (or fixed upstream) — nothing to do.
if(contents MATCHES "#include <string>")
    return()
endif()

string(REPLACE "#include <string_view>"
               "#include <string_view>\n#include <string>"
               contents "${contents}")
file(WRITE "${SRC_FILE}" "${contents}")

message(STATUS "Patched ${SRC_FILE}: added #include <string>")
