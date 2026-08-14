# Engine — Qt-free static library. No Qt6::* target is linked here and no
# Qt include directory is exposed. The handful of Qt-using files that live
# under src/engine/ (theme/style_manager, theme/icon_manager,
# notify/notification_backend, nxm/nxm_ipc, single_instance) are compiled
# into gmm_ui instead, keeping this target Qt-free at the CMake level.
#
# Dependency direction: engine -> runtime -> platform (all Qt-free).

# FOMOD engine sources (pugixml-backed ModuleConfig parser, condition tester,
# view-model state machine, staging-dir file installer). Shared by the app and
# the engine tests that drive FomodStage directly.
set(FOMOD_ENGINE_SOURCES
    fomod/module_config.cpp
    fomod/condition_tester.cpp
    fomod/fomod_view_model.cpp
    fomod/file_installer.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pugixml/pugixml.cpp
)

set(ENGINE_SOURCES
    instance/instance.cpp
    instance/instance_utils.cpp
    instance/game_icons.cpp
    util/fs_utils.cpp
    filetree/file_tree.cpp
    filetree/dir_file_tree.cpp
    filetree/data_archive.cpp
    filetree/archive_file_tree.cpp
    filetree/staging_layout.cpp
    registry/game_features/mod_data_checker.cpp
    registry/game_features/game_feature.cpp
    registry/game_features/game_feature_registry.cpp
    detect/game_detector.cpp
    detect/mod_scanner.cpp
    archive/archive_extractor.cpp
    index/conflict_index.cpp
    index/isaac_conflict_index.cpp
    index/conflict_engine.cpp
    model/profile.cpp
    deploy/order_hook.cpp
    deploy/strategy_symlink.cpp
    deploy/strategy_hardlink.cpp
    deploy/strategy_junction.cpp
    deploy/strategy_overlayfs_deploy.cpp
    deploy/deploy_ledger.cpp
    deploy/deploy_utils.cpp
    deploy/root_override.cpp
    launch/overlay_launcher.cpp
    pipeline/pipeline.cpp
    pipeline/fetch_stage.cpp
    pipeline/extract_stage.cpp
    pipeline/fomod_stage.cpp
    pipeline/install_stage.cpp
    pipeline/stage_stage.cpp
    pipeline/resolve_stage.cpp
    pipeline/deploy_stage.cpp
    pipeline/launch_stage.cpp
    pipeline/sync_stage.cpp
    pipeline/plugin_claim_stage.cpp
    trace/trace_recorder.cpp
    events/event_bus.cpp
    plugin_host/abi_bridge.cpp
    log/logger.cpp
    log/crash_handler.cpp
    cache/mod_cache.cpp
    plugin_host/plugin_loader.cpp
    plugin_host/diagnostics_registry.cpp
    registry/stage_registry.cpp
    registry/hook_registry.cpp
    registry/game_capabilities.cpp
    registry/game_knowledge.cpp
    nxm/nxm_router.cpp
    nxm/managed_games.cpp
    sort/abi_sort_provider.cpp
    sort/sort_registry.cpp
    workshop/remote_cache.cpp
    workshop/workshop_client.cpp
    theme/theme_manager.cpp
    tools/external_tool.cpp
    plugin_host/python_loader.cpp
    meta/mod_meta.cpp
    meta/categories.cpp
    overwrite/overwrite_utils.cpp
    plugins/esp_header.cpp
    plugins/plugin_database.cpp
    saves/save_game.cpp
    saves/save_reader.cpp
    saves/skyrim_save.cpp
    saves/save_scanner.cpp
    saves/save_missing_assets.cpp
    saves/local_saves.cpp
    sort/loot/loot_sorter.cpp
    sort/loot/masterlists.cpp
    source/source_provider.cpp
    source/nexus_provider.cpp
    source/nexus_account.cpp
    source/nexus_http.cpp
    source/nexus_servers.cpp
    source/steam_workshop_provider.cpp
    source/loverslab_provider.cpp
    source/nexus_auth.cpp
    source/loverslab_auth.cpp
    download/curl_download.cpp
    keyring/keyring.cpp
    launch/preload_interceptor.cpp
    launch/launcher.cpp
    launch/proton_tools.cpp
    ${FOMOD_ENGINE_SOURCES}
)

# FUSE/VFS strategies — Linux only
if(NOT WIN32 AND FUSE3_FOUND)
    list(APPEND ENGINE_SOURCES
        deploy/strategy_vfs.cpp
        deploy/strategy_overlayfs.cpp
    )
endif()

add_library(gmm_engine STATIC ${ENGINE_SOURCES})

# Include dirs are PUBLIC so consumers (gmm_ui, gmm_cli, tests, the app) get
# the project include roots transitively.
target_include_directories(gmm_engine PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/external/abi/include
    ${PROJECT_SOURCE_DIR}/third_party/pugixml
)

# Version is declared once (project(VERSION ...) in the top-level CMakeLists)
# and flows to every consumer through this PUBLIC definition.
target_compile_definitions(gmm_engine PUBLIC VERSION="${PROJECT_VERSION}")

# Link libraries — PRIVATE deps of a static library propagate as link-only
# dependencies to consumers, so tests that link gmm_engine get the system
# libraries they need without re-listing them.
target_link_libraries(gmm_engine PRIVATE
    gmm_runtime
    gmm_platform
    PkgConfig::LIBARCHIVE
    PkgConfig::SQLITE3
    PkgConfig::YAMLCPP
    PkgConfig::CURL
    PkgConfig::NLOHMANN_JSON
    PkgConfig::ZLIB
    PkgConfig::LZ4
    pybind11::embed
    Python3::Python
)
if(NOT WIN32)
    target_link_libraries(gmm_engine PRIVATE ${CMAKE_DL_LIBS})
    if(FUSE3_FOUND)
        target_link_libraries(gmm_engine PRIVATE PkgConfig::FUSE3)
        target_compile_definitions(gmm_engine PUBLIC HAS_FUSE3=1)
    endif()
endif()
