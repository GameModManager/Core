#pragma once

#include <QWidget>

#include <filesystem>

class QCheckBox;

namespace ui {

// Per-profile settings editor (MO2's ProfilesDialog checkboxes): Local Saves,
// Local Settings and Archive Invalidation. Values are read from and written
// to the profile's settings.ini through engine::profile::ProfileManager.
//
// set_profile() loads the values for a profile directory; toggling a checkbox
// emits settings_changed() so the owner can persist the change (the widget
// itself does not write - the owner knows which profile is selected).
class ProfileSettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProfileSettingsWidget(QWidget* parent = nullptr);

    // Load the settings for `profile_dir`. An empty path clears and disables
    // the checkboxes. Never emits settings_changed().
    void set_profile(const std::filesystem::path& profile_dir);

    // Current checkbox values.
    [[nodiscard]] bool local_saves() const;
    [[nodiscard]] bool local_settings() const;
    [[nodiscard]] bool archive_invalidation() const;

signals:
    // Emitted when the user toggles a checkbox (not on set_profile()).
    void settings_changed();

private:
    QCheckBox* local_saves_box_ = nullptr;
    QCheckBox* local_settings_box_ = nullptr;
    QCheckBox* invalidation_box_ = nullptr;
};

}  // namespace ui