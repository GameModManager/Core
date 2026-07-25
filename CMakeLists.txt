cmake_minimum_required(VERSION 3.20)
project(GameModManager
    VERSION 0.1.0
    DESCRIPTION "Cross-platform game mod manager"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3)

enable_testing()

# Engine sources — no Qt includes allowed here
set(ENGINE_SOURCES
    src/engine/instance/instance.cpp
    src/engine/detect/game_detector.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/deploy/strategy_symlink.cpp
    src/engine/deploy/deploy_ledger.cpp
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
    src/engine/deploy/strategy_vfs.cpp
    src/engine/deploy/strategy_overlayfs.cpp
    src/engine/theme/theme_manager.cpp
)

# Runtime — Qt-free
set(RUNTIME_SOURCES
    src/runtime/runtime.cpp
)

# UI sources — Qt lives here
set(UI_SOURCES
    src/ui/main_window/main_window.cpp
    src/ui/widgets/mod_list_model.cpp
    src/ui/widgets/main_toolbar.cpp
    src/ui/widgets/profile_bar.cpp
    src/ui/widgets/right_panel.cpp
    src/ui/widgets/exec_controls_bar.cpp
    src/ui/widgets/console_panel.cpp
    src/ui/widgets/gmm_status_bar.cpp
    src/ui/widgets/mod_filter_bar.cpp
    src/ui/widgets/right_filter_bar.cpp
    src/ui/widgets/column_toggle_header.cpp
    src/ui/panels/tab_panels.cpp
    src/ui/pipeline_worker.cpp
)

# App entry point
set(APP_SOURCES
    src/main.cpp
)

add_executable(gamemodmanager ${ENGINE_SOURCES} ${RUNTIME_SOURCES} ${UI_SOURCES} ${APP_SOURCES})
target_link_libraries(gamemodmanager PRIVATE Qt6::Widgets PkgConfig::LIBZIP PkgConfig::SQLITE3 ${CMAKE_DL_LIBS})
if(FUSE3_FOUND)
    target_link_libraries(gamemodmanager PRIVATE PkgConfig::FUSE3)
    target_compile_definitions(gamemodmanager PRIVATE HAS_FUSE3=1)
endif()
target_include_directories(gamemodmanager PRIVATE src/ ${CMAKE_CURRENT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(gamemodmanager PRIVATE VERSION="${PROJECT_VERSION}")

# Tests — headless, no Qt
add_executable(pipeline_test tests/engine/pipeline_test.cpp
    src/engine/instance/instance.cpp
    src/engine/detect/game_detector.cpp
    src/engine/archive/archive_extractor.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
    src/engine/deploy/order_hook.cpp
    src/engine/deploy/strategy_symlink.cpp
    src/engine/pipeline/pipeline.cpp
    src/engine/pipeline/fetch_stage.cpp
    src/engine/pipeline/extract_stage.cpp
    src/engine/pipeline/install_stage.cpp
    src/engine/pipeline/stage_stage.cpp
    src/engine/pipeline/resolve_stage.cpp
    src/engine/pipeline/deploy_stage.cpp
    src/engine/pipeline/launch_stage.cpp
    src/engine/pipeline/sync_stage.cpp
)
target_include_directories(pipeline_test PRIVATE src/)
target_link_libraries(pipeline_test PRIVATE PkgConfig::LIBZIP PkgConfig::SQLITE3)
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
)
target_include_directories(registry_test PRIVATE src/)
target_link_libraries(registry_test PRIVATE PkgConfig::SQLITE3)
add_test(NAME registry_test COMMAND registry_test)

add_executable(perf_benchmark tests/perf/benchmark_test.cpp
    src/engine/index/conflict_index.cpp
    src/engine/model/profile.cpp
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
)
target_include_directories(perf_benchmark PRIVATE src/)
target_link_libraries(perf_benchmark PRIVATE PkgConfig::SQLITE3)
target_compile_definitions(perf_benchmark PRIVATE VERSION="${PROJECT_VERSION}")
add_test(NAME perf_benchmark COMMAND perf_benchmark)
