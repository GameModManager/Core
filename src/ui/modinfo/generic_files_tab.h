#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

#include <vector>

class QLineEdit;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QSplitter;

namespace KSyntaxHighlighting {
class Repository;
class SyntaxHighlighter;
}

namespace ui {

// MO2's GenericFilesTab: a filterable list of files (matched by a subclass
// predicate) on the left and an inline plain-text editor on the right. Dirty
// edits are flushed on mod switch / dialog close (canClose prompts). Used by
// both the Text Files and Config Files tabs. The editor gets syntax
// highlighting via KSyntaxHighlighting, resolved per file from its file name.
class GenericFilesTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit GenericFilesTab(QWidget* parent = nullptr);
    ~GenericFilesTab() override;

    void set_mod(const ModInfoData& data) override;
    void save_state() override;
    bool can_close() override;

protected:
    // Return true to include `full_path` (path relative to the mod's data dir
    // is also given for cheap extension checks).
    virtual bool wants_file(const QString& rel_path,
                            const QString& full_path) const = 0;

    bool event(QEvent* event) override;

private:
    struct File {
        QString full_path;
        QString text;
    };

    void rebuild_list();
    void apply_filter();
    void select_file(const QModelIndex& index);
    void load_editor(const QString& path);
    void save_editor();
    bool maybe_flush_editor();
    void apply_theme();

    QSplitter* splitter_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QPushButton* save_btn_ = nullptr;
    KSyntaxHighlighting::Repository* repository_ = nullptr;
    KSyntaxHighlighting::SyntaxHighlighter* highlighter_ = nullptr;
    std::vector<File> files_;
    QString editor_path_;
    QString last_loaded_text_;
    bool editor_dirty_ = false;
};

}  // namespace ui
