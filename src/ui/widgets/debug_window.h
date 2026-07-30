#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine {
class PluginLoader;
}

namespace ui {

class DebugWindow : public QDialog {
    Q_OBJECT
public:
    explicit DebugWindow(const std::filesystem::path& instance_root,
                         const std::string& game_id,
                         const std::string& game_name,
                         engine::PluginLoader* plugin_loader,
                         std::function<void()> on_reload_ui = nullptr,
                         QWidget* parent = nullptr);
    ~DebugWindow();

private:
    void refresh_stats();
    static std::string read_proc(const char* path);
    static std::string read_proc_line(const char* path, const char* prefix);

    QLabel* cpu_label_ = nullptr;
    QLabel* ram_label_ = nullptr;
    QLabel* disk_label_ = nullptr;
    QLabel* uptime_label_ = nullptr;
    QLabel* interval_label_ = nullptr;
    QLabel* instance_root_label_ = nullptr;
    QLabel* game_label_ = nullptr;
    QLabel* plugins_label_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    int refresh_interval_ = 2;

    QPushButton* reload_ui_btn_ = nullptr;

    std::filesystem::path instance_root_;
    std::string game_id_;
    std::string game_name_;
    engine::PluginLoader* plugin_loader_ = nullptr;
};

} // namespace ui
