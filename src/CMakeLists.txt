# Core sources — one static library per subsystem, plus the app executable.
# Dependency direction: ui -> engine -> runtime -> platform (Qt lives only in
# the ui and app layers; engine/runtime/platform/cli are Qt-free).

add_subdirectory(platform)
add_subdirectory(runtime)
add_subdirectory(engine)
add_subdirectory(ui)
add_subdirectory(cli)

if(APPLE)
    add_library(gmm_swift_bridge STATIC swiftui/gmm_swift_bridge.cpp)
    target_include_directories(gmm_swift_bridge PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/swiftui)
    target_link_libraries(gmm_swift_bridge PRIVATE
        gmm_engine gmm_runtime gmm_platform
        PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::YAMLCPP PkgConfig::CURL
        PkgConfig::NLOHMANN_JSON PkgConfig::TOMLPLUSPLUS PkgConfig::ZLIB PkgConfig::LZ4
        pybind11::embed Python3::Python ${CMAKE_DL_LIBS})

    enable_language(Swift)
    add_executable(gmm_swift_app swiftui/App.swift swiftui/Bridge.swift)
    set_target_properties(gmm_swift_app PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.gamemodmanager.swiftui"
        MACOSX_BUNDLE_BUNDLE_NAME "GameModManager SwiftUI"
        SWIFT_VERSION 5.9
        AUTOMOC OFF
        SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/swiftui/gmm_swift_bridge.h")
    target_include_directories(gmm_swift_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/swiftui)
    target_compile_options(gmm_swift_app PRIVATE
        "$<$<COMPILE_LANGUAGE:Swift>:-Xcc>"
        "$<$<COMPILE_LANGUAGE:Swift>:-fmodule-map-file=${CMAKE_CURRENT_SOURCE_DIR}/swiftui/module.modulemap>")
    target_link_libraries(gmm_swift_app PRIVATE gmm_swift_bridge)
endif()

# ---------------------------------------------------------------------------
# App entry point — thin main() + wiring only
# ---------------------------------------------------------------------------
add_executable(gamemodmanager main.cpp)

# Keep the binary at <build>/gamemodmanager (documented in README.md), same
# as the pre-split layout where the app was defined at the repo root.
set_target_properties(gamemodmanager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Plugins resolve the gmm_* opaque-handle accessors (abi_bridge.cpp) through
# dlopen, so the executable must export them to its dynamic symbol table.
# Without this a plugin calling gmm_mod_name()/gmm_instance_game_id() etc.
# fails to load with "undefined symbol".
if(NOT WIN32 AND NOT APPLE)
    target_link_options(gamemodmanager PRIVATE -rdynamic)
endif()

# OS keyring (QtKeychain) — compiled in wherever the library is available
if(Qt6Keychain_FOUND)
    target_sources(gamemodmanager PRIVATE keyring/qtkeychain_keyring.cpp)
    target_compile_definitions(gamemodmanager PRIVATE GMM_HAS_QTKEYCHAIN=1)
    target_link_libraries(gamemodmanager PRIVATE Qt6Keychain::Qt6Keychain)
endif()

# Translations - source language is English.
# - qt_add_lupdate registers the standard `update_translations` target that
#   refreshes i18n/en_US.ts with lupdate.
# - qt_add_translations runs lrelease and embeds the resulting .qm under :/i18n/.
set(TS_FILES
    ${PROJECT_SOURCE_DIR}/i18n/en_US.ts
    ${PROJECT_SOURCE_DIR}/i18n/de_DE.ts
    ${PROJECT_SOURCE_DIR}/i18n/ro_RO.ts
)
qt_add_lupdate(gamemodmanager
    TS_FILES ${TS_FILES}
    SOURCE_TARGETS gamemodmanager
)
qt_add_translations(gamemodmanager
    TS_FILES ${TS_FILES}
    RESOURCE_PREFIX /i18n
)

# Link libraries — the subsystem libraries carry their own transitive deps
# (engine -> libarchive/sqlite3/yaml-cpp/curl/nlohmann_json/zlib/lz4/python;
# ui -> Qt6 + engine). The app links Qt directly for main.cpp and repeats the
# pkg-config deps explicitly so the final link line is self-documenting.
target_include_directories(gamemodmanager PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/external/abi/include
    ${PROJECT_SOURCE_DIR}/third_party/pugixml
)
target_compile_definitions(gamemodmanager PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(gamemodmanager PRIVATE
    gmm_ui gmm_cli gmm_engine gmm_runtime gmm_platform
    Qt6::Widgets Qt6::Network
    PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::YAMLCPP PkgConfig::CURL
    PkgConfig::NLOHMANN_JSON PkgConfig::TOMLPLUSPLUS PkgConfig::ZLIB PkgConfig::LZ4
    pybind11::embed Python3::Python
)
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(gamemodmanager PRIVATE KF6::SyntaxHighlighting)
endif()
if(Qt6Multimedia_FOUND)
    target_link_libraries(gamemodmanager PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
endif()
if(NOT WIN32)
    target_link_libraries(gamemodmanager PRIVATE ${CMAKE_DL_LIBS})
    if(FUSE3_FOUND)
        target_link_libraries(gamemodmanager PRIVATE PkgConfig::FUSE3)
    endif()
else()
    # Windows: link ws2_32 for network, advapi32 for registry, shell32 for
    # SHFileOperation (Recycle Bin / move-to-trash)
    target_link_libraries(gamemodmanager PRIVATE ws2_32 advapi32 shlwapi shell32)
endif()

# ---------------------------------------------------------------------------
# LD_PRELOAD intercept libraries — Linux-only, C, built as shared libs
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    add_library(gmm_overlay_intercept SHARED engine/deploy/launch/overlay_intercept.c)
    target_include_directories(gmm_overlay_intercept PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(gmm_overlay_intercept PRIVATE dl)
    set_target_properties(gmm_overlay_intercept PROPERTIES
        AUTOMOC OFF
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    # === BROKEN FEATURE — DO NOT ENABLE ===
    # libgmm_ci_intercept.so is a case-insensitive syscall interposer that is
    # broken: it shadows Wine's own case-insensitivity and broke Pandora's
    # game-tree reads (2026-08-09). It is built + unit-tested only as
    # kept-reference. do_launch never preloads it unless
    # GMM_ENABLE_BROKEN_CI_SHIM is explicitly set. Do not rely on it.
    add_library(gmm_ci_intercept SHARED engine/deploy/launch/ci_intercept.c)
    target_include_directories(gmm_ci_intercept PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(gmm_ci_intercept PRIVATE dl)
    set_target_properties(gmm_ci_intercept PROPERTIES
        AUTOMOC OFF
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
