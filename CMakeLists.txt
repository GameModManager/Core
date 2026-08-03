cmake_minimum_required(VERSION 3.20)
project(GameModManager
    VERSION 0.1.0
    DESCRIPTION "Cross-platform game mod manager"
    LANGUAGES CXX C
)

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

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Test LinguistTools)

# QtKeychain OS keyring (Secret Service / KWallet / macOS Keychain / Windows
# Credential Locker) — pulled on every platform that ships it.
find_package(Qt6Keychain QUIET)

# Dependencies — PkgConfig on Linux/macOS, find_package on Windows
if(WIN32)
    # Windows: use find_package for each dependency
    # (exact package names depend on how deps are installed — vcpkg, manual, etc.)
    find_path(LIBARCHIVE_INCLUDE_DIR archive.h)
    find_library(LIBARCHIVE_LIBRARY NAMES archive)
    find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
    find_library(SQLITE3_LIBRARY NAMES sqlite3)
    find_path(YAMLCPP_INCLUDE_DIR yaml-cpp/yaml.h)
    find_library(YAMLCPP_LIBRARY NAMES yaml-cpp)
    find_path(CURL_INCLUDE_DIR curl/curl.h)
    find_library(CURL_LIBRARY NAMES libcurl curllib)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)

    # Create imported targets for consistency with Linux path
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
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(NLOHMANN_JSON REQUIRED IMPORTED_TARGET nlohmann_json)
    pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3)
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

enable_testing()

# Build game plugins as shared libraries (loaded via dlopen at runtime)
add_subdirectory(external/plugins)

# FOMOD engine sources (pugixml-backed ModuleConfig parser, condition tester,
# view-model state machine, staging-dir file installer). Shared by the app and
# the engine tests that drive FomodStage directly.
set(FOMOD_ENGINE_SOURCES
    src/engine/fomod/module_config.cpp
    src/engine/fomod/condition_tester.cpp
    src/engine/fomod/fomod_view_model.cpp
    src/engine/fomod/file_installer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/pugixml.cpp
)

# Engine sources — no Qt includes allowed here
set(ENGINE_SOURCES
    src/engine/instance/instance.cpp
    src/engine/instance/instance_utils.cpp
    src/engine/fs_utils.cpp
    src/engine/detect/game_detector.cpp
    src/engine/detect/mod_scanner.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/index/conflict_index.cpp
    src/engine/index/isaac_conflict_index.cpp
    src/engine/index/conflict_engine.cpp
    src/engine/model/profile.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/deploy/strategy_symlink.cpp
    src/engine/deploy/strategy_hardlink.cpp
    src/engine/deploy/strategy_junction.cpp
    src/engine/deploy/strategy_overlayfs_deploy.cpp
    src/engine/deploy/deploy_ledger.cpp
    src/engine/deploy/deploy_utils.cpp
    src/engine/overlay_launcher.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/pipeline/fetch_stage.cpp
    src/engine/pipeline/extract_stage.cpp
    src/engine/pipeline/fomod_stage.cpp
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/pipeline/plugin_claim_stage.cpp
    src/engine/trace/trace_recorder.cpp
    src/engine/plugin_host/abi_bridge.cpp
    src/engine/notify/notification_backend.cpp
    src/engine/log/logger.cpp
    src/engine/log/crash_handler.cpp
    src/engine/cache/mod_cache.cpp
    src/engine/plugin_host/plugin_loader.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/registry/game_capabilities.cpp
    src/engine/registry/game_knowledge.cpp
    src/engine/nxm/nxm_router.cpp
    src/engine/nxm/managed_games.cpp
    src/engine/nxm/nxm_ipc.cpp
    src/engine/sort/abi_sort_provider.cpp
    src/engine/sort/sort_registry.cpp
    src/engine/workshop/remote_cache.cpp
    src/engine/workshop/workshop_client.cpp
    src/engine/theme/theme_manager.cpp
    src/engine/theme/style_manager.cpp
    src/engine/tools/external_tool.cpp
    src/engine/plugin_host/python_loader.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/overwrite/overwrite_utils.cpp
    src/engine/plugins/esp_header.cpp
    src/engine/plugins/plugin_database.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/nexus_provider.cpp
    src/engine/source/nexus_account.cpp
    src/engine/source/nexus_http.cpp
    src/engine/source/nexus_servers.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/nexus_auth.cpp
    src/engine/keyring.cpp
    src/engine/preload_interceptor.cpp
    src/engine/launcher.cpp
    src/cli/headless_launcher.cpp
    src/engine/single_instance.cpp
    ${FOMOD_ENGINE_SOURCES}
)

