#pragma once

#include "ui/modinfo/mod_info_data.h"

#include <QDialog>

#include <memory>
#include <vector>

class QLabel;
class QPushButton;
class QTabWidget;

namespace ui {

class ModInfoTab;

// MO2's Mod Info Window: a tabbed dialog showing everything about one mod.
//  - prev/next buttons cycle the whole mod list (each switch persists edits)
//  - Delete key removes the current mod (regular mods only)
//  - tabs are movable; the last active tab + window geometry are remembered
// Tabs live in src/ui/modinfo/ and are created here in ModInfoTabId order.
class ModInfoDialog : public QDialog {
    Q_OBJECT
public:
    explicit ModInfoDialog(std::vector<ModInfoData> mods, int index,
                           ModInfoTabId initial_tab, QWidget* parent = nullptr);
    ~ModInfoDialog() override;

    // Swaps in freshly rebuilt data for the current mod (e.g. after a conflict
    // recompute) and jumps to the Conflicts tab. Keeps the same mod selected.
    void reload_current(ModInfoData data);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

private:
    void load_index(int index);
    void switch_to(int index);
    // Nearest non-separator mod index in direction `dir` (+1 next, -1 prev)
    // from `from`, or -1 when there is none. Separators are never worth
    // viewing, so prev/next cycle past them.
    int next_mod_index(int from, int dir) const;
    bool can_switch() const;
    void persist_geometry();
    void restore_geometry();
    void on_delete_mod();

    std::vector<ModInfoData> mods_;
    int index_ = -1;
    QTabWidget* tabs_ = nullptr;
    QLabel* mod_name_ = nullptr;
    QPushButton* prev_btn_ = nullptr;
    QPushButton* next_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;
    std::vector<ModInfoTab*> tab_order_;
    std::vector<bool> tab_activated_;
};

}  // namespace ui
