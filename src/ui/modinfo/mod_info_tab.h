#pragma once

#include "ui/modinfo/mod_info_data.h"

#include <QWidget>

class QDialog;

namespace ui {

// Base class for every Mod Info tab. Lifecycle mirrors MO2's ModInfoDialogTab:
// set_mod() resets the tab for a new mod, first_activation() runs once the
// first time the tab is shown, save_state()/restore_state() persist pending
// edits (called on mod switch / dialog close), can_close() vetoes switching.
class ModInfoTab : public QWidget {
    Q_OBJECT
public:
    explicit ModInfoTab(QWidget* parent = nullptr);
    ~ModInfoTab() override;

    virtual void set_mod(const ModInfoData& data) = 0;
    virtual void first_activation() {}
    virtual void save_state() {}
    virtual void restore_state() {}
    virtual bool can_close() { return true; }

    ModInfoTabId tab_id() const { return tab_id_; }
    void set_tab_id(ModInfoTabId id) { tab_id_ = id; }

    // Swaps in the working data for the currently displayed mod. The dialog
    // calls this before set_mod(); tabs read it through current(). Public so a
    // host (or a test driving a tab standalone) can set the data too.
    void set_current(const ModInfoData& data) { current_ = data; }

protected:
    const ModInfoData& current() const { return current_; }

    // Marks the tab as needing data (name shows in normal weight). Tabs that
    // are empty for a mod are grayed out by the dialog.
    void set_has_data(bool has);

private:
    ModInfoData current_;
    ModInfoTabId tab_id_ = ModInfoTabId::TextFiles;
    bool has_data_ = false;
};

}  // namespace ui
