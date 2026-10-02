# Test suite — only processed when GMM_BUILD_TESTS=ON (see top-level
# CMakeLists.txt). Tests link the per-directory libraries (gmm_engine,
# gmm_ui, gmm_runtime, gmm_platform) instead of re-listing engine sources;
# the libraries carry their transitive link dependencies.
#
# Every C++ test is a Catch2 v3 test: gmm_test_support provides the runner
# main() (with QSettings sandboxing, see below) and catch_discover_tests()
# registers each TEST_CASE as its own CTest test, so a failure in one case
# never hides the rest.

# ---------------------------------------------------------------------------
# Catch2 test helper
# ---------------------------------------------------------------------------
# Shared runner (Workspace-2zm): one main() for every test target that
# sandboxes QSettings (IniFormat + process-unique temp dir + XDG_CONFIG_HOME)
# before QApplication exists, so no test can read or write the user's real
# config. Linking it instead of Catch2WithMain is what makes the isolation
# automatic - see tests/support/test_main.cpp. A test that must exercise
# NativeFormat opts in with GMM_TEST_NATIVE_QSETTINGS=1 in its CTest
# ENVIRONMENT (documented there); none does today.
# Qt6::Core is PRIVATE: test sources still cannot include Qt headers unless
# they link Qt themselves (gmm_ui / Qt6::Core in LIBS), so the Qt-free engine
# tests stay Qt-free at compile time.
add_library(gmm_test_support STATIC support/test_main.cpp)
target_link_libraries(gmm_test_support
    PUBLIC Catch2::Catch2
    PRIVATE Qt6::Core)

