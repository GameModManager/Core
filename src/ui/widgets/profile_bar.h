#pragma once

#include <QWidget>

class QComboBox;
class QToolButton;
class QHBoxLayout;

namespace ui {

// Important instance paths, mirroring MO2's openFolderMenu() (MO2
// mainwindow.cpp openFolderMenu). "Install" stays disabled until GMM is
// actually installed somewhere — the app currently runs from the build dir.
enum class FolderKind {
    Game,       // the game install folder
    MyGames,    // prefix Documents/My Games/<game>
    Inis,       // profile folder when local INIs are on, else the game's MyGames folder
    Instance,
    Mods,
    Profile,
    Downloads,
    Install,    // GMM install folder (disabled: app isn't installed anywhere)
    Plugins,    // GMM global plugins folder
    Themes,     // GMM global themes folder
    Logs,       // GMM data dir (gamemodmanager.log lives here)
};

class ProfileBar : public QWidget {
    Q_OBJECT
public:
    explicit ProfileBar(QWidget* parent = nullptr);

    // Repopulate the profile dropdown. `profiles` are the existing profile
    // names (from engine::profile::list_profiles); `current` is the active
    // profile and is selected when present (else the first entry). The
    // '<Manage...>' sentinel is always appended last. Programmatic changes
    // never emit profile_changed.
    void set_profiles(const QStringList& profiles, const QString& current);

    // The currently selected profile name, or empty when the '<Manage...>'
    // sentinel is selected.
    [[nodiscard]] QString current_profile() const;

signals:
    void profile_changed(const QString& profile);
    // Emitted when the user picks the '<Manage...>' entry; the controller
    // opens the profile manager dialog.
    void manage_profiles_requested();
    void open_folder_requested(ui::FolderKind kind);
    void export_clicked();
    void export_modlist_clicked();
    void import_modlist_clicked();
    void create_separator_clicked();
    void create_empty_mod_clicked();

private:
    QComboBox* profile_combo_ = nullptr;
    QToolButton* folders_btn_ = nullptr;
    QToolButton* export_btn_ = nullptr;
    QToolButton* import_export_btn_ = nullptr;
    QToolButton* create_btn_ = nullptr;
    // Last real profile selected (never the '<Manage...>' sentinel); used to
    // restore the selection after the sentinel is picked.
    QString last_profile_ = QStringLiteral("Default");
};

}  // namespace ui
