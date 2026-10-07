# Engine - Qt-free static library. No Qt6::* target is linked here and no
# Qt include directory is exposed. The Qt-using files (theme/style_manager,
# theme/icon_manager, notify/notification_backend, nxm/nxm_ipc,
# launch/multi_process) live under src/ui/ and are compiled into gmm_ui
# instead, keeping this target Qt-free at the CMake level.
#
# Directory groups (issue/33): mod/, source/, game/, pipeline/, deploy/,
# modpack/, plugin_host/; sort/ and index/ stay at engine top level. The old
# core/ group was hoisted into this directory; the old platform/ group moved
# out to src/platform/, with theme/ and tools/ hoisted here.
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
    mod/fomod/headless_replay.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pugixml/pugixml.cpp
)

set(ENGINE_SOURCES
    instance/instance.cpp
    instance/instance_utils.cpp
    instance/instance_registry.cpp
    instance/toml_utils.cpp
instance/mod_state.cpp
    instance/instance_snapshot.cpp
    instance/installed_pack_state.cpp
    instance/game_icons.cpp
    instance/masterlist_fetch.cpp
    util/fs_utils.cpp
    util/thread_priority.cpp
    vfs/game_file.h
    filter/filter_combine.h
    vfs/path_resolver.h
    vfs/path_resolver.cpp
    vfs/path_resolver_registry.h
    vfs/path_resolver_registry.cpp
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
    mod/archive/sevenzip_backend.cpp
    modpack/gmmpack/sha256.cpp
    modpack/gmmpack/bsdiff.cpp
    modpack/gmmpack/patch.cpp
    index/conflict_index.cpp
    index/conflict_engine.cpp
    mod/model/profile.cpp
    profile/profile.cpp
    profile/profile_creation.cpp
    profile/profile_switching.cpp
    profile/delayed_file_writer.cpp
    profile/safe_write_file.cpp
    backup/backup_service.cpp
    deploy/order_hook.cpp
    deploy/core.cpp
    deploy/strategy_symlink.cpp
    deploy/strategy_direct.cpp
    deploy/strategy_overlayfs_deploy.cpp
    deploy/deploy_ledger.cpp
    deploy/deploy_utils.cpp
    deploy/root_override.cpp
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
    mod/cache/mod_cache.cpp
    plugin_host/plugin_loader.cpp
    plugin_host/tool_registry.cpp
    plugin_host/category_factory.cpp
    plugin_host/diagnostics_registry.cpp
    plugin_host/diagnose_registry.cpp
    plugin_host/file_mapper_registry.cpp
    plugin_host/save_parser_registry.cpp
    plugin_host/requirements_registry.cpp
    plugin_host/order_encoding_registry.cpp
    plugin_host/deploy_strategy_registry.cpp
    plugin_host/hook_registry.cpp
    plugin_host/plugin_settings_registry.cpp
    pipeline/registry/stage_registry.cpp
    pipeline/registry/hook_registry.cpp
    game/registry/game_capabilities.cpp
    game/registry/game_knowledge.cpp
    modpack/pack/source_detector.cpp
    source/nxm/nxm_router.cpp
    source/nxm/managed_games.cpp
    sort/sorter/abi.cpp
    sort/sorter/registry.cpp
    sort/sorter/loot/sorter.cpp
    sort/sorter/loot/masterlists.cpp
    source/steam/workshop_client.cpp
    theme/theme_manager.cpp
    tools/external_tool.cpp
    plugin_host/python_loader.cpp
    mod/meta/mod_meta.cpp
    mod/meta/categories.cpp
    mod/meta/category_set_registry.cpp
    mod/overwrite/overwrite_utils.cpp
modpack/ini_edits.cpp
modpack/model.cpp
modpack/collection_to_pack.cpp
install/conflict_resolver.cpp
    modpack/incremental_update.cpp
    install/game_match_validator.cpp
    install/instance_router.cpp
    install/fresh_install.cpp
    install/append_install.cpp
    game/esp/esp_header.cpp
    game/esp/plugin_database.cpp
    game/saves/save_game.cpp
    game/saves/save_fast_scan.cpp
    game/saves/save_reader.cpp
    game/saves/save_scanner.cpp
    game/saves/save_missing_assets.cpp
    game/saves/local_saves.cpp
    modpack/collection/resolver.cpp
    modpack/collection/batch_installer.cpp
    modpack/collection/manifest.cpp
    modpack/collection/sync.cpp
    source/registry.cpp
    source/update_policy.cpp
    source/http_util.cpp
    source/nexus/provider.cpp
    source/nexus/account.cpp
    source/nexus/http.cpp
    source/nexus/servers.cpp
    source/steam/provider.cpp
    source/loverslab/fetch.cpp
    source/loverslab/provider.cpp
    source/modl/provider.cpp
    source/modpub/provider.cpp
    source/git/git_info.cpp
    source/nexus/auth.cpp
    source/loverslab/auth.cpp
    source/download/curl_download.cpp
    network/network_manager.cpp
    network/nexus_v2/client.cpp
    network/nexus_v2/collection_revision.cpp
    update/self_updater.cpp
    update/install_method.cpp
    update/windows_self_updater.cpp
    update/macos_self_updater.cpp
    update/appimage_updater.cpp
    source/update/mod_update_db_client.cpp
    keyring/keyring.cpp
    github.cpp
    modpack/collection/nexus/parser.cpp
    modpack/collection/nexus/adapter.cpp
    modpack/collection/nexus/collection_zip.cpp
    module.cpp
    directory_refresher.cpp
    deploy/launch/preload_interceptor.cpp
    deploy/launch/elevation.cpp
    deploy/launch/launcher.cpp
    deploy/launch/proton_tools.cpp
    proton/proton_tweaks.cpp
modpack/gmmpack/schema_validator.cpp
    modpack/gmmpack/tree_parser.cpp
    modpack/gmmpack/unpacker.cpp
    modpack/gmmpack/ini_edit_parser.cpp
    modpack/gmmpack/packer.cpp
    modpack/gmmpack/uuid.cpp
    modpack/collection/choice_groups.cpp
    modpack/gmmpack/executable_pipeline.cpp
    ${FOMOD_ENGINE_SOURCES}
)

