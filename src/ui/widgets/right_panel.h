#pragma once

#include <QWidget>
#include <string>
#include <unordered_map>

class QTabWidget;
class QTableWidget;

namespace engine { class GameCapabilities; }

namespace ui {

class ConflictsTab;
class ExecControlsBar;
class RightFilterBar;
class DataTab;
class DownloadsTab;
class PluginsTab;

class RightPanel : public QWidget {
    Q_OBJECT
public:
    explicit RightPanel(QWidget* parent = nullptr);

    void set_capabilities(const engine::GameCapabilities* caps) { capabilities_ = caps; }

    // Rebuild the tab bar based on what the current game supports.
    // "Data" is always shown. Other tabs only appear if the game registers that capability.
    void set_game(const std::string& game_id);

    [[nodiscard]] ExecControlsBar* exec_controls() const { return exec_controls_; }
    [[nodiscard]] QTabWidget* tab_widget() const { return tab_widget_; }
    [[nodiscard]] RightFilterBar* filter_bar() const { return filter_bar_; }
    [[nodiscard]] DownloadsTab* downloads_tab() const;
    [[nodiscard]] ConflictsTab* conflicts_tab() const;
    [[nodiscard]] DataTab* data_tab() const;
    [[nodiscard]] PluginsTab* plugins_tab() const;

    // Switch the tab bar to the Downloads tab (no-op when the game has no
    // downloads capability). Used when a download arrives so the user sees
    // the new entry appear.
    void show_downloads_tab();

private:
    void clear_tabs();
    void ensure_tab(const std::string& capability, const QString& label);
    void apply_filter();
    QTableWidget* current_table() const;

    ExecControlsBar* exec_controls_ = nullptr;
    QTabWidget* tab_widget_ = nullptr;
    RightFilterBar* filter_bar_ = nullptr;
    DataTab* data_tab_ = nullptr;

    // Lazily created - owned by tab_widget_ once added
    std::unordered_map<std::string, QWidget*> tabs_;

    const engine::GameCapabilities* capabilities_ = nullptr;
    std::string current_game_id_;
};

}  // namespace ui
