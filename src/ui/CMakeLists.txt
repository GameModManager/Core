# UI — Qt lives here. Static library linking Qt6 and the Qt-free engine.
# The Qt-using files that used to live under src/engine/ (theme/style_manager,
# theme/icon_manager, notify/notification_backend, nxm/nxm_ipc,
# app/single_instance) were relocated here (issue/33) so gmm_engine stays
# Qt-free at the CMake level.

set(UI_SOURCES
    settings/settings.cpp
    settings/settings_dialog.cpp
    settings/source_pages.cpp
    settings/categories_dialog.cpp
    main_window/main_window.cpp
    main_window/loot_sort_worker.cpp
    main_window/conflict_scan_worker.cpp
    main_window/mod_scan_worker.cpp
    main_window/plugin_db_load_worker.cpp
    main_window/saves_scan_worker.cpp
    main_window/data_tab_build_worker.cpp
    main_window/deploy_worker.cpp
    controllers/launch_controller.cpp
    controllers/queue_controller.cpp
    controllers/overwrite_controller.cpp
    controllers/downloads_controller.cpp
    controllers/mod_list_controller.cpp
    controllers/settings_controller.cpp
    widgets/mod_list_model.cpp
    widgets/mod_table_view.cpp
    widgets/main_toolbar.cpp
    widgets/profile_bar.cpp
    widgets/right_panel.cpp
    widgets/exec_controls_bar.cpp
    widgets/exec_entry_dialog.cpp
    widgets/console_panel.cpp
    widgets/gmm_status_bar.cpp
    widgets/instance_switcher_dialog.cpp
    widgets/game_icon_cache.cpp
    widgets/instance_statistics_dialog.cpp
    widgets/debug_window.cpp
    widgets/pipeline_window.cpp
    widgets/zoom_controls.cpp
    widgets/mod_filter_bar.cpp
    widgets/category_filter_panel.cpp
    widgets/right_filter_bar.cpp
    widgets/column_toggle_header.cpp
    widgets/list_dialog.cpp
    widgets/menu_bar.cpp
    panels/downloads_tab.cpp
    panels/plugins_tab.cpp
    panels/archives_tab.cpp
    panels/data_tab.cpp
    panels/saves_tab.cpp
    panels/conflicts_tab.cpp
    workers/pipeline_worker.cpp
    preview/anm2_parser.cpp
    preview/preview_widget.cpp
    preview/preview_window.cpp
    game_selection/game_card.cpp
    game_selection/game_selection_widget.cpp
    overwrite/sync_overwrite_dialog.cpp
    overwrite/move_to_mod_dialog.cpp
    overwrite/overwrite_info_dialog.cpp
    overwrite/query_overwrite_dialog.cpp
    install/install_name_dialog.cpp
    install/install_progress_dialog.cpp
    fomod/fomod_image_viewer.cpp
    fomod/fomod_wizard_dialog.cpp
    proton/proton_panel.cpp
    # Qt-using files relocated from src/engine/ (issue/33) — keeps gmm_engine Qt-free
    ${PROJECT_SOURCE_DIR}/src/ui/theme/style_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/theme/icon_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/notify/notification_backend.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/nxm/nxm_ipc.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/app/single_instance.cpp
)

# FileViewer module — reusable file viewers (image/video/3D-scene). The video
# page needs QtMultimedia; without it the container shows the unsupported page.
set(FILE_VIEWER_SOURCES
    viewer/file_viewer_widget.cpp
    viewer/file_viewer.cpp
    viewer/image_viewer.cpp
    viewer/scene_viewer.cpp
)
if(Qt6Multimedia_FOUND)
    list(APPEND FILE_VIEWER_SOURCES viewer/video_viewer.cpp)
endif()
list(APPEND UI_SOURCES ${FILE_VIEWER_SOURCES})

# Mod Info dialog — the 9 MO2-parity tabs plus the dialog shell.
set(MODINFO_SOURCES
    modinfo/mod_info_data.cpp
    modinfo/mod_info_tab.cpp
    modinfo/mod_info_dialog.cpp
    modinfo/generic_files_tab.cpp
    modinfo/text_files_tab.cpp
    modinfo/config_files_tab.cpp
    modinfo/images_tab.cpp
    modinfo/esps_tab.cpp
    modinfo/conflicts_tab.cpp
    modinfo/categories_tab.cpp
    modinfo/source_tab.cpp
    modinfo/source_fetch_worker.cpp
    modinfo/notes_tab.cpp
    modinfo/filetree_tab.cpp
    modinfo/bbcode.cpp
)
list(APPEND UI_SOURCES ${MODINFO_SOURCES})

add_library(gmm_ui STATIC ${UI_SOURCES})

target_include_directories(gmm_ui PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/external/abi/include
    ${PROJECT_SOURCE_DIR}/third_party/pugixml
)

target_compile_definitions(gmm_ui PUBLIC VERSION="${PROJECT_VERSION}")

# Qt is PUBLIC: consumers (tests, the app) compile their own sources against
# Qt headers and link Qt, so the usage requirements must propagate.
target_link_libraries(gmm_ui PUBLIC
    Qt6::Widgets
    Qt6::Network
)
target_link_libraries(gmm_ui PRIVATE
    gmm_engine
    gmm_runtime
    gmm_platform
    PkgConfig::TOMLPLUSPLUS
)
if(Qt6Multimedia_FOUND)
    target_compile_definitions(gmm_ui PUBLIC GMM_HAS_MULTIMEDIA=1)
    target_link_libraries(gmm_ui PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
endif()
if(KF6SyntaxHighlighting_FOUND)
    target_compile_definitions(gmm_ui PUBLIC GMM_HAS_SYNTAX_HIGHLIGHTING=1)
    target_link_libraries(gmm_ui PRIVATE KF6::SyntaxHighlighting)
endif()