# FUSE/VFS strategies — Linux only
if(NOT WIN32 AND FUSE3_FOUND)
    list(APPEND ENGINE_SOURCES
        src/engine/deploy/strategy_vfs.cpp
        src/engine/deploy/strategy_overlayfs.cpp
    )
endif()

# Platform module — compile only the current OS's implementation
set(PLATFORM_SOURCES
    src/platform/${GMM_PLATFORM}/${GMM_PLATFORM}_platform.cpp
)

# Runtime — Qt-free
set(RUNTIME_SOURCES
    src/runtime/runtime.cpp
    src/runtime/wine_runtime.cpp
)

# UI sources — Qt lives here
set(UI_SOURCES
    src/ui/settings/settings.cpp
    src/ui/settings/settings_dialog.cpp
    src/ui/settings/source_pages.cpp
    src/ui/main_window/main_window.cpp
    src/ui/widgets/mod_list_model.cpp
    src/ui/widgets/mod_table_view.cpp
    src/ui/widgets/main_toolbar.cpp
    src/ui/widgets/profile_bar.cpp
    src/ui/widgets/right_panel.cpp
    src/ui/widgets/exec_controls_bar.cpp
    src/ui/widgets/exec_entry_dialog.cpp
    src/ui/widgets/console_panel.cpp
    src/ui/widgets/gmm_status_bar.cpp
    src/ui/widgets/instance_switcher_dialog.cpp
    src/ui/widgets/instance_statistics_dialog.cpp
    src/ui/widgets/mod_info_dialog.cpp
    src/ui/widgets/debug_window.cpp
    src/ui/widgets/pipeline_window.cpp
    src/ui/widgets/zoom_controls.cpp
    src/ui/widgets/mod_filter_bar.cpp
    src/ui/widgets/right_filter_bar.cpp
    src/ui/widgets/column_toggle_header.cpp
    src/ui/widgets/menu_bar.cpp
    src/ui/panels/tab_panels.cpp
    src/ui/pipeline_worker.cpp
    src/ui/preview/anm2_parser.cpp
    src/ui/preview/preview_widget.cpp
    src/ui/preview/preview_window.cpp
    src/ui/game_selection/game_card.cpp
    src/ui/game_selection/game_selection_widget.cpp
    src/ui/overwrite/sync_overwrite_dialog.cpp
    src/ui/overwrite/move_to_mod_dialog.cpp
    src/ui/overwrite/overwrite_info_dialog.cpp
    src/ui/overwrite/query_overwrite_dialog.cpp
    src/ui/fomod/fomod_image_viewer.cpp
    src/ui/fomod/fomod_wizard_dialog.cpp
)

# App entry point
set(APP_SOURCES
    src/main.cpp
)

add_executable(gamemodmanager ${ENGINE_SOURCES} ${RUNTIME_SOURCES} ${PLATFORM_SOURCES} ${UI_SOURCES} ${APP_SOURCES})

# OS keyring (QtKeychain) — compiled in wherever the library is available
if(Qt6Keychain_FOUND)
    target_sources(gamemodmanager PRIVATE src/keyring/qtkeychain_keyring.cpp)
    target_compile_definitions(gamemodmanager PRIVATE GMM_HAS_QTKEYCHAIN=1)
    target_link_libraries(gamemodmanager PRIVATE Qt6Keychain::Qt6Keychain)
endif()

