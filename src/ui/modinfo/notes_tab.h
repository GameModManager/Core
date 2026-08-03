#pragma once

#include "ui/modinfo/mod_info_tab.h"

class QLineEdit;
class QPushButton;
class QTextEdit;

namespace ui {

// MO2's Notes tab: a comments line edit (background tinted by the mod's
// separator color) plus an HTML notes editor. Set/Reset color buttons apply
// only to separators (the only mod type GMM colors). All fields persist to the
// mod's meta.ini ([General] comments / notes / color, MO2-compatible).
class NotesTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit NotesTab(QWidget* parent = nullptr);
    ~NotesTab() override;

    void set_mod(const ModInfoData& data) override;
    void save_state() override;

private:
    void update_comments_color();
    void on_comments_edited();
    void on_set_color();
    void on_reset_color();
    void persist_notes();

    QLineEdit* comments_ = nullptr;
    QTextEdit* notes_ = nullptr;
    QPushButton* set_color_ = nullptr;
    QPushButton* reset_color_ = nullptr;
    bool notes_dirty_ = false;
};

}  // namespace ui
