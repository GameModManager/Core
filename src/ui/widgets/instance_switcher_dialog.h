#pragma once

#include <QDialog>
#include <QString>
#include <filesystem>
#include <string>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QLabel;

namespace engine {
class PluginLoader;
}

namespace ui {

struct InstanceSwitcherEntry {
    std::string name;          // folder name (e.g. "The_Binding_of_Isaac_Rebirth")
    std::string game_id;       // from instance.toml
    std::string display_name;  // resolved from the plugin (fallback: game_id)
    std::filesystem::path root; // absolute path to instance root
    bool portable = true;        // from instance.toml (default: portable)
    QLabel* icon_label = nullptr;  // owned by the list widget; refreshed when
                                   // the async icon fetch lands
};

class InstanceSwitcherDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstanceSwitcherDialog(engine::PluginLoader* plugins, QWidget* parent = nullptr);

    // Populate from the global instances directory
    void load_instances(const std::string& instances_dir);

    // Returns the selected instance name (empty if cancelled or no selection)
    [[nodiscard]] QString selected_instance() const { return selected_; }

    // Returns true if the user clicked "Create new instance"
    [[nodiscard]] bool create_requested() const { return create_requested_; }

signals:
    void create_new_instance();

private:
    void on_ok();
    void on_create();
    void refresh_list();
    void update_icons_for(const QString& game_id);

    engine::PluginLoader* plugins_ = nullptr;
    std::string instances_dir_;
    QListWidget* list_ = nullptr;
    QString selected_;
    bool create_requested_ = false;
    std::vector<InstanceSwitcherEntry> entries_;
};

}  // namespace ui