# Translations - source language is English.
# - qt_add_lupdate registers the standard `update_translations` target that
#   refreshes i18n/en_US.ts with lupdate.
# - qt_add_translations runs lrelease and embeds the resulting .qm under :/i18n/.
set(TS_FILES
    i18n/en_US.ts
    i18n/de_DE.ts
    i18n/ro_RO.ts
)
qt_add_lupdate(gamemodmanager
    TS_FILES ${TS_FILES}
    SOURCE_TARGETS gamemodmanager
)
qt_add_translations(gamemodmanager
    TS_FILES ${TS_FILES}
    RESOURCE_PREFIX /i18n
)

# Link libraries — platform-specific
target_link_libraries(gamemodmanager PRIVATE Qt6::Widgets Qt6::Network PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::YAMLCPP PkgConfig::CURL PkgConfig::NLOHMANN_JSON pybind11::embed Python3::Python)
if(NOT WIN32)
    target_link_libraries(gamemodmanager PRIVATE ${CMAKE_DL_LIBS})
    if(FUSE3_FOUND)
        target_link_libraries(gamemodmanager PRIVATE PkgConfig::FUSE3)
        target_compile_definitions(gamemodmanager PRIVATE HAS_FUSE3=1)
    endif()
else()
    # Windows: link ws2_32 for network, advapi32 for registry, shell32 for
    # SHFileOperation (Recycle Bin / move-to-trash)
    target_link_libraries(gamemodmanager PRIVATE ws2_32 advapi32 shlwapi shell32)
endif()

