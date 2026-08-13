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
class SavesTab;

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
    [[nodiscard]] SavesTab* saves_tab() const;
    [[nodiscard]] ConflictsTab* conflicts_tab() const;
    [[nodiscard]] DataTab* data_tab() const;
    [[nodiscard]] PluginsTab* plugins_tab() const;

    // Switch the tab bar to the Downloads tab (no-op when the game has no
    // downloads capability). Used when a download arrives so the user sees
    // the new entry appear.
    void show_downloads_tab();

    // Re-apply the current filter text to the current tab's table. Used after
    // a Plugins-tab refresh, whose set_plugins() rebuild rebuilds rows and
    // clears the row-hidden states the filter had set.
    void reapply_current_filter() { apply_filter(); }

    // Restore the last selected tab for the current instance. `capability` is
    // the persisted tab key ("data", "plugins", "downloads", ...). No-op when
    // the capability is empty or the game doesn't support it - the tab bar
    // stays on the first tab (the default).
    void restore_tab(const std::string& capability);

signals:
    // LOOT sort shortcut pressed in the Plugins tab filter bar.
    void sort_requested();

    // The user switched the right-panel tab. Emits the tab's capability key
    // ("data", "plugins", "downloads", ...) so the caller can persist it per
    // instance. Not emitted during programmatic rebuilds (set_game,
    // restore_tab, show_downloads_tab).
    void tab_changed(const QString& capability);

private:
    void clear_tabs();
    void ensure_tab(const std::string& capability, const QString& label);
    void apply_filter();
    void update_sort_visibility();
    QTableWidget* current_table() const;
    // Capability key of the currently visible tab ("" when none).
    std::string current_tab_capability() const;

    ExecControlsBar* exec_controls_ = nullptr;
    QTabWidget* tab_widget_ = nullptr;
    RightFilterBar* filter_bar_ = nullptr;
    DataTab* data_tab_ = nullptr;

    // Lazily created - owned by tab_widget_ once added
    std::unordered_map<std::string, QWidget*> tabs_;

    const engine::GameCapabilities* capabilities_ = nullptr;
    std::string current_game_id_;
    // True while the tab bar is being rebuilt/restored programmatically
    // (set_game, restore_tab, show_downloads_tab); suppresses tab_changed so
    // only genuine user selections are persisted.
    bool suppress_tab_save_ = false;
};

}  // namespace ui
