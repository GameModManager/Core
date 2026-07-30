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

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

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

# Engine sources — no Qt includes allowed here
set(ENGINE_SOURCES
    src/engine/instance/instance.cpp
    src/engine/instance/instance_utils.cpp
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
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
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
    src/engine/source/source_provider.cpp
    src/engine/source/nexus_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/nexus_auth.cpp
    src/engine/preload_interceptor.cpp
    src/engine/launcher.cpp
    src/cli/headless_launcher.cpp
    src/engine/single_instance.cpp
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
    src/ui/widgets/debug_window.cpp
    src/ui/widgets/mod_filter_bar.cpp
    src/ui/widgets/right_filter_bar.cpp
    src/ui/widgets/column_toggle_header.cpp
    src/ui/widgets/menu_bar.cpp
    src/ui/panels/tab_panels.cpp
    src/ui/pipeline_worker.cpp
    src/ui/preview/anm2_parser.cpp
    src/ui/preview/preview_widget.cpp
    src/ui/game_selection/game_card.cpp
    src/ui/game_selection/game_selection_widget.cpp
)

# App entry point
set(APP_SOURCES
    src/main.cpp
)

add_executable(gamemodmanager ${ENGINE_SOURCES} ${RUNTIME_SOURCES} ${PLATFORM_SOURCES} ${UI_SOURCES} ${APP_SOURCES})

# Link libraries — platform-specific
target_link_libraries(gamemodmanager PRIVATE Qt6::Widgets Qt6::Network PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::YAMLCPP PkgConfig::CURL PkgConfig::NLOHMANN_JSON pybind11::embed Python3::Python)
if(NOT WIN32)
    target_link_libraries(gamemodmanager PRIVATE ${CMAKE_DL_LIBS})
    if(FUSE3_FOUND)
        target_link_libraries(gamemodmanager PRIVATE PkgConfig::FUSE3)
        target_compile_definitions(gamemodmanager PRIVATE HAS_FUSE3=1)
    endif()
else()
    # Windows: link ws2_32 for network, advapi32 for registry
    target_link_libraries(gamemodmanager PRIVATE ws2_32 advapi32 shlwapi)
endif()

target_include_directories(gamemodmanager PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
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
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/workshop/workshop_client.cpp
)
target_include_directories(pipeline_test PRIVATE src/)
target_link_libraries(pipeline_test PRIVATE PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::CURL PkgConfig::NLOHMANN_JSON)
if(WIN32)
    target_link_libraries(pipeline_test PRIVATE advapi32)
endif()
add_test(NAME pipeline_test COMMAND pipeline_test)

add_executable(phase04_test tests/engine/phase04_test.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
    src/engine/deploy/order_hook.cpp
)
target_include_directories(phase04_test PRIVATE src/)
add_test(NAME phase04_test COMMAND phase04_test)

add_executable(registry_test tests/engine/registry_test.cpp
    src/engine/registry/stage_registry.cpp
    src/engine/registry/hook_registry.cpp
    src/engine/instance/instance.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/pipeline/fetch_stage.cpp
    src/engine/pipeline/extract_stage.cpp
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/workshop/workshop_client.cpp
)
target_include_directories(registry_test PRIVATE src/)
target_link_libraries(registry_test PRIVATE PkgConfig::LIBARCHIVE PkgConfig::SQLITE3 PkgConfig::CURL PkgConfig::NLOHMANN_JSON)
add_test(NAME registry_test COMMAND registry_test)

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
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
    src/engine/log/logger.cpp
    src/engine/meta/mod_meta.cpp
    src/engine/source/source_provider.cpp
    src/engine/source/steam_workshop_provider.cpp
    src/engine/workshop/workshop_client.cpp
)
target_include_directories(perf_benchmark PRIVATE src/)
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
