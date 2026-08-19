# Engine — Qt-free static library. No Qt6::* target is linked here and no
# Qt include directory is exposed. The Qt-using files (theme/style_manager,
# theme/icon_manager, notify/notification_backend, nxm/nxm_ipc,
# launch/single_instance) live under src/ui/ and are compiled into gmm_ui
# instead, keeping this target Qt-free at the CMake level.
#
# Directory groups (issue/33): core/, mod/, source/, game/, pipeline/,
# deploy/, platform/; sort/ and index/ stay at engine top level.
#
# Dependency direction: engine -> runtime -> platform (all Qt-free).

# FOMOD engine sources (pugixml-backed ModuleConfig parser, condition tester,
# view-model state machine, staging-dir file installer). Shared by the app and
# the engine tests that drive FomodStage directly.
set(FOMOD_ENGINE_SOURCES
    mod/fomod/module_config.cpp
    mod/fomod/condition_tester.cpp
    mod/fomod/fomod_view_model.cpp
    mod/fomod/file_installer.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pugixml/pugixml.cpp
)

set(ENGINE_SOURCES
    core/instance/instance.cpp
    core/instance/instance_utils.cpp
    core/instance/toml_utils.cpp
    core/instance/game_icons.cpp
    core/util/fs_utils.cpp
    mod/filetree/file_tree.cpp
    mod/filetree/dir_file_tree.cpp
    mod/filetree/data_archive.cpp
    mod/filetree/archive_file_tree.cpp
    mod/filetree/staging_layout.cpp
    game/registry/game_features/mod_data_checker.cpp
    game/registry/game_features/game_feature.cpp
    game/registry/game_features/game_feature_registry.cpp
    game/detect/game_detector.cpp
    game/detect/mod_scanner.cpp
    mod/archive/archive_extractor.cpp
    index/conflict_index.cpp
    index/isaac_conflict_index.cpp
    index/conflict_engine.cpp
    mod/model/profile.cpp
    profile/profile.cpp
    profile/profile_creation.cpp
    profile/profile_switching.cpp
    profile/delayed_file_writer.cpp
    profile/safe_write_file.cpp
    deploy/order_hook.cpp
    deploy/strategy_symlink.cpp
    deploy/strategy_direct.cpp
    deploy/strategy_hardlink.cpp
    deploy/strategy_junction.cpp
    deploy/strategy_overlayfs_deploy.cpp
    deploy/deploy_ledger.cpp
    deploy/deploy_utils.cpp
    deploy/root_override.cpp
    deploy/launch/overlay_launcher.cpp
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
    core/trace/trace_recorder.cpp
    core/events/event_bus.cpp
    pipeline/plugin_host/abi_bridge.cpp
    core/log/logger.cpp
    core/log/crash_handler.cpp
    mod/cache/mod_cache.cpp
    pipeline/plugin_host/plugin_loader.cpp
    pipeline/plugin_host/category_factory.cpp
    pipeline/plugin_host/diagnostics_registry.cpp
    pipeline/registry/stage_registry.cpp
    pipeline/registry/hook_registry.cpp
    game/registry/game_capabilities.cpp
    game/registry/game_knowledge.cpp
    source/nxm/nxm_router.cpp
    source/nxm/managed_games.cpp
    sort/abi_sort_provider.cpp
    sort/sort_registry.cpp
    source/workshop/remote_cache.cpp
    source/workshop/workshop_client.cpp
    platform/theme/theme_manager.cpp
    platform/tools/external_tool.cpp
    pipeline/plugin_host/python_loader.cpp
    mod/meta/mod_meta.cpp
    mod/meta/categories.cpp
    mod/overwrite/overwrite_utils.cpp
    game/plugins/esp_header.cpp
    game/plugins/plugin_database.cpp
    game/saves/save_game.cpp
    game/saves/save_reader.cpp
    game/saves/skyrim_save.cpp
    game/saves/save_scanner.cpp
    game/saves/save_missing_assets.cpp
    game/saves/local_saves.cpp
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
    source/download/curl_download.cpp
    core/keyring/keyring.cpp
    deploy/launch/preload_interceptor.cpp
    deploy/launch/launcher.cpp
    deploy/launch/proton_tools.cpp
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
    PkgConfig::TOMLPLUSPLUS
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
