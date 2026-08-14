# Test suite — only processed when GMM_BUILD_TESTS=ON (see top-level
# CMakeLists.txt). Tests link the per-directory libraries (gmm_engine,
# gmm_ui, gmm_runtime, gmm_platform) instead of re-listing engine sources;
# the libraries carry their transitive link dependencies.
#
# Every C++ test is a Catch2 v3 test: Catch2WithMain provides the runner
# main() and catch_discover_tests() registers each TEST_CASE as its own CTest
# test, so a failure in one case never hides the rest.

# ---------------------------------------------------------------------------
# Catch2 test helper
# ---------------------------------------------------------------------------
# gmm_catch2_test(<name> <sources...>
#     [UI]
#     LIBS <extra link libraries>
#     DEFINES <compile definitions>
#     PROPERTIES <extra CTest properties>)
#
# Builds a Catch2 test executable and registers every TEST_CASE with CTest.
# Every test links gmm_engine (its PUBLIC include dirs carry the project
# include roots); pass extra libraries via LIBS. All tests run with TZ=UTC
# (hermetic); UI tests additionally get QT_QPA_PLATFORM=offscreen so they run
# headless.
function(gmm_catch2_test NAME)
    set(options UI)
    set(oneValueArgs "")
    set(multiValueArgs "LIBS" "DEFINES" "PROPERTIES")
    cmake_parse_arguments(GCT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${NAME} ${GCT_UNPARSED_ARGUMENTS})
    target_link_libraries(${NAME} PRIVATE Catch2::Catch2WithMain gmm_engine ${GCT_LIBS})
    if(GCT_DEFINES)
        target_compile_definitions(${NAME} PRIVATE ${GCT_DEFINES})
    endif()

    set(_env "TZ=UTC")
    if(GCT_UI)
        list(APPEND _env "QT_QPA_PLATFORM=offscreen")
    endif()
    catch_discover_tests(${NAME}
        TEST_PREFIX "${NAME}."
        PROPERTIES ENVIRONMENT "${_env}" ${GCT_PROPERTIES}
    )
endfunction()

# ---------------------------------------------------------------------------
# Engine tests — Qt-free, link the engine library
# ---------------------------------------------------------------------------
gmm_catch2_test(pipeline_test engine/pipeline_test.cpp)
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
gmm_catch2_test(categories_test engine/categories_test.cpp)
gmm_catch2_test(mod_meta_test engine/mod_meta_test.cpp)
gmm_catch2_test(launch_params_test engine/launch_params_test.cpp)
gmm_catch2_test(game_icons_test engine/game_icons_test.cpp)
gmm_catch2_test(nxm_router_test engine/nxm_router_test.cpp)
gmm_catch2_test(scanner_test engine/scanner_test.cpp)
gmm_catch2_test(theme_test engine/theme_test.cpp)
gmm_catch2_test(keyring_test engine/keyring_test.cpp)
gmm_catch2_test(conflict_test engine/conflict_test.cpp)
gmm_catch2_test(overwrite_utils_test engine/overwrite_utils_test.cpp)
gmm_catch2_test(fs_utils_test engine/fs_utils_test.cpp)
gmm_catch2_test(local_saves_test engine/local_saves_test.cpp)
gmm_catch2_test(deploy_hidden_test engine/deploy_hidden_test.cpp)
gmm_catch2_test(deploy_case_insensitive_test engine/deploy_case_insensitive_test.cpp)
gmm_catch2_test(root_override_test engine/root_override_test.cpp)
gmm_catch2_test(deploy_binary_test engine/deploy_binary_test.cpp)
gmm_catch2_test(deploy_parallel_test engine/deploy_parallel_test.cpp)
gmm_catch2_test(nexus_http_test engine/nexus_http_test.cpp)
gmm_catch2_test(nexus_provider_test engine/nexus_provider_test.cpp)
gmm_catch2_test(loverslab_auth_test engine/loverslab_auth_test.cpp)
gmm_catch2_test(install_progress_test engine/install_progress_test.cpp)
gmm_catch2_test(perf_benchmark perf/benchmark_test.cpp)

