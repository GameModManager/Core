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
target_include_directories(gmm_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(gmm_test_support
    PUBLIC Catch2::Catch2
    PRIVATE Qt6::Core)

# ---------------------------------------------------------------------------
# Cross-platform abstraction ratchet
# ---------------------------------------------------------------------------
# All OS-specific code goes through src/platform/. This static scan of src/
# fails when a file outside src/platform/ gains a direct OS header include, an
# OS preprocessor branch (#ifdef _WIN32, Q_OS_WIN, __APPLE__, ...), or an OS
# path literal. There are ~210 pre-existing occurrences, so this is a ratchet
# rather than a zero-tolerance check: findings are compared against
# platform_abstraction_baseline.txt keyed on (file, construct) with a count, so
# an unrelated edit elsewhere in src/ never moves it, while any NEW or EXTRA
# violation fails. Resolved violations also fail, so the baseline stays honest:
#   python3 tests/platform_abstraction_scan.py --src <root>/src \
#       --baseline <root>/tests/platform_abstraction_baseline.txt --update
# Pure text scan with no OS dependence, so it is registered here rather than in
# the POSIX-only block at the bottom of this file.
add_test(NAME platform_abstraction_test
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/platform_abstraction_scan.py
        --src ${PROJECT_SOURCE_DIR}/src
        --baseline ${CMAKE_CURRENT_SOURCE_DIR}/platform_abstraction_baseline.txt)

# ---------------------------------------------------------------------------
# Platform contract: pure virtual coverage + OS-arm completeness
# ---------------------------------------------------------------------------
# Only one of Linux/Windows/macOS is ever compiled, so a pure virtual that an
# uncompiled adaptor does not override - or a GMM_PLATFORM chain missing an OS
# arm - cannot fail any build we run. That is exactly how app/core.cpp shipped a
# null Platform* on macOS. This checks both structurally:
#   1. every pure virtual on Platform and FileTypeDispatcher is overridden by
#      all three adaptors, with a body in the header or a definition in the .cpp
#   2. every multi-arm GMM_PLATFORM chain ends in a populated #else, and a
#      chain that names a concrete adaptor class names all three OSes
# Pure text with no OS dependence, so it runs everywhere. Run it by hand with
#   python3 tests/platform_contract_scan.py --src src --verbose
add_test(NAME platform_contract_test
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/platform_contract_scan.py
        --src ${PROJECT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# ABI boundary: SDK veneer must not leak into Core (v3 P6)
# ---------------------------------------------------------------------------
add_test(NAME abi_boundary_test
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/abi_boundary_scan.py
        --src ${PROJECT_SOURCE_DIR}/src
        --cmake-root ${PROJECT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Windows compile check (Workspace-kgvn)
# ---------------------------------------------------------------------------
# Windows platform module: CMake picks GMM_PLATFORM from the host, so a
# Windows-only compile error ships silently. Where the mingw cross-compiler is
# installed, this syntax-checks every source under src/platform/windows/
# against the real Windows headers. It skips cleanly where it is not.
#   python3 tests/mingw_windows_syntax_check.py --root <root>
add_test(NAME windows_syntax_check
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/mingw_windows_syntax_check.py
        --root ${PROJECT_SOURCE_DIR}
        --skip-if-missing)

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
gmm_catch2_test(conflict_resolver_test engine/conflict_resolver_test.cpp)

# The mod list's AND/OR filter join: a Qt-free predicate, so it is tested here
# rather than through the widget that feeds it.

# The crash handler, verified by crashing a forked child on purpose and
# reading the dump back. dladdr() (inside backtrace_symbols) only sees .dynsym,
# so the test can only assert on the crashing function's NAME when the
# executable exports its symbols - the same reason the app links -rdynamic.
gmm_catch2_test(crash_handler_test engine/crash_handler_test.cpp)
if(NOT WIN32 AND NOT APPLE)
    target_link_options(crash_handler_test PRIVATE -rdynamic)
endif()
gmm_catch2_test(instance_router_test engine/instance_router_test.cpp)
gmm_catch2_test(fresh_install_test engine/fresh_install_test.cpp)
gmm_catch2_test(pipeline_test engine/pipeline_test.cpp)
# A Replace overwrite keeps the mod's load-order position (priority +
# separator_id) across the folder delete. Filesystem only, no UI.
gmm_catch2_test(install_overwrite_order_test engine/install_overwrite_order_test.cpp
    PROPERTIES TIMEOUT 120)
gmm_catch2_test(plugin_database_test engine/plugin_database_test.cpp)
gmm_catch2_test(instance_path_test engine/instance_path_test.cpp)
# Per-game category sets: the Skyrim-shows-Isaac regression, and the Nexus
# per-game category import. TIMEOUT set so a regression that wedges goes red
# instead of hanging CI (Workspace-tfp7).
# Nexus mod-update sweep: the cost rule (a settled library owes no per-mod
# request) and the refusal contract (a refused check is never renderable as
# "no updates"). TIMEOUT because a case that reaches libcurl would sit in a
# 30s DNS timeout rather than failing fast.
# The page-update verdict ladder (pure) and the LoversLab/ModPub sweep over the
# injected FakeNetworkManager: a readable page becomes a verdict, a failed
# scrape writes NOTHING, a page that states nothing gets no verdict. TIMEOUT
# for the same reason as the sweep test above.
# A digest mismatch must discard the bad bytes, not leave a resumable orphan
# (Workspace-wrx7). TIMEOUT for the same reason as the three above.
gmm_catch2_test(profile_test engine/profile_test.cpp)
gmm_catch2_test(profile_switching_test engine/profile_switching_test.cpp)

# Workspace-czc0: timestamped backup / restore points of a profile's load order
# and mod list. Qt-free, so the whole of the feature's risk (stamp naming, the
# keep-10 eviction, the unconditional pre-restore safety backup and the
# per-file restore reporting) is covered here without a GUI.
gmm_catch2_test(backup_service_test engine/backup_service_test.cpp)
# gmm_platform/gmm_runtime as well as gmm_engine: the Proton precedence cases
# below drive the real LinuxPlatform and ProtonRuntime rather than a double, so
# the defaults under test are the ones production computes from a real Steam
# install. TIMEOUT because those cases repoint HOME and read the environment:
# a wedge there has to go red rather than hang the suite.

# Workspace-cch0: headless CLI launch honors per-executable args/env/cwd -
# Qt-free splitter/cwd/entry-lookup helpers mirrored from the GUI path.

# Workspace-cch0: headless CLI --args/--env/--cwd parsing plus the
# Config/entry -> LaunchPrepRequest -> LaunchParams mapping (no spawn).
# Reading .git/config the way git writes it - tabbed, quoted subsection name -
# from a mod folder under a game path containing spaces. A pure file read, so
# this runs whether or not git is installed.
gmm_catch2_test(uri_router_test engine/uri_router_test.cpp)
gmm_catch2_test(scanner_test engine/scanner_test.cpp)
# The enable/disable sentinel reason: a refused write has to report what the
# OS said, and a successful one has to report nothing. Runs against a game
# with no plugin so the engine default sentinel is the code under test.
# Negative redaction tests at the log call sites: a signed URL or an
# unparseable env entry must not reach gamemodmanager.log with its secret.
gmm_catch2_test(redaction_callsite_test engine/redaction_callsite_test.cpp)
gmm_catch2_test(conflict_test engine/conflict_test.cpp)
gmm_catch2_test(fs_utils_test engine/fs_utils_test.cpp)
gmm_catch2_test(local_saves_test engine/local_saves_test.cpp)
# Settings > Workarounds: the QSettings read reaching the engine predicates.
gmm_catch2_test(deploy_case_insensitive_test engine/deploy_case_insensitive_test.cpp)
gmm_catch2_test(deploy_parallel_test engine/deploy_parallel_test.cpp)
gmm_catch2_test(deploy_direct_test engine/deploy_direct_test.cpp)
# The --list-games document: derived from plugin registration plus
# managed_games.json, validated against a checked-in sample. TIMEOUT because a
# case that opens the Workshop SQLite client would sit on a lock rather than
# failing fast.

# 7-Zip IArchive backend: the RAR routing table and the large-dictionary and
# encrypted-RAR cases libarchive's RAR5 reader refuses outright.
gmm_catch2_test(sevenzip_archive_test engine/sevenzip_archive_test.cpp)

# Tests that include third-party headers directly need those include dirs at
# compile time (the engine library's PRIVATE deps only propagate to the link
# line, not to consumers' compile step).
gmm_catch2_test(save_parser_test engine/save_parser_test.cpp
    LIBS PkgConfig::ZLIB PkgConfig::LZ4)

# Proton tool-running: runner override resolution + run guards (needs runtime).

# IconManager resolution chain — offscreen GUI test (QApplication for SP
# fallback). icon_manager.cpp lives in gmm_ui (it is Qt-using); theme_manager
# comes from gmm_engine.

# Live OS keyring roundtrip — only when QtKeychain is available; skips
# cleanly when no keyring daemon is running. qtkeychain_keyring.cpp is not in
# any library (it is compiled into the app only), so compile it here.

# Cross-platform ratchet equivalence tests. These exercise the OS primitives
# that were moved out of engine/ into src/platform/ (Workspace-fjx8), and prove
# each one reaches a real consumer rather than just returning a value. Needs
# gmm_platform because the functions under test are free functions defined in
# the adaptor for the OS being built.
gmm_catch2_test(platform_primitives_test engine/platform_primitives_test.cpp
    LIBS gmm_platform)

# Linux platform module (mimeapps / XDG handling).
if(GMM_PLATFORM STREQUAL "linux")
    gmm_catch2_test(mimeapps_test engine/mimeapps_test.cpp
        LIBS gmm_platform)
endif()

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
add_library(gmm_multi_game_plugin MODULE
    engine/plugins/multi_game_plugin.cpp
)
target_include_directories(gmm_multi_game_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
set_target_properties(gmm_multi_game_plugin PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

add_library(gmm_v3_loader_fixture MODULE
    engine/plugins/v3_loader_fixture.cpp
)
target_include_directories(gmm_v3_loader_fixture PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
set_target_properties(gmm_v3_loader_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

add_library(gmm_v3_version_mismatch_fixture MODULE
    engine/plugins/v3_version_mismatch_fixture.cpp
)
target_include_directories(gmm_v3_version_mismatch_fixture PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
set_target_properties(gmm_v3_version_mismatch_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

# Same source, two targets: colliding game_id, distinct display names. The
# determinism test loads both from one directory and asserts filename order.
add_library(gmm_collide_plugin_a MODULE
    engine/plugins/collide_game_plugin.cpp
)
target_include_directories(gmm_collide_plugin_a PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(gmm_collide_plugin_a PRIVATE
    GMM_COLLIDE_DISPLAY_NAME="Collide Alpha")
set_target_properties(gmm_collide_plugin_a PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

add_library(gmm_collide_plugin_b MODULE
    engine/plugins/collide_game_plugin.cpp
)
target_include_directories(gmm_collide_plugin_b PRIVATE
    ${PROJECT_SOURCE_DIR}/external/abi/include)
target_compile_definitions(gmm_collide_plugin_b PRIVATE
    GMM_COLLIDE_DISPLAY_NAME="Collide Beta")
set_target_properties(gmm_collide_plugin_b PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins"
)

if(APPLE)
    foreach(_fixture gmm_override_mod_data_checker gmm_fomod_stage_plugin gmm_settings_tab_plugin gmm_multi_game_plugin gmm_collide_plugin_a gmm_collide_plugin_b gmm_v3_loader_fixture gmm_v3_version_mismatch_fixture)
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

# v3 P0 RegistrationSink parity - the real Skyrim plugin through the ABI
# bridge (route A) must produce the same engine state as a direct
# GameDescriptor + RegistrationSink replay (route B). GMM_SKYRIM_PLUGIN_VERSION
# mirrors the VERSION define the plugin target compiles with (plugin-local, so
# it does not propagate to tests).

# v3 P1 N-records + determinism - register_game twice yields two
# game_plugins() rows; colliding game_ids resolve in filename order.

# v3 P3 loader - v3 fixture loads, v2 fixture still loads, version mismatch refused.
gmm_catch2_test(plugin_v3_loader_test engine/plugin_v3_loader_test.cpp
    DEFINES
        GMM_V3_LOADER_FIXTURE_PATH="$<TARGET_FILE:gmm_v3_loader_fixture>"
        GMM_V3_VERSION_MISMATCH_FIXTURE_PATH="$<TARGET_FILE:gmm_v3_version_mismatch_fixture>"
        GMM_SKYRIM_PLUGIN_PATH="$<TARGET_FILE:SkyrimSpecialEdition>"
)
add_dependencies(plugin_v3_loader_test gmm_v3_loader_fixture gmm_v3_version_mismatch_fixture SkyrimSpecialEdition)
# P1.4 host UI bridge test — a plugin claims the Fomod stage and runs the
# install through GmmHostUi::fomod_wizard. The fixture plugin resolves the
# gmm_* accessors through dlopen — same export requirement as the app.
# The fixture plugin resolves gmm_* accessors through dlopen — same export
# requirement as the app. Linux needs -rdynamic; macOS needs -export_dynamic
# or the bundle's load-time flat lookup can't see the executable's symbols.
# Nothing references abi_bridge.cpp at link time, so also force its archive
# member out of libgmm_engine (-u) or the accessors are absent entirely and
# the plugin fails to load with "symbol not found in flat namespace".

# StageRegistry wildcard claim resolution — pure engine test, no Qt, no UI.

# ---------------------------------------------------------------------------
# UI tests — offscreen GUI tests, link the UI library (which carries Qt and
# the engine transitively)
# ---------------------------------------------------------------------------






# The three updater settings and the read-only install-method line: asserts on
# what each setting's consumer renders, not on a getter round-trip.

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

# A profile switch has to move the DEPLOY SET, not just the checkbox column:
# deploy_utils.cpp:389 reads only the on-disk sentinel, so a profile whose
# enable/disable never reaches disk is decorative. Asserts on the real deploy
# strategy's output over the real mods dir, driven through the real MainWindow
# with the REAL Isaac plugin - a model-only assertion passes while the bug
# stands. Also pins that a mod no profile lists keeps its sentinel, which is the
# state an install leaves and install_stage.cpp carries across an overwrite.
gmm_catch2_test(profile_deploy_state_test ui/profile_deploy_state_test.cpp
    UI
    LIBS gmm_ui Qt6::Test ${CMAKE_DL_LIBS}
    DEFINES GMM_ISAAC_PLUGIN_PATH="$<TARGET_FILE:TheBindingOfIsaacRebirth>")
add_dependencies(profile_deploy_state_test TheBindingOfIsaacRebirth)

# Checkerboard transparency grid (PreviewWindow / ImageViewer): reference
# tile colors, 2x2 menu icons, transparency gating, Settings persistence.
# Offscreen; throwaway XDG_CONFIG_HOME so the Settings section is hermetic.

# PreviewWindow image auto-fit: small images scale up, large images scale
# down, resize re-fits. Offscreen; throwaway XDG_CONFIG_HOME (hermetic).


# The Git source actions against a real repository and a real git binary -
# the reset's scope guard and its "says what it discards" promise are only
# meaningful there, and a mock would assert the mock. Skips when git is
# absent. Local paths only, no network.




# The per-file "<archive>.meta" download state (MO2's format) and the one-way
# migration off the single .download_manifest.json. TIMEOUT is mandatory: this
# creates a QApplication and touches the filesystem in six cases.



gmm_catch2_test(conflict_send_test ui/conflict_send_test.cpp
    UI
    LIBS gmm_ui)






# Export wizard pack building runs on a worker thread (the build walks every
# mod folder and hashes every bundled byte). Offscreen not needed: QCoreApplication.



# DataTab incremental conflict update on mod reorder (Workspace-z1iu):
# reordered winners update in place instead of clearing + recreating the
# whole tree. Offscreen; pumps the worker build + chunked apply.

# Double-click opens previews (Workspace-co2 + Workspace-636): the
# Settings::double_clicks_open_previews round-trip, the DataTab
# setting x modifier x file-type matrix, and the plain double-click
# integration. Offscreen; throwaway XDG_CONFIG_HOME + /tmp/opencode tree.

# Data tab file recognition: every real file row carries a size and the open
# action hands a .txt to the OS handler, whatever the file name's casing (the
# registry keys are CI-normalized, so mixed-case names used to resolve to
# nothing - no size, and open/preview silently did nothing). Offscreen;
# throwaway /tmp/opencode tree; pumps the worker build + chunked apply.

# Data tab vs instance switch (Workspace-0sp3): the tab is the one
# right-panel tab that survives a switch, and nothing used to clear it -
# the previous instance's rows stayed visible until the new instance's
# first conflict scan landed (forever for a game-less instance). Drives the
# real MainWindow + set_game_info switch path; offscreen, throwaway /tmp
# root, no network.
gmm_catch2_test(data_tab_instance_switch_test
    ui/data_tab_instance_switch_test.cpp
    UI
    LIBS gmm_ui PkgConfig::ZLIB
    # Real window + worker threads: a ceiling rather than hanging forever.
    PROPERTIES TIMEOUT 300)

# BBCode adapter + entity/BR normalization (ui/modinfo/bbcode.cpp). Pure
# logic - no QApplication, no panel, no event loop. Reaches gmm_ui for
# the bbcode_to_html symbol but does not need the QT_QPA_PLATFORM=offscreen
# environment.

# Tray close/restore decisions (ui/system_tray/tray_decision.h). Pure logic
# over a facts struct - no QApplication, no tray, no window, and no link
# against gmm_ui either, since the decision is a header-only inline function.

# What an nxm:// collection link click does (ui/nxm/collection_click.h). Same
# shape as the tray decisions: pure logic over a facts struct, so the three-way
# choice is reachable without a display.

# DescriptionBrowser (ui/modinfo/description_browser.cpp) - the QTextBrowser
# subclass that loads remote [img] URLs via QNetworkAccessManager. Needs a
# QApplication for the loadResource override to be exercisable, so this is
# a UI test (offscreen).

gmm_catch2_test(plugins_tab_test ui/plugins_tab_test.cpp
    UI
    DEFINES GMM_TEST_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
    LIBS gmm_ui Qt6::Test)

gmm_catch2_test(saves_tab_test ui/saves_tab_test.cpp
    UI
    LIBS gmm_ui Qt6::Test PkgConfig::ZLIB)


# The Instance Options toolbar button keeps both halves: body opens the
# panel, side arrow opens the dropdown (QToolButton splits the two by hit
# test, so real mouse events are needed to reach it).

# The LOOT report popup body: MO2's General messages section plus the stats
# line, with each message at the level LOOT gave it.


# Scroll-frame cost measurement (Workspace-lb45). Prints the per-frame model
# cost of one mod-list repaint; TIMEOUT so a wedged measurement goes red
# instead of hanging CI.


# Settings > Workarounds > Network: the custom browser actually receives the URL
# (a real recorder program is started, not just a getter round-trip).

# Settings > Windows > "Center dialogs on screen": where a restored dialog lands.

# Settings > General > "Confirm before switching game instance".

# The restart-only instance switch wiring (Workspace-gb0b phase 2): the real
# switch_to_instance driven through the injected restart seam - relaunch
# requested, no live mutation, last instance written; the OFF fallback stays
# live. Real window + worker threads: a ceiling rather than hanging forever.
gmm_catch2_test(instance_switch_restart_test
    ui/instance_switch_restart_test.cpp
    UI
    LIBS gmm_ui
    PROPERTIES TIMEOUT 300)

# Last-used directory per named file dialog (MO2 FileDialogMemory).


# Change Categories commit-on-hide: opening the submenu and leaving it alone
# must not write, and a real edit still must.

# A cross-profile mod rename that only reaches some profiles must tell the
# user which ones missed out, by name, through the real dialog.

# All Mods > Enable/Disable must reach mods inside a folded separator, not
# silently skip them (the view's row visibility conflates filter and fold).

# A nested mod's checkbox click must toggle it: the IndentDelegate draws the
# checkbox shifted right by the nesting depth, so the click hit-test has to
# follow the shift (a real mouse click on the drawn checkbox, no shims).





# Workspace-vwa: "<Edit...>" sentinel must open the editor even with 0
# executables; rebuilds must never open it.


# Elevated launch planning + the per-executable elevation field. Asserts the
# constructed argv only - nothing here executes pkexec/osascript/fakeroot, and
# the fakeroot probe is injected so both branches are deterministic.

# Workspace-421: executables seeding also scans macOS app roots outside
# game_dir (~/Applications, /Applications) and stores those hits absolute.



# Workspace-0713: DebugWindow overhaul smoke test. Pushes samples into the
# rolling chart widget to make sure neither QtCharts nor the QPainter
# fallback path crashes.

# Workspace-tnj: set_game_info guard teardown — banner visibility + game-less
# instance mod ops.

# Status bar: context label (game - instance - profile), the metered-source
# rule that keeps a per-source readout only for a source with a budget, the
# separator that must not dangle, and the transient status timeout.

# lock_gui: the Workarounds gate on ui::Locker. Off suppresses a plain LockUI
# lock; OutputRequired overrides it, which is the branch a suppression-only
# test would not catch.





# FOMOD plugin integration — loads the FomodInstaller plugin, verifies its
# settings tab appears in the Settings dialog with correct settings and
# persistence. Also documents whether the old standalone FOMOD tab still exists.

# Workspace-52hc: reusable ui::TaskDialog (MO2 U267 port) + the
# Settings::dialog_choice remember seam + the Remove Mods walking-skeleton
# consumer. TDD red phase: the component does not exist yet, so these three
# fail to compile until src/ui/widgets/task_dialog.{h,cpp}, the Settings
# seam, and ui::configure_remove_mods_dialog land.


# Workspace-kxm7: Settings General-tab "Reset dialog choices" button closing
# the TaskDialog remember loop (MO2 QuestionBoxMemory-reset parity).

# Sibling of the above: "Reset dialog sizes and positions" must clear the four
# per-dialog geometry keys the dialogs restore from, not the unrelated
# center_dialogs boolean that shared the group it used to remove.

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

gmm_catch2_test(mod_list_filter_grammar_test ui/mod_list_filter_grammar_test.cpp
    LIBS gmm_ui Qt6::Core)


# The settings migration: a data-loss path, so it gets the one test the design
# note calls for. Drives settings_migration::migrate() - the exact call the
# private Settings ctor makes - against its own sandboxed QSettings handle with
# its own move table, seeds awkward values (escapes, ini separators, a newline,
# empty string, empty list, QColor, QDateTime, QByteArray) and asserts they
# cross verbatim. Non-UI: no widget, no event loop.
gmm_catch2_test(settings_migration_test ui/settings_migration_test.cpp
    LIBS gmm_ui Qt6::Core)




# Workspace-nef3: instance-switch restart confirmation (MO2 U044 parity).
# ui::configure_instance_restart_dialog seam + the switch_to_instance wiring.

# Workspace-t6z5: TaskDialog adoption for the overwrite confirmations
# (clear-overwrite + Overwrite info-dialog delete). configure_* seams plus
# the clear_overwrite / delete_selected wiring.

# Workspace-t373: launch-failure details dialogs (spawn-error parity).
# ui::configure_executable_unreachable_dialog +
# ui::configure_launch_failed_dialog seams + the on_launch_params_prepared
# wiring.

# Workspace-axva: reusable UI primitives pack (MO2 U264 / U265 / U269).
# SortableTreeWidget + set_customizable_columns, ExpanderWidget, LinkLabel,
# LineEditClear and the generic EventFilter, one test case per primitive.

# Workspace-so1s: global ui::report_error / ui::critical_on_top error popup
# (MO2 U268 port). TDD red phase: fails to compile until
# src/ui/widgets/error_popup.{h,cpp} exists.

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
    LIBS gmm_ui Qt6::Test PkgConfig::ZLIB
    # A real window, real modals and a worker thread all in one binary: this is
    # the target that hangs offscreen, so it gets a ceiling rather than
    # running until ctest's global default.
    PROPERTIES TIMEOUT 300)

# The whole mod pipeline in one case: download -> install -> verify -> a newer
# archive -> install over the top -> verify again -> the update check. Real
# window, real downloads table, real PipelineWorker running the real stages,
# two real zips built with libarchive, and the two install modals answered by
# clicking their real buttons. libarchive is listed because the case WRITES the
# archives itself rather than shipping a stand-in the extractor never sees.
gmm_catch2_test(mod_pipeline_e2e_test ui/mod_pipeline_e2e_test.cpp
    UI
    LIBS gmm_ui Qt6::Test PkgConfig::LIBARCHIVE
    # Two full installs with real modal round trips, so the same ceiling as the
    # window harness.
    PROPERTIES TIMEOUT 300)

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

# The wheel must not switch a CLOSED dropdown anywhere in the app: the two
# beside the mod list hide rows or reload a different profile, the program
# dropdown changes what Run launches. Drives the REAL MainWindow offscreen and
# delivers the wheel through QTest::wheelEvent (the platform dispatch path), so
# it covers both the block and the fact that the combo is the widget Qt hands
# the wheel to. Covers a combo nobody wired a filter to by hand plus one outside
# any app widget tree, and carries the guard-off control that MUST move, so
# neither a dead dispatch nor a guard that stopped filtering can pass.

# In-window log view retention. The panel is fed by engine::Logger for the
# whole session, so the rendered document needs an on-screen window or it grows
# without bound; this pins the window and that the tail is what survives.

# MO2 menu parity - the Help tree (order, the game-support-URL condition, the
# stubbed Tutorials submenu) and the keyboard table read off the real
# AppMenuBar. The menu bar owns every action and its shortcut, so it is built
# standalone: no MainWindow, no game, no instance, no files touched.

# The Show Menu Bar toggle hides the bar the View menu lives in, so the restore
# is a key event on the real window (MO2 MainWindow::keyReleaseEvent). Needs
# Qt6::Test to synthesize the Alt release.

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
