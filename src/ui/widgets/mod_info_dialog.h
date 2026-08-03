#pragma once

#include <QDialog>
#include <QString>

class QFormLayout;

namespace ui {

// Minimal read-only Mod Info dialog (MO2's Mod Info Window, trimmed down).
// Shows identity, state, source metadata and conflict stats for one mod.
class ModInfoDialog : public QDialog {
    Q_OBJECT
public:
    struct Data {
        QString name;
        QString folder;
        QString version;
        QString source;      // rendered source line, e.g. "Nexus Mods (id: 123)"
        bool enabled = true;
        int priority = 0;
        int conflict_wins = 0;
        int conflict_losses = 0;
        int file_count = 0;
    };

    explicit ModInfoDialog(const Data& data, QWidget* parent = nullptr);

private:
    void add_row(QFormLayout* form, const QString& label, const QString& value);
};

}  // namespace ui
