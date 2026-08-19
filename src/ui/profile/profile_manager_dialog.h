#pragma once

#include <QDialog>

#include <filesystem>

class QListWidget;
class QPushButton;

namespace ui {
class ProfileSettingsWidget;

// MO2's ProfilesDialog: list all profiles, create/copy/rename/delete, mark a
// default (startup) profile, and edit per-profile settings (Local Saves,
// Local Settings, Archive Invalidation). The engine stays Qt-free — this
// dialog drives engine::profile directly.
//
// The dialog never switches the active profile itself: the caller reads
// selected_profile() after exec() (or connects to the signals) and performs
// the switch. profiles_changed() fires after every list mutation so the
// caller can refresh the main-window selector.
class ProfileManagerDialog : public QDialog {
    Q_OBJECT
public:
    // `profiles_dir` is the instance's profiles directory; `active_profile`
    // is the currently selected profile (cannot be deleted/renamed, shown
    // with an "(active)" marker); `default_profile` is the persisted startup
    // profile (shown with a "(default)" marker).
    ProfileManagerDialog(const std::filesystem::path& profiles_dir,
                         const QString& active_profile,
                         const QString& default_profile = {},
                         QWidget* parent = nullptr);

    // Profile the user chose to switch to (Select button / double-click), or
    // empty when the dialog was closed without a selection.
    [[nodiscard]] QString selected_profile() const { return selected_; }

signals:
    // Emitted after the profile list changed (create/copy/rename/delete) so
    // the caller can refresh the main-window selector.
    void profiles_changed();
    // Emitted when the user marks a profile as the default (startup) profile.
    void default_profile_changed(const QString& name);

private:
    void refresh_list();
    void on_create();
    void on_copy();
    void on_rename();
    void on_delete_profile();
    void on_set_default();
    void on_select();
    void on_selection_changed();
    void load_settings_for(const QString& name);
    [[nodiscard]] QString current_item_name() const;

    std::filesystem::path profiles_dir_;
    QString active_profile_;
    QString default_profile_;
    QString selected_;

    QListWidget* list_ = nullptr;
    ProfileSettingsWidget* settings_widget_ = nullptr;
    QPushButton* copy_btn_ = nullptr;
    QPushButton* rename_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;
    QPushButton* default_btn_ = nullptr;
    QPushButton* select_btn_ = nullptr;
};

}  // namespace ui