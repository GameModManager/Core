#pragma once

#include <QDialog>
#include <QString>
#include <string>

namespace engine {
class PluginLoader;
}

namespace ui {

class InstanceSwitcherContentWidget;

// Instance switcher dialog. Thin QDialog wrapper around the mode-agnostic
// InstanceSwitcherContentWidget: the instance list + create button mapped to
// accept/reject semantics. In Full UI tab mode the same widget is embedded
// directly in MainTabContainer, so this dialog is only used for popup mode.
class InstanceSwitcherDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstanceSwitcherDialog(engine::PluginLoader* plugins,
                                    QWidget* parent = nullptr);

    // Populate from the global instances directory
    void load_instances(const std::string& instances_dir);

    // Returns the selected instance name (empty if cancelled or no selection)
    [[nodiscard]] QString selected_instance() const;

    // Returns true if the user clicked "Create new instance"
    [[nodiscard]] bool create_requested() const { return create_requested_; }

signals:
    void create_new_instance();

private:
    void on_ok();

    InstanceSwitcherContentWidget* content_ = nullptr;
    bool create_requested_ = false;
};

}  // namespace ui