target_include_directories(gamemodmanager PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
target_compile_definitions(gamemodmanager PRIVATE VERSION="${PROJECT_VERSION}")

# LD_PRELOAD intercept library — Linux-only, C, built as a shared lib
if(UNIX AND NOT APPLE)
    add_library(gmm_overlay_intercept SHARED src/engine/overlay_intercept.c)
    target_link_libraries(gmm_overlay_intercept PRIVATE dl)
    set_target_properties(gmm_overlay_intercept PROPERTIES
        AUTOMOC OFF
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    # Smoke test: verify all 17 function symbols resolve in the .so
    add_executable(overlay_intercept_test tests/engine/overlay_intercept_test.c)
    target_compile_definitions(overlay_intercept_test PRIVATE
        GMM_INTERCEPT_SO="${CMAKE_CURRENT_BINARY_DIR}/libgmm_overlay_intercept.so")
    target_link_libraries(overlay_intercept_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME overlay_intercept_test COMMAND overlay_intercept_test)
endif()

# Tests — headless, no Qt
add_executable(pipeline_test tests/engine/pipeline_test.cpp
    src/engine/instance/instance.cpp
    src/engine/detect/game_detector.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/deploy/strategy_symlink.cpp
    src/engine/deploy/strategy_hardlink.cpp
    src/engine/deploy/strategy_junction.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/pipeline/fetch_stage.cpp
    src/engine/pipeline/extract_stage.cpp
    src/engine/pipeline/fomod_stage.cpp
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/trace/trace_recorder.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/workshop/workshop_client.cpp
    ${FOMOD_ENGINE_SOURCES}
)
target_include_directories(pipeline_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
target_link_libraries(pipeline_test PRIVATE PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::CURL PkgConfig::NLOHMANN_JSON)
if(WIN32)
    target_link_libraries(pipeline_test PRIVATE advapi32)
endif()
add_test(NAME pipeline_test COMMAND pipeline_test)

add_executable(plugin_database_test tests/engine/plugin_database_test.cpp
    src/engine/plugins/esp_header.cpp
    src/engine/plugins/plugin_database.cpp
    src/engine/instance/instance.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/registry/game_knowledge.cpp
)
target_include_directories(plugin_database_test PRIVATE src/)
target_link_libraries(plugin_database_test PRIVATE PkgConfig::NLOHMANN_JSON)
add_test(NAME plugin_database_test COMMAND plugin_database_test)

add_executable(logger_test tests/engine/logger_test.cpp
    src/engine/log/logger.cpp
)
target_include_directories(logger_test PRIVATE src/)
add_test(NAME logger_test COMMAND logger_test)

add_executable(phase04_test tests/engine/phase04_test.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
    src/engine/deploy/order_hook.cpp
)
target_include_directories(phase04_test PRIVATE src/)
add_test(NAME phase04_test COMMAND phase04_test)

add_executable(trace_test tests/engine/trace_test.cpp
    src/engine/trace/trace_recorder.cpp
)
target_include_directories(trace_test PRIVATE src/)
add_test(NAME trace_test COMMAND trace_test)

add_executable(registry_test tests/engine/registry_test.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/instance/instance.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/pipeline/fetch_stage.cpp
    src/engine/pipeline/extract_stage.cpp
    src/engine/pipeline/fomod_stage.cpp
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/trace/trace_recorder.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/workshop/workshop_client.cpp
    ${FOMOD_ENGINE_SOURCES}
)
target_include_directories(registry_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
target_link_libraries(registry_test PRIVATE PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::CURL PkgConfig::NLOHMANN_JSON)
add_test(NAME registry_test COMMAND registry_test)

add_executable(fomod_module_config_test tests/engine/fomod_module_config_test.cpp
    src/engine/log/logger.cpp
    ${FOMOD_ENGINE_SOURCES}
)
target_include_directories(fomod_module_config_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
add_test(NAME fomod_module_config_test COMMAND fomod_module_config_test)

add_executable(fomod_condition_test tests/engine/fomod_condition_test.cpp
    src/engine/log/logger.cpp
    ${FOMOD_ENGINE_SOURCES}
)
target_include_directories(fomod_condition_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
add_test(NAME fomod_condition_test COMMAND fomod_condition_test)

add_executable(fomod_installer_test tests/engine/fomod_installer_test.cpp
    src/engine/log/logger.cpp
    ${FOMOD_ENGINE_SOURCES}
)
target_include_directories(fomod_installer_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
target_link_libraries(fomod_installer_test PRIVATE PkgConfig::NLOHMANN_JSON)
add_test(NAME fomod_installer_test COMMAND fomod_installer_test)

add_executable(instance_path_test tests/engine/instance_path_test.cpp
    src/engine/instance/instance.cpp
)
target_include_directories(instance_path_test PRIVATE src/)
add_test(NAME instance_path_test COMMAND instance_path_test)

# Regression: GUI + CLI launches must go through prepare_launch_params, which
# deploys enabled mods to .gmm_staging and adds it to extra_lowerdirs.
add_executable(launch_params_test tests/engine/launch_params_test.cpp
    src/engine/instance/instance_utils.cpp
    src/engine/instance/instance.cpp
    src/engine/deploy/deploy_utils.cpp
    src/engine/deploy/strategy_overlayfs_deploy.cpp
    src/engine/registry/game_knowledge.cpp
    src/engine/overlay_launcher.cpp
    src/engine/fs_utils.cpp
    src/engine/log/logger.cpp
)
target_include_directories(launch_params_test PRIVATE src/)
add_test(NAME launch_params_test COMMAND launch_params_test)

add_executable(nxm_router_test tests/engine/nxm_router_test.cpp
    src/engine/nxm/nxm_router.cpp
)
target_include_directories(nxm_router_test PRIVATE src/)
add_test(NAME nxm_router_test COMMAND nxm_router_test)

# ModScanner separator/meta scanning — MO2-parity: display name = folder minus
# suffix, color from meta.ini [General] color (empty when unset), Qt-free.
add_executable(scanner_test tests/engine/scanner_test.cpp
    src/engine/detect/mod_scanner.cpp
    src/engine/registry/game_knowledge.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/log/logger.cpp
)
target_include_directories(scanner_test PRIVATE src/)
add_test(NAME scanner_test COMMAND scanner_test)

# Theme engine — Qt-free scan/tokens/discovery.
add_executable(theme_test tests/engine/theme_test.cpp
    src/engine/theme/theme_manager.cpp
    src/engine/log/logger.cpp
)
target_include_directories(theme_test PRIVATE src/)
add_test(NAME theme_test COMMAND theme_test)

# FileKeyring + NexusAuth keyring fallback/migration — pure C++, no Qt
add_executable(keyring_test tests/engine/keyring_test.cpp
    src/engine/keyring.cpp
    src/engine/nexus_auth.cpp
    src/engine/log/logger.cpp
)
target_include_directories(keyring_test PRIVATE src/)
add_test(NAME keyring_test COMMAND keyring_test)

# Live OS keyring roundtrip — only when QtKeychain is available; skips
# cleanly when no keyring daemon is running.
if(Qt6Keychain_FOUND)
    add_executable(qtkeychain_test tests/engine/qtkeychain_test.cpp
        src/engine/keyring.cpp
        src/engine/nexus_auth.cpp
        src/engine/log/logger.cpp
        src/keyring/qtkeychain_keyring.cpp
    )
    target_include_directories(qtkeychain_test PRIVATE src/)
    target_link_libraries(qtkeychain_test PRIVATE Qt6Keychain::Qt6Keychain Qt6::Core)
    add_test(NAME qtkeychain_test COMMAND qtkeychain_test)
endif()

if(GMM_PLATFORM STREQUAL "linux")
    add_executable(mimeapps_test tests/engine/mimeapps_test.cpp
        src/platform/linux/linux_platform.cpp
    )
    target_include_directories(mimeapps_test PRIVATE src/)
    add_test(NAME mimeapps_test COMMAND mimeapps_test)
endif()

add_executable(perf_benchmark tests/perf/benchmark_test.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
    src/engine/instance/instance.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/deploy/deploy_ledger.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/pipeline/fetch_stage.cpp
    src/engine/pipeline/extract_stage.cpp
    src/engine/pipeline/fomod_stage.cpp
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/trace/trace_recorder.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/workshop/workshop_client.cpp
    ${FOMOD_ENGINE_SOURCES}
)
target_include_directories(perf_benchmark PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)
target_link_libraries(perf_benchmark PRIVATE PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::CURL PkgConfig::NLOHMANN_JSON)
target_compile_definitions(perf_benchmark PRIVATE VERSION="${PROJECT_VERSION}")
add_test(NAME perf_benchmark COMMAND perf_benchmark)

# Python plugin test — requires pybind11 embed + Python
add_executable(python_plugin_test tests/engine/python_plugin_test.cpp
    src/engine/plugin_host/plugin_loader.cpp
    src/engine/plugin_host/python_loader.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/registry/game_capabilities.cpp
    src/engine/registry/game_knowledge.cpp
    src/engine/tools/external_tool.cpp
    src/engine/sort/sort_registry.cpp
    src/engine/sort/abi_sort_provider.cpp
    src/engine/log/logger.cpp
)
target_include_directories(python_plugin_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_link_libraries(python_plugin_test PRIVATE pybind11::embed Python3::Python PkgConfig::SQLITE3)
if(NOT WIN32)
    target_link_libraries(python_plugin_test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME python_plugin_test COMMAND python_plugin_test)

add_executable(conflict_test tests/engine/conflict_test.cpp
    src/engine/index/conflict_engine.cpp
    src/engine/log/logger.cpp
)
target_include_directories(conflict_test PRIVATE src/)
target_link_libraries(conflict_test PRIVATE PkgConfig::NLOHMANN_JSON)
add_test(NAME conflict_test COMMAND conflict_test)

# Overwrite workflow helpers (path mapping, move/sync, clear, sync plan).
add_executable(overwrite_utils_test tests/engine/overwrite_utils_test.cpp
    src/engine/overwrite/overwrite_utils.cpp
    src/engine/index/conflict_engine.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/fs_utils.cpp
    src/engine/log/logger.cpp
)
target_include_directories(overwrite_utils_test PRIVATE src/)
target_link_libraries(overwrite_utils_test PRIVATE PkgConfig::NLOHMANN_JSON)
add_test(NAME overwrite_utils_test COMMAND overwrite_utils_test)

# Windows-native path resolution (engine::resolve_path) + the shared string/path
# helpers (normalize_separators, name_matches_ci, find_file_ci) - the regression
# home for backslash/case handling across the FOMOD pipeline and archive
# extraction.
add_executable(fs_utils_test tests/engine/fs_utils_test.cpp
    src/engine/fs_utils.cpp
    src/engine/log/logger.cpp
)
target_include_directories(fs_utils_test PRIVATE src/)
add_test(NAME fs_utils_test COMMAND fs_utils_test)

# Hidden-file contract: is_hidden_file/hide_file/unhide_file (.gmmhidden +
# .mohidden) and deploy_all_enabled_mods skipping hidden files on both suffixes.
add_executable(deploy_hidden_test tests/engine/deploy_hidden_test.cpp
    src/engine/deploy/deploy_utils.cpp
    src/engine/deploy/strategy_overlayfs_deploy.cpp
    src/engine/fs_utils.cpp
    src/engine/log/logger.cpp
)
target_include_directories(deploy_hidden_test PRIVATE src/)
add_test(NAME deploy_hidden_test COMMAND deploy_hidden_test)

# URL percent-encoding for libcurl (Nexus CDN URLs embed unencoded filenames).
add_executable(nexus_http_test tests/engine/nexus_http_test.cpp
    src/engine/source/nexus_http.cpp
)
target_include_directories(nexus_http_test PRIVATE src/)
target_link_libraries(nexus_http_test PRIVATE PkgConfig::CURL)
add_test(NAME nexus_http_test COMMAND nexus_http_test)

# Settings -> Plugins tab — offscreen GUI regression test. Exercises the
# currentItemChanged -> rebuild_info crash path, register_category grouping,
# the filter, Enabled toggles, and the missing Path/Steam App ID rows.
add_executable(settings_plugins_tab_test tests/ui/settings_plugins_tab_test.cpp
    src/ui/settings/settings.cpp
    src/ui/settings/settings_dialog.cpp
    src/ui/settings/source_pages.cpp
    src/engine/log/logger.cpp
    src/engine/trace/trace_recorder.cpp
    src/engine/theme/theme_manager.cpp
    src/engine/theme/style_manager.cpp
    src/engine/plugin_host/plugin_loader.cpp
    src/engine/plugin_host/python_loader.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/registry/game_capabilities.cpp
    src/engine/registry/game_knowledge.cpp
    src/engine/tools/external_tool.cpp
    src/engine/sort/sort_registry.cpp
    src/engine/sort/abi_sort_provider.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/nexus_provider.cpp
    src/engine/source/nexus_account.cpp
    src/engine/source/nexus_http.cpp
    src/engine/source/nexus_servers.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/nexus_auth.cpp
    src/engine/keyring.cpp
    src/engine/workshop/workshop_client.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/instance/instance.cpp
    src/engine/instance/instance_utils.cpp
    src/engine/deploy/deploy_utils.cpp
    src/engine/deploy/strategy_symlink.cpp
    src/engine/deploy/strategy_hardlink.cpp
    src/engine/deploy/strategy_junction.cpp
    src/engine/deploy/strategy_overlayfs_deploy.cpp
    src/engine/deploy/deploy_ledger.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/launcher.cpp
    src/engine/overlay_launcher.cpp
    src/engine/preload_interceptor.cpp
    src/engine/fs_utils.cpp
    src/engine/single_instance.cpp
    src/runtime/runtime.cpp
    src/runtime/wine_runtime.cpp
    src/platform/${GMM_PLATFORM}/${GMM_PLATFORM}_platform.cpp
)
target_include_directories(settings_plugins_tab_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(settings_plugins_tab_test PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(settings_plugins_tab_test PRIVATE Qt6::Widgets Qt6::Network
    PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::YAMLCPP PkgConfig::CURL PkgConfig::NLOHMANN_JSON
    pybind11::embed Python3::Python)
if(NOT WIN32)
    target_link_libraries(settings_plugins_tab_test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME settings_plugins_tab_test COMMAND settings_plugins_tab_test ${CMAKE_CURRENT_BINARY_DIR}/plugins)
set_tests_properties(settings_plugins_tab_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# StyleManager palette-change re-apply — offscreen GUI regression test. The
# global app stylesheet (installed even for the Default/system theme) renders
# widgets via QStyleSheetStyle, which resolves palette(...) at polish time and
# freezes the UI against system Light<->Dark switches; StyleManager must
# re-apply the active sheet on QApplication::paletteChanged.
add_executable(style_manager_palette_test tests/ui/style_manager_palette_test.cpp
    src/engine/theme/theme_manager.cpp
    src/engine/theme/style_manager.cpp
    src/engine/log/logger.cpp
)
target_include_directories(style_manager_palette_test PRIVATE src/)
target_link_libraries(style_manager_palette_test PRIVATE Qt6::Widgets)
add_test(NAME style_manager_palette_test COMMAND style_manager_palette_test)
set_tests_properties(style_manager_palette_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Settings -> Sources tab — offscreen GUI regression test. Exercises the
# nested per-source sub-tabs and the MO2-parity Nexus panel (account/stats
# labels, connect/manual/disconnect buttons, options, known/preferred server
# drag&drop lists), plus the engine server-registry and NexusAuth user-info
# persistence. Hermetic: no network calls are made, XDG_CONFIG_HOME is a
# throwaway dir.
add_executable(settings_sources_tab_test tests/ui/settings_sources_tab_test.cpp
    src/ui/settings/settings.cpp
    src/ui/settings/settings_dialog.cpp
    src/ui/settings/source_pages.cpp
    src/engine/log/logger.cpp
    src/engine/trace/trace_recorder.cpp
    src/engine/theme/theme_manager.cpp
    src/engine/theme/style_manager.cpp
    src/engine/plugin_host/plugin_loader.cpp
    src/engine/plugin_host/python_loader.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/registry/game_capabilities.cpp
    src/engine/registry/game_knowledge.cpp
    src/engine/tools/external_tool.cpp
    src/engine/sort/sort_registry.cpp
    src/engine/sort/abi_sort_provider.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/nexus_provider.cpp
    src/engine/source/nexus_account.cpp
    src/engine/source/nexus_http.cpp
    src/engine/source/nexus_servers.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/nexus_auth.cpp
    src/engine/keyring.cpp
    src/engine/workshop/workshop_client.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/instance/instance.cpp
    src/engine/instance/instance_utils.cpp
    src/engine/deploy/deploy_utils.cpp
    src/engine/deploy/strategy_symlink.cpp
    src/engine/deploy/strategy_hardlink.cpp
    src/engine/deploy/strategy_junction.cpp
    src/engine/deploy/strategy_overlayfs_deploy.cpp
    src/engine/deploy/deploy_ledger.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/launcher.cpp
    src/engine/overlay_launcher.cpp
    src/engine/preload_interceptor.cpp
    src/engine/fs_utils.cpp
    src/engine/single_instance.cpp
    src/runtime/runtime.cpp
    src/runtime/wine_runtime.cpp
    src/platform/${GMM_PLATFORM}/${GMM_PLATFORM}_platform.cpp
)
target_include_directories(settings_sources_tab_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(settings_sources_tab_test PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(settings_sources_tab_test PRIVATE Qt6::Widgets Qt6::Network
    PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::YAMLCPP PkgConfig::CURL PkgConfig::NLOHMANN_JSON
    pybind11::embed Python3::Python)
if(NOT WIN32)
    target_link_libraries(settings_sources_tab_test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME settings_sources_tab_test COMMAND settings_sources_tab_test ${CMAKE_CURRENT_BINARY_DIR}/plugins)
set_tests_properties(settings_sources_tab_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Downloads tab — offscreen GUI regression test for the untracked-archive
# scan (manually downloaded / dragged-in archives must show up and install).
# Hermetic: throwaway XDG_CONFIG_HOME, no network.
add_executable(downloads_tab_test tests/ui/downloads_tab_test.cpp
    src/ui/panels/tab_panels.cpp
    src/ui/settings/settings.cpp
    src/ui/widgets/column_toggle_header.cpp
    src/engine/log/logger.cpp
    src/engine/fs_utils.cpp
)
target_include_directories(downloads_tab_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(downloads_tab_test PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(downloads_tab_test PRIVATE Qt6::Widgets)
add_test(NAME downloads_tab_test COMMAND downloads_tab_test)
set_tests_properties(downloads_tab_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Data tab — offscreen GUI regression test for the MO2-parity file context
# menu and double-click behavior: tree population, hidden-file rendering
# (.gmmhidden/.mohidden), Open/Execute/Preview/Add-as-Executable/Mod Info/Hide
# signal wiring, disabled VFS entries, and the common menus.
add_executable(data_tab_test tests/ui/data_tab_test.cpp
    src/ui/panels/tab_panels.cpp
    src/ui/settings/settings.cpp
    src/ui/widgets/column_toggle_header.cpp
    src/engine/log/logger.cpp
    src/engine/fs_utils.cpp
)
target_include_directories(data_tab_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(data_tab_test PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(data_tab_test PRIVATE Qt6::Widgets)
add_test(NAME data_tab_test COMMAND data_tab_test)
set_tests_properties(data_tab_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Plugins tab — offscreen GUI test: population, pinned (force-loaded) rows,
# missing-master styling, toggle + sync_enabled signal behavior. Drag-reorder
# is intentionally not exercised (see the test's header comment).
add_executable(plugins_tab_test tests/ui/plugins_tab_test.cpp
    src/ui/panels/tab_panels.cpp
    src/ui/settings/settings.cpp
    src/ui/widgets/column_toggle_header.cpp
    src/engine/log/logger.cpp
    src/engine/fs_utils.cpp
)
target_include_directories(plugins_tab_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(plugins_tab_test PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(plugins_tab_test PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME plugins_tab_test COMMAND plugins_tab_test)
set_tests_properties(plugins_tab_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ModListModel — offscreen GUI test: game-native (unmanaged) band enforcement.
# Covers move_mod() no-op + clamp, drag-source exclusion, and drop clamping so
# unmanaged mods can never end up below user mods. The load-time band ordering
# (priority assignment + band-aware sort) lives in MainWindow and is verified
# manually.
add_executable(mod_list_model_test tests/ui/mod_list_model_test.cpp
    src/ui/widgets/mod_list_model.cpp
    src/ui/settings/settings.cpp
)
target_include_directories(mod_list_model_test PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(mod_list_model_test PRIVATE VERSION="${PROJECT_VERSION}")
target_link_libraries(mod_list_model_test PRIVATE Qt6::Widgets)
add_test(NAME mod_list_model_test COMMAND mod_list_model_test)
set_tests_properties(mod_list_model_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Overwrite dialogs — offscreen GUI test: QueryOverwriteDialog actions/backup,
# MoveToModDialog selection, SyncOverwriteDialog combos + targets(), and
# OverwriteInfoDialog mapping-root protection. Hermetic: throwaway
# XDG_CONFIG_HOME and /tmp file trees, no network.
add_executable(overwrite_dialogs_test tests/ui/overwrite_dialogs_test.cpp
    src/ui/overwrite/sync_overwrite_dialog.cpp
    src/ui/overwrite/move_to_mod_dialog.cpp
    src/ui/overwrite/overwrite_info_dialog.cpp
    src/ui/overwrite/query_overwrite_dialog.cpp
    src/engine/overwrite/overwrite_utils.cpp
    src/engine/index/conflict_engine.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/fs_utils.cpp
    src/engine/log/logger.cpp
)
target_include_directories(overwrite_dialogs_test PRIVATE src/)
target_link_libraries(overwrite_dialogs_test PRIVATE Qt6::Widgets
    PkgConfig::NLOHMANN_JSON)
add_test(NAME overwrite_dialogs_test COMMAND overwrite_dialogs_test)
set_tests_properties(overwrite_dialogs_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
