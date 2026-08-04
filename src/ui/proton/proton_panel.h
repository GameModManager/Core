#pragma once

#include <QDialog>

#include <filesystem>
#include <string>

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace engine {
class PlatformInterface;
class PluginLoader;
}

namespace ui {

// Modal "Proton options" panel: per-instance Proton runner selector plus the
// game's recommended wine packages (wine.json shipped with the game plugin).
// The runner is persisted to instance.toml by the caller (MainWindow) after
// exec(), following the SettingsDialog pattern.
class ProtonPanel : public QDialog {
    Q_OBJECT
public:
    ProtonPanel(engine::PlatformInterface* platform,
                engine::PluginLoader* plugin_loader,
                const std::string& game_id,
                const std::string& game_display_name,
                const std::filesystem::path& game_dir,
                uint32_t steam_appid,
                const std::filesystem::path& instance_root,
                const std::string& current_runner,
                QWidget* parent = nullptr);

    // Runner selected in the panel (display name or absolute path).
    // Empty = automatic (Steam per-game override, then latest).
    [[nodiscard]] std::string selected_runner() const;

private:
    void refresh_runners();
    void update_runner_detail();
    void load_recommended_packages();
    void install_packages(const QStringList& verbs);
    [[nodiscard]] std::filesystem::path recommended_packages_path() const;

    engine::PlatformInterface* platform_ = nullptr;
    engine::PluginLoader* plugin_loader_ = nullptr;
    std::string game_id_;
    std::string game_display_name_;
    std::filesystem::path game_dir_;
    uint32_t steam_appid_ = 0;
    std::filesystem::path instance_root_;

    QComboBox* runner_combo_ = nullptr;
    QLabel* runner_detail_ = nullptr;
    QPushButton* install_all_btn_ = nullptr;
    QVBoxLayout* packages_layout_ = nullptr;
    QLabel* packages_status_ = nullptr;
};

}  // namespace ui