# OverlayFS launcher - Linux-only APIs (clone, mount namespaces, setxattr,
# /proc/*); excluded on macOS/Windows rather than ifdef'd per-error.
if(NOT APPLE)
    list(APPEND ENGINE_SOURCES
        deploy/launch/overlay_launcher.cpp
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

# nlohmann_json is header-only and leaks through the PUBLIC engine header
# mod/fomod/fomod_view_model.h, so consumers compiling that header need its
# include dir - it must be PUBLIC, not PRIVATE (PRIVATE would propagate to
# the link line only).
target_link_libraries(gmm_engine PUBLIC
    PkgConfig::NLOHMANN_JSON
)

# Link libraries - PRIVATE deps of a static library propagate as link-only
# dependencies to consumers, so tests that link gmm_engine get the system
# libraries they need without re-listing them.
target_link_libraries(gmm_engine PRIVATE
    gmm_runtime
    gmm_platform
    Qt6::Core
    Qt6::Gui # proton_tweaks.cpp: QProcess env + QScreen DPI detection
    PkgConfig::LIBARCHIVE
    PkgConfig::SEVENZIP
    PkgConfig::SQLITE3
    PkgConfig::YAMLCPP
    PkgConfig::CURL
    PkgConfig::TOMLPLUSPLUS
    PkgConfig::ZLIB
    PkgConfig::LZ4
    OpenSSL::Crypto
    pybind11::embed
    Python3::Python
)
if(NOT WIN32)
    target_link_libraries(gmm_engine PRIVATE ${CMAKE_DL_LIBS})
endif()
