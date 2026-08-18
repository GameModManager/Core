#pragma once

#include <QDialog>
#include <QSet>

#include <filesystem>

class QPushButton;
class QTableWidget;

namespace ui {

// MO2 CategoriesDialog port (categoriesdialog.{cpp,ui}): a modal editor for
// the global category registry (engine::CategoryFactory). Shows every
// category in an editable table (ID | Name | ParentID), lets the user add
// rows (next free ID, name "new", root parent), remove the selected row, and
// edit any cell. The ID column rejects 0 and duplicates; the ParentID column
// accepts 0 (root) or an existing category id (never the row's own id).
//
// The table is a working copy: nothing touches the factory until OK. On OK
// the whole table is validated, committed to the factory as a diff
// (add/update/remove), and persisted to <instance_root>/categories.dat when
// an instance root is given. Cancel discards every edit.
class CategoriesDialog : public QDialog {
    Q_OBJECT
public:
    explicit CategoriesDialog(const std::filesystem::path& instance_root = {},
                              QWidget* parent = nullptr);

    // Validates the table and applies it to engine::CategoryFactory::instance()
    // (add/update/remove diff), then saves categories.dat when an instance
    // root was given. Returns false (and selects the first bad cell) when the
    // table is invalid; the factory is left untouched in that case. Public so
    // tests can drive the commit without clicking OK.
    bool commit_changes();

private slots:
    void on_add_clicked();
    void on_remove_clicked();

private:
    void fill_table();
    // Next free positive id (max id in the table + 1).
    int next_free_id() const;
    // Validates every row: positive unique ids, parents that are 0 or an
    // existing id (never the row's own id). Selects the first bad cell and
    // returns a user-facing error message (empty when the table is valid).
    // Never shows a dialog itself, so tests can call it headless.
    QString validate_table() const;

    QTableWidget* table_ = nullptr;
    QPushButton* remove_btn_ = nullptr;
    std::filesystem::path instance_root_;
};

}  // namespace ui