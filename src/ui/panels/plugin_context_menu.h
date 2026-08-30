#pragma once

#include <QObject>
#include <QString>

#include <string>
#include <vector>

class QMenu;

namespace engine::PluginDb {

// Per-row metadata needed by the context menu (extracted from PluginsTab).
struct RowInfo {
    std::string name;
    bool locked = false;
    bool force_loaded = false;
};

// Context menu for the plugin table (MO2 PluginListContextMenu parity).
// Receives row metadata and emits lock/unlock requests.
class ContextMenu : public QObject {
    Q_OBJECT
public:
    explicit ContextMenu(QObject* parent = nullptr);

    // Update the per-row metadata (called when the plugin list is rebuilt).
    void set_rows(const std::vector<RowInfo>& rows);

    // Fill `menu` with actions for the given row index.
    // Split out so tests can drive it without exec()-ing a modal menu.
    void add_actions(QMenu& menu, int row);

signals:
    void lock_requested(const std::string& name, bool locked);

private:
    std::vector<RowInfo> rows_;
};

}  // namespace engine::PluginDb