# Tests that include third-party headers directly need those include dirs at
# compile time (the engine library's PRIVATE deps only propagate to the link
# line, not to consumers' compile step).
gmm_catch2_test(save_parser_test engine/save_parser_test.cpp
    LIBS PkgConfig::ZLIB PkgConfig::LZ4)
gmm_catch2_test(filetree_test engine/filetree_test.cpp
    LIBS PkgConfig::LIBARCHIVE)
gmm_catch2_test(fomod_installer_test engine/fomod_installer_test.cpp
    LIBS PkgConfig::NLOHMANN_JSON)

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

# P1.2 GameFeatureRegistry — priority + replace registration, combined
# ModDataChecker, and the exit criterion (a test plugin overrides the Skyrim
# plugin's data-checker).
gmm_catch2_test(game_feature_registry_test engine/game_feature_registry_test.cpp
    DEFINES
        GMM_OVERRIDE_PLUGIN_PATH="${CMAKE_BINARY_DIR}/test_plugins/gmm_override_mod_data_checker${CMAKE_SHARED_LIBRARY_SUFFIX}"
        GMM_SKYRIM_PLUGIN_PATH="${CMAKE_BINARY_DIR}/plugins/SkyrimSpecialEdition${CMAKE_SHARED_LIBRARY_SUFFIX}"
)
add_dependencies(game_feature_registry_test SkyrimSpecialEdition gmm_override_mod_data_checker)

# P1.4 host UI bridge test — a plugin claims the Fomod stage and runs the
# install through GmmHostUi::fomod_wizard. The fixture plugin resolves the
# gmm_* accessors through dlopen — same export requirement as the app.
gmm_catch2_test(fomod_stage_plugin_test engine/fomod_stage_plugin_test.cpp
    DEFINES
        GMM_FOMOD_PLUGIN_PATH="${CMAKE_BINARY_DIR}/test_plugins/gmm_fomod_stage_plugin${CMAKE_SHARED_LIBRARY_SUFFIX}"
)
if(NOT WIN32 AND NOT APPLE)
    target_link_options(fomod_stage_plugin_test PRIVATE -rdynamic)
endif()
add_dependencies(fomod_stage_plugin_test gmm_fomod_stage_plugin)

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

gmm_catch2_test(flags_delegate_test ui/flags_delegate_test.cpp
    LIBS gmm_ui)

gmm_catch2_test(settings_sources_tab_test ui/settings_sources_tab_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS})

gmm_catch2_test(source_tab_test ui/source_tab_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(downloads_tab_test ui/downloads_tab_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(pipeline_worker_test ui/pipeline_worker_test.cpp
    LIBS gmm_ui Qt6::Core)

gmm_catch2_test(plugin_db_load_test ui/plugin_db_load_test.cpp
    LIBS gmm_ui Qt6::Core)

gmm_catch2_test(data_tab_test ui/data_tab_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(generic_files_tab_test ui/generic_files_tab_test.cpp
    UI
    LIBS gmm_ui)
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(generic_files_tab_test PRIVATE KF6::SyntaxHighlighting)
endif()

gmm_catch2_test(plugins_tab_test ui/plugins_tab_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(saves_tab_test ui/saves_tab_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(mod_list_model_test ui/mod_list_model_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(exec_entry_dialog_test ui/exec_entry_dialog_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(executables_parser_test ui/executables_parser_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(overwrite_dialogs_test ui/overwrite_dialogs_test.cpp
    UI
    LIBS gmm_ui)

gmm_catch2_test(list_dialog_test ui/list_dialog_test.cpp
    UI
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(proton_panel_test ui/proton_panel_test.cpp
    UI
    LIBS gmm_ui ${CMAKE_DL_LIBS}
    DEFINES GMM_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins")

gmm_catch2_test(install_progress_dialog_test ui/install_progress_dialog_test.cpp
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
endif()