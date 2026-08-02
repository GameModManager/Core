#pragma once

#include <QDialog>
#include <QString>

namespace engine {
enum class OverwriteAction;
struct OverwriteDecision;
}

class QCheckBox;

namespace ui {

// MO2 "Mod Exists" dialog (QueryOverwriteDialog port). Asked when an install
// targets a mod folder that already exists. Mirrors
// REFERENCES/modorganizer/src/queryoverwritedialog.{cpp,ui} 1:1: Merge /
// Replace / Rename (default) / Cancel buttons, a "Keep Backup" checkbox
// (default checked), and a question-mark icon with the explanation text.
class QueryOverwriteDialog : public QDialog {
    Q_OBJECT
public:
    QueryOverwriteDialog(const QString& mod_name, bool default_backup,
                         QWidget* parent = nullptr);

    engine::OverwriteAction action() const;
    bool backup() const;

private:
    void set_action(engine::OverwriteAction action);

    engine::OverwriteAction action_;
    QCheckBox* backup_box_ = nullptr;
};

// Blocking helper that shows the dialog and returns the full decision. Safe to
// call from any thread: when invoked off the main thread (the install pipeline
// runs on a worker QThread) it marshals the modal dialog onto the main thread
// and waits, using the same pattern as QtKeychainKeyring's run_on_main.
engine::OverwriteDecision ask_overwrite(const QString& mod_name,
                                        bool default_backup,
                                        QWidget* parent = nullptr);

}  // namespace ui
