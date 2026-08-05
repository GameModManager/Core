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

signals:
    void profile_changed(const QString& profile);
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
};

}  // namespace ui