# gmm_catch2_test(<name> <sources...>
#     [UI]
#     LIBS <extra link libraries>
#     DEFINES <compile definitions>
#     PROPERTIES <extra CTest properties>)
#
# Builds a Catch2 test executable and registers every TEST_CASE with CTest.
# Every test links gmm_engine (its PUBLIC include dirs carry the project
# include roots); pass extra libraries via LIBS. All tests run with TZ=UTC
# (hermetic); UI tests additionally get QT_QPA_PLATFORM=offscreen via
# ENVIRONMENT_MODIFICATION so they run headless.
function(gmm_catch2_test NAME)
    set(options UI)
    set(oneValueArgs "")
    set(multiValueArgs "LIBS" "DEFINES" "PROPERTIES")
    cmake_parse_arguments(GCT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Every test also links the LSan suppression list (Workspace-86bq):
    # __lsan_default_suppressions() in tests/support/lsan_suppressions.c
    # silences third-party exit-time allocations (Qt font/WebEngine, GPU
    # drivers, ...) without any env plumbing, so manual runs are covered too.
    add_executable(${NAME} ${GCT_UNPARSED_ARGUMENTS}
        ${CMAKE_CURRENT_SOURCE_DIR}/support/lsan_suppressions.c)
    target_link_libraries(${NAME} PRIVATE gmm_test_support gmm_engine ${GCT_LIBS})
    if(GCT_DEFINES)
        target_compile_definitions(${NAME} PRIVATE ${GCT_DEFINES})
    endif()

    # NOTE: keep every PROPERTIES value single-valued. The old quoted
    # ENVIRONMENT "TZ=UTC;QT_QPA_PLATFORM=offscreen" list broke because
    # Catch.cmake:222 joins TEST_PROPERTIES on semicolons and re-splits it,
    # so the env var name became a bogus property. Offscreen therefore rides
    # ENVIRONMENT_MODIFICATION (one value, no semicolons, survives all layers).
    set(_props ENVIRONMENT "TZ=UTC")
    if(GCT_UI)
        list(APPEND _props ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:offscreen")
    endif()
    catch_discover_tests(${NAME}
        TEST_PREFIX "${NAME}."
        PROPERTIES ${_props} ${GCT_PROPERTIES}
    )
endfunction()

# ---------------------------------------------------------------------------
# Engine tests — Qt-free, link the engine library
# ---------------------------------------------------------------------------
gmm_catch2_test(pack_adapter_test engine/pack_adapter_test.cpp)
gmm_catch2_test(conflict_resolver_test engine/conflict_resolver_test.cpp)
gmm_catch2_test(game_match_validator_test engine/game_match_validator_test.cpp)
gmm_catch2_test(instance_router_test engine/instance_router_test.cpp)
gmm_catch2_test(fresh_install_test engine/fresh_install_test.cpp)
gmm_catch2_test(append_install_test engine/append_install_test.cpp)
gmm_catch2_test(pipeline_test engine/pipeline_test.cpp)
gmm_catch2_test(pipeline_failure_reason_test engine/pipeline_failure_reason_test.cpp)
gmm_catch2_test(plugin_database_test engine/plugin_database_test.cpp)
gmm_catch2_test(loot_sorter_test engine/loot_sorter_test.cpp)
gmm_catch2_test(logger_test engine/logger_test.cpp)
gmm_catch2_test(event_bus_test engine/event_bus_test.cpp)
gmm_catch2_test(phase04_test engine/phase04_test.cpp)
gmm_catch2_test(trace_test engine/trace_test.cpp)
gmm_catch2_test(registry_test engine/registry_test.cpp)
gmm_catch2_test(fomod_module_config_test engine/fomod_module_config_test.cpp)
gmm_catch2_test(fomod_condition_test engine/fomod_condition_test.cpp)
gmm_catch2_test(instance_path_test engine/instance_path_test.cpp)
gmm_catch2_test(instance_settings_test engine/instance_settings_test.cpp)
gmm_catch2_test(mod_state_test engine/mod_state_test.cpp)
gmm_catch2_test(instance_snapshot_test engine/instance_snapshot_test.cpp)
gmm_catch2_test(installed_pack_state_test engine/installed_pack_state_test.cpp)
gmm_catch2_test(fs_utils_executables_test engine/fs_utils_executables_test.cpp)
gmm_catch2_test(vfs_path_resolver_test engine/vfs_path_resolver_test.cpp)
gmm_catch2_test(categories_test engine/categories_test.cpp)
gmm_catch2_test(category_factory_test engine/category_factory_test.cpp)
gmm_catch2_test(category_set_registry_test engine/category_set_registry_test.cpp)
gmm_catch2_test(mod_meta_test engine/mod_meta_test.cpp)
gmm_catch2_test(profile_test engine/profile_test.cpp)
gmm_catch2_test(profile_creation_test engine/profile_creation_test.cpp)
gmm_catch2_test(profile_switching_test engine/profile_switching_test.cpp)

# Workspace-czc0: timestamped backup / restore points of a profile's load order
# and mod list. Qt-free, so the whole of the feature's risk (stamp naming, the
# keep-10 eviction, the unconditional pre-restore safety backup and the
# per-file restore reporting) is covered here without a GUI.
gmm_catch2_test(backup_service_test engine/backup_service_test.cpp)
gmm_catch2_test(launch_params_test engine/launch_params_test.cpp)

# Workspace-cch0: headless CLI launch honors per-executable args/env/cwd -
# Qt-free splitter/cwd/entry-lookup helpers mirrored from the GUI path.
gmm_catch2_test(headless_launch_config_test engine/headless_launch_config_test.cpp)

# Workspace-cch0: headless CLI --args/--env/--cwd parsing plus the
# Config/entry -> LaunchPrepRequest -> LaunchParams mapping (no spawn).
gmm_catch2_test(headless_launch_cli_test cli/headless_launch_test.cpp
    ../src/cli/command_line.cpp
    UI
    LIBS gmm_cli Qt6::Core)
gmm_catch2_test(game_icons_test engine/game_icons_test.cpp)
# Reading .git/config the way git writes it - tabbed, quoted subsection name -
# from a mod folder under a game path containing spaces. A pure file read, so
# this runs whether or not git is installed.
gmm_catch2_test(git_info_test engine/git_info_test.cpp)
gmm_catch2_test(nxm_router_test engine/nxm_router_test.cpp)
gmm_catch2_test(pack_source_detector_test engine/pack_source_detector_test.cpp)
gmm_catch2_test(scanner_test engine/scanner_test.cpp)
# The enable/disable sentinel reason: a refused write has to report what the
# OS said, and a successful one has to report nothing. Runs against a game
# with no plugin so the engine default sentinel is the code under test.
gmm_catch2_test(mod_toggle_reason_test engine/mod_toggle_reason_test.cpp)
gmm_catch2_test(theme_test engine/theme_test.cpp)
gmm_catch2_test(keyring_test engine/keyring_test.cpp)
# Negative redaction tests at the log call sites: a signed URL or an
# unparseable env entry must not reach gamemodmanager.log with its secret.
gmm_catch2_test(redaction_callsite_test engine/redaction_callsite_test.cpp)
gmm_catch2_test(conflict_test engine/conflict_test.cpp)
gmm_catch2_test(conflict_external_merge_test engine/conflict_external_merge_test.cpp)
gmm_catch2_test(overwrite_utils_test engine/overwrite_utils_test.cpp)
gmm_catch2_test(ini_edits_test engine/ini_edits_test.cpp)
gmm_catch2_test(incremental_update_test engine/incremental_update_test.cpp)
gmm_catch2_test(fs_utils_test engine/fs_utils_test.cpp)
gmm_catch2_test(local_saves_test engine/local_saves_test.cpp)
gmm_catch2_test(mygames_test engine/mygames_test.cpp)
gmm_catch2_test(deploy_hidden_test engine/deploy_hidden_test.cpp)
gmm_catch2_test(workarounds_test engine/workarounds_test.cpp)
# Settings > Workarounds: the QSettings read reaching the engine predicates.
gmm_catch2_test(workarounds_bridge_test ui/workarounds_bridge_test.cpp
    UI
    LIBS gmm_ui)
gmm_catch2_test(deploy_case_insensitive_test engine/deploy_case_insensitive_test.cpp)
gmm_catch2_test(root_override_test engine/root_override_test.cpp)
gmm_catch2_test(deploy_binary_test engine/deploy_binary_test.cpp)
gmm_catch2_test(deploy_parallel_test engine/deploy_parallel_test.cpp)
gmm_catch2_test(deploy_direct_test engine/deploy_direct_test.cpp)
gmm_catch2_test(nexus_http_test engine/nexus_http_test.cpp)
gmm_catch2_test(network_test engine/network_test.cpp)
gmm_catch2_test(network_include_order_test engine/network_include_order_test.cpp
    LIBS PkgConfig::CURL)
gmm_catch2_test(nexus_provider_test engine/nexus_provider_test.cpp)
gmm_catch2_test(nexus_v2_test engine/nexus_v2_test.cpp)
gmm_catch2_test(loverslab_auth_test engine/loverslab_auth_test.cpp)
gmm_catch2_test(loverslab_provider_test engine/loverslab_provider_test.cpp)
gmm_catch2_test(modpub_provider_test engine/modpub_provider_test.cpp)
gmm_catch2_test(loverslab_real_test engine/loverslab_real_test.cpp)
gmm_catch2_test(mod_update_db_client_test engine/mod_update_db_client_test.cpp)
gmm_catch2_test(install_method_test engine/install_method_test.cpp)
gmm_catch2_test(self_updater_test engine/self_updater_test.cpp)
gmm_catch2_test(update_policy_test engine/update_policy_test.cpp)
gmm_catch2_test(install_progress_test engine/install_progress_test.cpp)

# 7-Zip IArchive backend: the RAR routing table and the large-dictionary and
# encrypted-RAR cases libarchive's RAR5 reader refuses outright.
gmm_catch2_test(sevenzip_archive_test engine/sevenzip_archive_test.cpp)
gmm_catch2_test(manifest_schema_test engine/manifest_schema_test.cpp)
gmm_catch2_test(collection_provider_test engine/collection_provider_test.cpp)
gmm_catch2_test(collection_batch_install_test engine/collection_batch_install_test.cpp)
gmm_catch2_test(collection_download_router_test engine/collection_download_router_test.cpp)
gmm_catch2_test(collection_sync_test engine/collection_sync_test.cpp)
gmm_catch2_test(nexus_collection_parser_test engine/nexus_collection_parser_test.cpp)
gmm_catch2_test(nexus_adapter_test engine/nexus_adapter_test.cpp)
gmm_catch2_test(gmmpack_patch_test engine/gmmpack_patch_test.cpp)
gmm_catch2_test(fomod_headless_replay_test engine/fomod_headless_replay_test.cpp)
gmm_catch2_test(choice_groups_test engine/choice_groups_test.cpp)
gmm_catch2_test(perf_benchmark perf/benchmark_test.cpp)

# Tests that include third-party headers directly need those include dirs at
# compile time (the engine library's PRIVATE deps only propagate to the link
# line, not to consumers' compile step).
gmm_catch2_test(save_parser_test engine/save_parser_test.cpp
    LIBS PkgConfig::ZLIB PkgConfig::LZ4)
gmm_catch2_test(filetree_test engine/filetree_test.cpp
    LIBS PkgConfig::LIBARCHIVE)
gmm_catch2_test(fomod_installer_test engine/fomod_installer_test.cpp)

# Proton tool-running: runner override resolution + run guards (needs runtime).
gmm_catch2_test(proton_tools_test engine/proton_tools_test.cpp
    LIBS gmm_runtime)

# IconManager resolution chain — offscreen GUI test (QApplication for SP
# fallback). icon_manager.cpp lives in gmm_ui (it is Qt-using); theme_manager
# comes from gmm_engine.
gmm_catch2_test(icon_manager_test engine/icon_manager_test.cpp
    UI
    LIBS gmm_ui)

# Live OS keyring roundtrip — only when QtKeychain is available; skips
# cleanly when no keyring daemon is running. qtkeychain_keyring.cpp is not in
# any library (it is compiled into the app only), so compile it here.
if(Qt6Keychain_FOUND)
    gmm_catch2_test(qtkeychain_test engine/qtkeychain_test.cpp
        ${PROJECT_SOURCE_DIR}/src/keyring/qtkeychain_keyring.cpp
        LIBS Qt6Keychain::Qt6Keychain Qt6::Core)
endif()

# Linux platform module (mimeapps / XDG handling).
if(GMM_PLATFORM STREQUAL "linux")
    gmm_catch2_test(mimeapps_test engine/mimeapps_test.cpp
        LIBS gmm_platform)
endif()

# Python plugin test — requires pybind11 embed + Python (propagated by gmm_engine).
gmm_catch2_test(python_plugin_test engine/python_plugin_test.cpp)

gmm_catch2_test(gmmpack_test engine/gmmpack_test.cpp
    LIBS PkgConfig::LIBARCHIVE OpenSSL::Crypto
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gmm_catch2_test(gmmpack_mod_entry_test engine/gmmpack_mod_entry_test.cpp)
gmm_catch2_test(gmmpack_manifest_parser_test engine/gmmpack_manifest_parser_test.cpp
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gmm_catch2_test(gmmpack_tree_test engine/gmmpack_tree_test.cpp)
gmm_catch2_test(gmmpack_packer_test engine/gmmpack_packer_test.cpp
    LIBS PkgConfig::LIBARCHIVE OpenSSL::Crypto
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gmm_catch2_test(ini_edit_parser_test engine/ini_edit_parser_test.cpp)
gmm_catch2_test(executable_pipeline_test engine/executable_pipeline_test.cpp)

# ---------------------------------------------------------------------------
# Test fixture plugins (MODULE libraries) — built into test_plugins/ and
# loaded ONLY by the tests below; never shipped to the app's plugins dir.
# ---------------------------------------------------------------------------
add_library(gmm_override_mod_data_checker MODULE
    engine/plugins/override_mod_data_checker.cpp
)
target_include_directories(gmm_override_mod_data_checker PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
set_target_properties(gmm_override_mod_data_checker PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

add_library(gmm_fomod_stage_plugin MODULE
    engine/plugins/fomod_stage_plugin.cpp
)
target_include_directories(gmm_fomod_stage_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
set_target_properties(gmm_fomod_stage_plugin PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

add_library(gmm_settings_tab_plugin MODULE
    engine/plugins/settings_tab_plugin.cpp
)
target_include_directories(gmm_settings_tab_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
set_target_properties(gmm_settings_tab_plugin PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

# Fixture plugins resolve the host's gmm_* accessors through dlopen, so their
# undefined symbols are expected. Linux .so links allow that by default; macOS
# two-level namespace does not — opt into load-time flat lookup.
if(APPLE)
    foreach(_fixture gmm_override_mod_data_checker gmm_fomod_stage_plugin gmm_settings_tab_plugin)
        target_link_options(${_fixture} PRIVATE -undefined dynamic_lookup)
    endforeach()
endif()

# P1.2 GameFeatureRegistry — priority + replace registration, combined
# ModDataChecker, and the exit criterion (a test plugin overrides the Skyrim
# plugin's data-checker).
gmm_catch2_test(game_feature_registry_test engine/game_feature_registry_test.cpp
    DEFINES
        GMM_OVERRIDE_PLUGIN_PATH="$<TARGET_FILE:gmm_override_mod_data_checker>"
        GMM_SKYRIM_PLUGIN_PATH="$<TARGET_FILE:SkyrimSpecialEdition>"
)
add_dependencies(game_feature_registry_test SkyrimSpecialEdition gmm_override_mod_data_checker)

# P1.4 host UI bridge test — a plugin claims the Fomod stage and runs the
# install through GmmHostUi::fomod_wizard. The fixture plugin resolves the
# gmm_* accessors through dlopen — same export requirement as the app.
gmm_catch2_test(fomod_stage_plugin_test engine/fomod_stage_plugin_test.cpp
    DEFINES
        GMM_FOMOD_PLUGIN_PATH="$<TARGET_FILE:gmm_fomod_stage_plugin>"
)
# The fixture plugin resolves gmm_* accessors through dlopen — same export
# requirement as the app. Linux needs -rdynamic; macOS needs -export_dynamic
# or the bundle's load-time flat lookup can't see the executable's symbols.
# Nothing references abi_bridge.cpp at link time, so also force its archive
# member out of libgmm_engine (-u) or the accessors are absent entirely and
# the plugin fails to load with "symbol not found in flat namespace".
if(APPLE)
    target_link_options(fomod_stage_plugin_test PRIVATE -Wl,-export_dynamic -Wl,-u,_gmm_instance_game_id)
elseif(NOT WIN32)
    target_link_options(fomod_stage_plugin_test PRIVATE -rdynamic -Wl,-u,gmm_instance_game_id)
endif()
add_dependencies(fomod_stage_plugin_test gmm_fomod_stage_plugin)

# StageRegistry wildcard claim resolution — pure engine test, no Qt, no UI.
gmm_catch2_test(stage_registry_wildcard_test engine/stage_registry_wildcard_test.cpp)

# ---------------------------------------------------------------------------
# UI tests — offscreen GUI tests, link the UI library (which carries Qt and
# the engine transitively)
# ---------------------------------------------------------------------------
gmm_catch2_test(settings_plugins_tab_test ui/settings_plugins_tab_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS}
    DEFINES
        GMM_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins"
        GMM_TEST_PLUGINS_DIR="${CMAKE_BINARY_DIR}/test_plugins"
)
add_dependencies(settings_plugins_tab_test gmm_settings_tab_plugin)

gmm_catch2_test(style_manager_palette_test ui/style_manager_palette_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(style_manager_base_style_test ui/style_manager_base_style_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(flags_delegate_test ui/flags_delegate_test.cpp
    LIBS gmm_ui)

gmm_catch2_test(settings_sources_tab_test ui/settings_sources_tab_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS})

gmm_catch2_test(settings_content_widget_test ui/settings_content_widget_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# The three updater settings and the read-only install-method line: asserts on
# what each setting's consumer renders, not on a getter round-trip.
gmm_catch2_test(settings_updater_test ui/settings_updater_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# Isaac's disable.it sentinel. Drives the real MainWindow with the REAL game
# plugin's knowledge - a hand-written GameKnowledge would pass even if the
# plugin still asked for a deferred write - and asserts the sentinel is in the
# mod folder the moment the checkbox is ticked. The plugin is loaded from the
# build tree for the same reason game_feature_registry_test loads Skyrim, so a
# declaration change fails here rather than at the user's desk.
gmm_catch2_test(disable_it_toggle_test ui/disable_it_toggle_test.cpp
    UI
    LIBS gmm_ui Qt6::Test ${CMAKE_DL_LIBS}
    DEFINES GMM_ISAAC_PLUGIN_PATH="$<TARGET_FILE:TheBindingOfIsaacRebirth>")
add_dependencies(disable_it_toggle_test TheBindingOfIsaacRebirth)

# Checkerboard transparency grid (PreviewWindow / ImageViewer): reference
# tile colors, 2x2 menu icons, transparency gating, Settings persistence.
# Offscreen; throwaway XDG_CONFIG_HOME so the Settings section is hermetic.
gmm_catch2_test(checkerboard_test ui/checkerboard_test.cpp
    UI
    LIBS gmm_ui)

# PreviewWindow image auto-fit: small images scale up, large images scale
# down, resize re-fits. Offscreen; throwaway XDG_CONFIG_HOME (hermetic).
gmm_catch2_test(preview_fit_test ui/preview_fit_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(source_tab_test ui/source_tab_test.cpp
    UI
    LIBS gmm_ui)

# The Git source actions against a real repository and a real git binary -
# the reset's scope guard and its "says what it discards" promise are only
# meaningful there, and a mock would assert the mock. Skips when git is
# absent. Local paths only, no network.
gmm_catch2_test(git_ops_test ui/git_ops_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(loverslab_source_panel_test ui/loverslab_source_panel_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(downloads_tab_test ui/downloads_tab_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(downloads_tab_state_test ui/downloads_tab_state_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(pipeline_worker_test ui/pipeline_worker_test.cpp
    LIBS gmm_ui Qt6::Core)

gmm_catch2_test(plugin_db_load_test ui/plugin_db_load_test.cpp
    LIBS gmm_ui Qt6::Core)

gmm_catch2_test(mod_scan_worker_test ui/mod_scan_worker_test.cpp
    LIBS gmm_ui Qt6::Core)

gmm_catch2_test(generic_files_tab_test ui/generic_files_tab_test.cpp
    UI
    LIBS gmm_ui)
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(generic_files_tab_test PRIVATE KF6::SyntaxHighlighting)
endif()

gmm_catch2_test(filetree_tab_test ui/filetree_tab_test.cpp
    UI
    LIBS gmm_ui)

# DataTab incremental conflict update on mod reorder (Workspace-z1iu):
# reordered winners update in place instead of clearing + recreating the
# whole tree. Offscreen; pumps the worker build + chunked apply.
gmm_catch2_test(data_tab_reorder_test ui/data_tab_reorder_test.cpp
    UI
    LIBS gmm_ui)

# Double-click opens previews (Workspace-co2 + Workspace-636): the
# Settings::double_clicks_open_previews round-trip, the DataTab
# setting x modifier x file-type matrix, and the plain double-click
# integration. Offscreen; throwaway XDG_CONFIG_HOME + /tmp/opencode tree.
gmm_catch2_test(double_click_previews_test ui/double_click_previews_test.cpp
    UI
    LIBS gmm_ui)

# BBCode adapter + entity/BR normalization (ui/modinfo/bbcode.cpp). Pure
# logic - no QApplication, no panel, no event loop. Reaches gmm_ui for
# the bbcode_to_html symbol but does not need the QT_QPA_PLATFORM=offscreen
# environment.
gmm_catch2_test(bbcode_test ui/bbcode_test.cpp
    LIBS gmm_ui)

# DescriptionBrowser (ui/modinfo/description_browser.cpp) - the QTextBrowser
# subclass that loads remote [img] URLs via QNetworkAccessManager. Needs a
# QApplication for the loadResource override to be exercisable, so this is
# a UI test (offscreen).
gmm_catch2_test(description_browser_test ui/description_browser_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(plugins_tab_test ui/plugins_tab_test.cpp
    UI
    DEFINES GMM_TEST_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(saves_tab_test ui/saves_tab_test.cpp
    UI
    LIBS gmm_ui Qt6::Test PkgConfig::ZLIB)

gmm_catch2_test(save_info_dialog_test ui/save_info_dialog_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(mod_list_model_test ui/mod_list_model_test.cpp
    UI
    LIBS gmm_ui
    # The Source column's badges (nexusmods, github, git) are only meaningful
    # when the icon chain actually resolves; pointing IconManager at the source
    # tree gives the coexistence case real icons to compare.
    DEFINES GMM_TEST_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources")

# Settings > Workarounds > Network: the custom browser actually receives the URL
# (a real recorder program is started, not just a getter round-trip).
gmm_catch2_test(web_link_test ui/web_link_test.cpp
    UI
    LIBS gmm_ui)

# Settings > Windows > "Center dialogs on screen": where a restored dialog lands.
gmm_catch2_test(dialog_placement_test ui/dialog_placement_test.cpp
    UI
    LIBS gmm_ui)

# Settings > General > "Confirm before switching game instance".
gmm_catch2_test(instance_switch_plan_test ui/instance_switch_plan_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(category_filter_panel_test ui/category_filter_panel_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(categories_tab_test ui/categories_tab_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(categories_dialog_test ui/categories_dialog_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(profile_manager_dialog_test ui/profile_manager_dialog_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(exec_entry_dialog_test ui/exec_entry_dialog_test.cpp
    UI
    LIBS gmm_ui)

# Workspace-vwa: "<Edit...>" sentinel must open the editor even with 0
# executables; rebuilds must never open it.
gmm_catch2_test(exec_controls_bar_test ui/exec_controls_bar_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(executables_parser_test ui/executables_parser_test.cpp
    UI
    LIBS gmm_ui)

# Elevated launch planning + the per-executable elevation field. Asserts the
# constructed argv only - nothing here executes pkexec/osascript/fakeroot, and
# the fakeroot probe is injected so both branches are deterministic.
gmm_catch2_test(elevation_test engine/elevation_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# Workspace-421: executables seeding also scans macOS app roots outside
# game_dir (~/Applications, /Applications) and stores those hits absolute.
gmm_catch2_test(executables_seed_test ui/executables_seed_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(overwrite_dialogs_test ui/overwrite_dialogs_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(list_dialog_test ui/list_dialog_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# Workspace-0713: DebugWindow overhaul smoke test. Pushes samples into the
# rolling chart widget to make sure neither QtCharts nor the QPainter
# fallback path crashes.
gmm_catch2_test(rolling_chart_test ui/rolling_chart_test.cpp
    UI
    LIBS gmm_ui)

# Workspace-tnj: set_game_info guard teardown — banner visibility + game-less
# instance mod ops.
gmm_catch2_test(game_path_banner_test ui/game_path_banner_test.cpp
    UI
    LIBS gmm_ui)

# Status bar: context label (game - instance - profile), the metered-source
# rule that keeps a per-source readout only for a source with a budget, the
# separator that must not dangle, and the transient status timeout.
gmm_catch2_test(status_bar_test ui/status_bar_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(instance_options_panel_test ui/instance_options_panel_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS}
    DEFINES GMM_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins")

gmm_catch2_test(instance_options_widget_test ui/instance_options_widget_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS}
    DEFINES GMM_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins")

gmm_catch2_test(install_progress_dialog_test ui/install_progress_dialog_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(install_widget_test ui/install_widget_test.cpp
    UI
    LIBS gmm_ui)

# FOMOD plugin integration — loads the FomodInstaller plugin, verifies its
# settings tab appears in the Settings dialog with correct settings and
# persistence. Also documents whether the old standalone FOMOD tab still exists.
gmm_catch2_test(fomod_plugin_settings_test ui/fomod_plugin_settings_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS}
    DEFINES
        GMM_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins"
        GMM_TEST_PLUGINS_DIR="${CMAKE_BINARY_DIR}/test_plugins"
)

# Workspace-52hc: reusable ui::TaskDialog (MO2 U267 port) + the
# Settings::dialog_choice remember seam + the Remove Mods walking-skeleton
# consumer. TDD red phase: the component does not exist yet, so these three
# fail to compile until src/ui/widgets/task_dialog.{h,cpp}, the Settings
# seam, and ui::configure_remove_mods_dialog land.
gmm_catch2_test(task_dialog_test ui/task_dialog_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(settings_dialog_choice_test ui/settings_dialog_choice_test.cpp
    UI
    LIBS gmm_ui)

# Workspace-kxm7: Settings General-tab "Reset dialog choices" button closing
# the TaskDialog remember loop (MO2 QuestionBoxMemory-reset parity).
gmm_catch2_test(settings_reset_choices_test ui/settings_reset_choices_test.cpp
    UI
    LIBS gmm_ui)

# Workspace-2zm: QSettings isolation. Two halves of the same proof.
#   qsettings_sandbox_test       - the production QSettings objects land in a
#     temp ini when the shared runner has flipped the default format, and
#     cannot touch the user's real config.
#   qsettings_native_format_test - the same production objects with the sandbox
#     opted out (GMM_TEST_NATIVE_QSETTINGS=1), i.e. the real native store, and
#     the constructor they replaced reads back everything they wrote.
# Both are non-UI (no widget, no event loop).
#
# The native target WRITES, so it must not be able to reach a real config. It
# relocates the native store root itself, with QSettings::setPath, before it
# constructs anything - see ui/qsettings_native_format_test.cpp. It is NOT done
# with a fake HOME env var here: ENVIRONMENT_MODIFICATION takes a ';' list, and
# an escaped list does not survive catch_discover_tests' -D hand-off (the values
# arrive as separate arguments and become bogus property names - the same
# failure mode as the ENVIRONMENT note above), while a single HOME override
# would not be enough anyway: a developer machine with XDG_CONFIG_HOME set
# resolves the native store from that, not from HOME.
gmm_catch2_test(qsettings_sandbox_test ui/qsettings_sandbox_test.cpp
    LIBS gmm_ui Qt6::Core)

gmm_catch2_test(qsettings_native_format_test ui/qsettings_native_format_test.cpp
    LIBS gmm_ui Qt6::Core
    PROPERTIES ENVIRONMENT_MODIFICATION "GMM_TEST_NATIVE_QSETTINGS=set:1")

gmm_catch2_test(remove_mods_dialog_test ui/remove_mods_dialog_test.cpp
    UI
    LIBS gmm_ui
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gmm_catch2_test(remove_download_dialog_test ui/remove_download_dialog_test.cpp
    UI
    LIBS gmm_ui
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gmm_catch2_test(delete_profile_dialog_test ui/delete_profile_dialog_test.cpp
    UI
    LIBS gmm_ui
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Workspace-nef3: instance-switch restart confirmation (MO2 U044 parity).
# ui::configure_instance_restart_dialog seam + the switch_to_instance wiring.
gmm_catch2_test(instance_restart_dialog_test ui/instance_restart_dialog_test.cpp
    UI
    LIBS gmm_ui
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Workspace-t6z5: TaskDialog adoption for the overwrite confirmations
# (clear-overwrite + Overwrite info-dialog delete). configure_* seams plus
# the clear_overwrite / delete_selected wiring.
gmm_catch2_test(overwrite_confirm_dialog_test ui/overwrite_confirm_dialog_test.cpp
    UI
    LIBS gmm_ui
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Workspace-t373: launch-failure details dialogs (spawn-error parity).
# ui::configure_executable_unreachable_dialog +
# ui::configure_launch_failed_dialog seams + the on_launch_params_prepared
# wiring.
gmm_catch2_test(launch_failure_dialog_test ui/launch_failure_dialog_test.cpp
    UI
    LIBS gmm_ui
    DEFINES PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Workspace-axva: reusable UI primitives pack (MO2 U264 / U265 / U269).
# SortableTreeWidget + set_customizable_columns, ExpanderWidget, LinkLabel,
# LineEditClear and the generic EventFilter, one test case per primitive.
gmm_catch2_test(ui_primitives_test ui/ui_primitives_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# Workspace-so1s: global ui::report_error / ui::critical_on_top error popup
# (MO2 U268 port). TDD red phase: fails to compile until
# src/ui/widgets/error_popup.{h,cpp} exists.
gmm_catch2_test(error_popup_test ui/error_popup_test.cpp
    UI
    LIBS gmm_ui)

# Workspace-qs50: full-window in-process GUI harness - drives the REAL
# MainWindow offscreen with QTest (tab clicks, sort button, mod context
# menu, hover), asserts QToolTip content, model/view state and filesystem
# side effects (LOOT sort -> plugin hover bullets, Saves tab scan rows,
# tab switch -> last_tab, mod delete -> trash). Per-test throwaway
# /tmp/gmm_qs50_* roots; a fake gmm_lootcli is installed beside the test
# binary at runtime (removed on teardown), so no network and no real LOOT
# binary are needed.
gmm_catch2_test(main_window_harness_test ui/main_window_harness_test.cpp
    UI
    LIBS gmm_ui Qt6::Test PkgConfig::ZLIB)

# Workspace-czc0: the load-order / mod-list backup and restore points, driven
# through the real window's four toolbar buttons with QTest clicks, asserting
# the bytes on disk. Consumer-reaches-control: a button click is the whole
# chain, and only the files afterwards are worth asserting.
gmm_catch2_test(backup_restore_test ui/backup_restore_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# Filter-bar MO2 parity - the "Filter" placeholder plus the
# Ctrl+F focus / Escape clear pair. Drives the REAL MainWindow offscreen and
# sends the keys with QTest, so the check covers Qt actually routing them to
# the window shortcuts (not a direct handler call). No game, no instance, no
# files; rows go straight into the model so the clear path is observable
# through row visibility.
gmm_catch2_test(filter_shortcuts_test ui/filter_shortcuts_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

# In-window log view retention. The panel is fed by engine::Logger for the
# whole session, so the rendered document needs an on-screen window or it grows
# without bound; this pins the window and that the tail is what survives.
gmm_catch2_test(console_panel_test ui/console_panel_test.cpp
    UI
    LIBS gmm_ui)

# MO2 menu parity - the Help tree (order, the game-support-URL condition, the
# stubbed Tutorials submenu) and the keyboard table read off the real
# AppMenuBar. The menu bar owns every action and its shortcut, so it is built
# standalone: no MainWindow, no game, no instance, no files touched.
gmm_catch2_test(menu_parity_test ui/menu_parity_test.cpp
    UI
    LIBS gmm_ui)

# ---------------------------------------------------------------------------
# LD_PRELOAD intercept smoke tests — Linux-only, C (plain executables, not
# Catch2 — they are C and test the interposer shared libraries directly)
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    add_executable(overlay_intercept_test engine/overlay_intercept_test.c)
    target_compile_definitions(overlay_intercept_test PRIVATE
        GMM_INTERCEPT_SO="${CMAKE_BINARY_DIR}/libgmm_overlay_intercept.so")
    target_link_libraries(overlay_intercept_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME overlay_intercept_test COMMAND overlay_intercept_test)
    set_tests_properties(overlay_intercept_test PROPERTIES ENVIRONMENT "TZ=UTC")

    add_executable(ci_intercept_test engine/ci_intercept_test.c)
    target_compile_definitions(ci_intercept_test PRIVATE
        GMM_CI_SO="${CMAKE_BINARY_DIR}/libgmm_ci_intercept.so")
    target_link_libraries(ci_intercept_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME ci_intercept_test COMMAND ci_intercept_test)
    set_tests_properties(ci_intercept_test PROPERTIES ENVIRONMENT "TZ=UTC")

    # Deliberate leak that MUST still be reported. The binary links the same
    # suppression list as every other test; PASS_REGULAR_EXPRESSION matches
    # LSan's report, so a suppression entry broad enough to hide our own
    # leaks turns this test red.
    #
    # Gated on a leak sanitizer being compiled in, not on the platform: the
    # binary is plain C with no allocator of its own, so the report comes
    # entirely from the compiler's -fsanitize flag. `-fsanitize=leak` is
    # standalone LSan; `-fsanitize=address` enables LSan by default on Linux.
    # Without either, the binary leaks silently and exits 0, so registering
    # the test unconditionally leaves it permanently red in a default build.
    # CMAKE_C_FLAGS is what compiles this target, but a project-wide sanitizer
    # is often set through CXX-only flags, so accept either.
    if(CMAKE_C_FLAGS MATCHES "fsanitize=(address|leak)"
       OR CMAKE_CXX_FLAGS MATCHES "fsanitize=(address|leak)")
        add_executable(lsan_selftest engine/lsan_selftest.c
            support/lsan_suppressions.c)
        add_test(NAME lsan_selftest COMMAND lsan_selftest)
        set_tests_properties(lsan_selftest PROPERTIES
            ENVIRONMENT "TZ=UTC"
            PASS_REGULAR_EXPRESSION "detected memory leaks")
    endif()

    # 7-Zip is the only source-built shared library, so it is the only one whose
    # location a copied binary depends on. Not a Catch2 case: it reads the built
    # app's dynamic section and the install tree, not the archive code. RUNPATH
    # must be relative and must name the lib/ the library installs into, so a
    # binary copied off the build tree still loads lib7zip.so. Linux-only: ELF
    # RUNPATH, and the bundler that consumes this layout is the Linux one.
    add_test(NAME sevenzip_packaging_test
        COMMAND ${CMAKE_COMMAND}
            -DGMM_BUILD_DIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/sevenzip_packaging_test.cmake)
endif()
