#pragma once

#include "engine/game/plugins/plugin_info.h"
#include "ui/panels/plugin_context_menu.h"

#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QLCDNumber;
class QMenu;
class QPushButton;
class QShowEvent;
class QTableWidget;

namespace ui {

class PluginsTab : public QWidget {
    Q_OBJECT
public:
    explicit PluginsTab(QWidget* parent = nullptr);
    // Out-of-line: table_ is a private PluginTable* whose base needs the
    // complete type for the upcast.
    [[nodiscard]] QTableWidget* table() const;

    // Replace the plugin list contents. Row 0 = most dominant (first-loaded).
    // Force-loaded rows (game-native, CC) are pinned and shown greyed.
    void set_plugins(const std::vector<engine::GamePlugin>& plugins);

    // Re-sync enabled checkboxes from engine state without rebuilding rows
    // (used to revert a blocked toggle, incl. transitively flipped masters).
    void sync_enabled(const std::vector<engine::GamePlugin>& plugins);

    // MO2-style plugin counter (PluginListView::updatePluginCount parity,
    // modorganizer/src/pluginlistview.cpp:67). The number shows how many
    // enabled plugins pass the tab's text filter (row-hidden rows are
    // excluded); the tooltip breaks the count down by type with active/total
    // columns. Recomputed by set_plugins()/sync_enabled(), on show(), and by
    // RightPanel whenever the filter text changes.
    void refresh_counters();

    // MO2 parity. Two independent highlight flags rendered by apply_highlights
    // (contained wins over master), re-applied by set_plugins() which rebuilds
    // the rows:
    //  - set_contained_plugins: plugins owned by the mod selected in the mod
    //    list -> plugin_list_contained.
    //  - set_master_plugins: masters of the plugin selected here ->
    //    plugin_list_master.
    void set_contained_plugins(const QVector<QString>& contained);
    void set_master_plugins(const QVector<QString>& masters);

    // Names of the plugins currently selected in the table (row order).
    [[nodiscard]] QStringList selected_plugin_names() const;

    // User role on the Flags column holding the row's emblems as individual
    // QIcons (QList<QIcon>). A FlagsDelegate paints them one-by-one at native
    // size (wrapping, growing the row) - the stacked-pixmap single-icon
    // approach scales every emblem down to one icon slot.
    static constexpr int kPluginFlagsRole = Qt::UserRole + 60;
    // Parallel role on the Flags column: per-emblem hover text (QStringList,
    // same order as the kPluginFlagsRole icon list). FlagsDelegate::helpEvent
    // shows ONLY the entry of the emblem under the cursor.
    static constexpr int kPluginFlagTooltipsRole = Qt::UserRole + 61;

signals:
    void toggle_requested(const std::string& name, bool enabled);
    void reorder_requested(int from_row, int to_row);
    // User-pinned (immovable) load-order lock, from the row context menu.
    void lock_requested(const std::string& name, bool locked);
    // Refresh button pressed: re-scan plugins on disk and repopulate.
    void refresh_requested();

protected:
    // Fills `menu` with the actions for the row at `row` (MO2's
    // PluginListContextMenu lock actions). Split out of on_custom_context_menu
    // so tests can drive it without exec()-ing a modal menu (DataTab pattern).
    void add_context_menu_actions(QMenu& menu, int row);
    // Recompute the counter when the tab becomes visible again (the text
    // filter may have been changed while another tab was current).
    void showEvent(QShowEvent* event) override;

private:
    void apply_highlights();
    void on_custom_context_menu(const QPoint& pos);
    // Recompute every row's height from the emblem wrap math (FlagsDelegate
    // paints one QIcon per emblem; a QTableWidget does not auto-size rows from
    // a delegate's sizeHint). Runs after set_plugins and on Flags-column
    // resizes so wrapping rows grow as the column narrows.
    void relayout_flag_rows();

    // MO2 plugin classification for the counter (updatePluginCount order):
    // medium > light (ext-or-flag) > master (ext-or-flag) > regular.
    enum class PluginType { Regular, Master, Light, Medium };

    class PluginTable;
    PluginTable* table_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QLCDNumber* counter_display_ = nullptr;
    std::vector<std::string> names_;
    // Per-row engine state backing the context menu (locked / core rows).
    std::vector<bool> rows_locked_;
    std::vector<bool> rows_force_loaded_;
    // Per-row MO2 plugin type, index-aligned with names_ (drives the counter).
    std::vector<PluginType> rows_type_;
    QSet<QString> contained_names_;
    QSet<QString> master_names_;
    bool syncing_ = false;
    // Extracted context menu (lock/unlock actions).
    std::unique_ptr<engine::PluginDb::ContextMenu> context_menu_;
};

}  // namespace ui
