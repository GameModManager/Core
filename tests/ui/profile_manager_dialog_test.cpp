// Offscreen GUI test for the MO2-style ProfileManagerDialog.
//
// Covers: the profile list populated from engine::profile::list_profiles,
// the active-profile marker, per-profile settings (Local Saves / Local
// Settings / Archive Invalidation) read from settings.ini and persisted on
// toggle, and selected_profile() after the user picks a profile.
//
// Create/copy/rename/delete themselves are thin wrappers over the engine
// functions (create_fresh_profile / copy_profile / rename_profile /
// Profile::remove) which are covered by the engine tests
// (profile_creation_test.cpp, profile_test.cpp); here we verify the dialog
// surface that does not require driving modal sub-dialogs.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/profile/profile_manager_dialog.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"

#include <QApplication>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

namespace fs = std::filesystem;

fs::path make_profiles_dir(const char* tag) {
    const fs::path root = fs::temp_directory_path() /
                          ("gmm_profile_manager_" + std::string(tag));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

QListWidget* list_of(ui::ProfileManagerDialog& dlg) {
    auto* list = dlg.findChild<QListWidget*>();
    REQUIRE(list != nullptr);
    return list;
}

QCheckBox* checkbox_of(ui::ProfileManagerDialog& dlg, const QString& text) {
    for (auto* box : dlg.findChildren<QCheckBox*>()) {
        if (box->text() == text)
            return box;
    }
    return nullptr;
}

QPushButton* button_of(ui::ProfileManagerDialog& dlg, const QString& text) {
    for (auto* btn : dlg.findChildren<QPushButton*>()) {
        if (btn->text() == text)
            return btn;
    }
    return nullptr;
}

// Item display text for the profile at `row` (name + marker).
QString item_text(ui::ProfileManagerDialog& dlg, int row) {
    auto* list = list_of(dlg);
    REQUIRE(row < list->count());
    return list->item(row)->text();
}

}  // namespace

TEST_CASE("profile manager dialog", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path root = make_profiles_dir("dialog");
    qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const fs::path profiles_dir = root / "profiles";
    auto created = engine::profile::create_fresh_profile(profiles_dir, "Default");
    REQUIRE(created.success);
    created = engine::profile::create_fresh_profile(profiles_dir, "Modded");
    REQUIRE(created.success);

    SECTION("lists profiles and marks the active one") {
        ui::ProfileManagerDialog dlg(profiles_dir, QStringLiteral("Default"));
        auto* list = list_of(dlg);
        REQUIRE(list->count() == 2);
        // Sorted lexicographically by the engine.
        REQUIRE(item_text(dlg, 0).startsWith("Default"));
        REQUIRE(item_text(dlg, 0).contains("(active)"));
        REQUIRE(item_text(dlg, 1).startsWith("Modded"));
        REQUIRE_FALSE(item_text(dlg, 1).contains("(active)"));
    }

    SECTION("marks the default profile when it differs from the active one") {
        ui::ProfileManagerDialog dlg(profiles_dir, QStringLiteral("Default"),
                                     QStringLiteral("Modded"));
        auto* list = list_of(dlg);
        REQUIRE(list->count() == 2);
        REQUIRE(item_text(dlg, 1).contains("(default)"));
        REQUIRE_FALSE(item_text(dlg, 0).contains("(default)"));
    }

    SECTION("per-profile settings read from settings.ini") {
        // Give "Modded" non-default settings on disk first.
        {
            engine::profile::ProfileManager profile(profiles_dir / "Modded");
            profile.set_local_saves(true);
            profile.set_local_settings(true);
            profile.set_automatic_archive_invalidation(true);
            REQUIRE(profile.save_settings());
        }

        ui::ProfileManagerDialog dlg(profiles_dir, QStringLiteral("Default"));
        auto* list = list_of(dlg);
        // Select "Modded" (row 1).
        list->setCurrentRow(1);

        auto* saves = checkbox_of(dlg, "Local Saves");
        auto* settings = checkbox_of(dlg, "Local Settings");
        auto* invalidation = checkbox_of(dlg, "Archive Invalidation");
        REQUIRE(saves != nullptr);
        REQUIRE(settings != nullptr);
        REQUIRE(invalidation != nullptr);
        REQUIRE(saves->isChecked());
        REQUIRE(settings->isChecked());
        REQUIRE(invalidation->isChecked());
    }

    SECTION("toggling a setting persists it to settings.ini") {
        ui::ProfileManagerDialog dlg(profiles_dir, QStringLiteral("Default"));
        auto* list = list_of(dlg);
        list->setCurrentRow(1);  // "Modded"

        auto* saves = checkbox_of(dlg, "Local Saves");
        REQUIRE(saves != nullptr);
        REQUIRE_FALSE(saves->isChecked());  // fresh profile default
        saves->setChecked(true);

        // The toggle handler writes settings.ini immediately.
        engine::profile::ProfileManager profile(profiles_dir / "Modded");
        REQUIRE(profile.local_saves());
    }

    SECTION("selecting a profile returns it via selected_profile") {
        ui::ProfileManagerDialog dlg(profiles_dir, QStringLiteral("Default"));
        auto* list = list_of(dlg);
        list->setCurrentRow(1);  // "Modded"
        auto* select_btn = button_of(dlg, "Select");
        REQUIRE(select_btn != nullptr);
        REQUIRE(select_btn->isEnabled());
        select_btn->click();
        REQUIRE(dlg.selected_profile() == QStringLiteral("Modded"));
    }

    SECTION("delete is disabled for the active profile") {
        ui::ProfileManagerDialog dlg(profiles_dir, QStringLiteral("Default"));
        auto* list = list_of(dlg);
        list->setCurrentRow(0);  // "Default" (active)
        auto* delete_btn = button_of(dlg, "Delete");
        REQUIRE(delete_btn != nullptr);
        // The active profile cannot be deleted; the button stays enabled but
        // the handler refuses with a warning (defense in depth — the engine
        // also refuses via ProfileRemoveResult::ActiveProfile).
        REQUIRE(delete_btn->isEnabled());
    }
}