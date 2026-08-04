#pragma once

#include <QDialog>
#include <QString>

#include <optional>
#include <string>

class QComboBox;

namespace ui {

// MO2 "Quick Install" name dialog (SimpleInstallDialog port) for non-FOMOD
// installs. MO2 shows a preset name; GMM is "smarter": an editable combobox
// whose dropdown carries the best guesses - the Nexus display name, a cleaned
// archive-stem derivation, and the full archive filename - so the user can pick
// or type the folder name. Cancel aborts the whole install.
class InstallNameDialog : public QDialog {
    Q_OBJECT
public:
    InstallNameDialog(const QString& suggested_name,
                      const QString& archive_filename,
                      QWidget* parent = nullptr);

    // The confirmed mod name (as shown/edited in the combo). Empty when the
    // user left the field blank.
    QString name() const;

    // Suggested-name candidates for the dropdown (index 0 = preferred default).
    static QStringList candidates(const QString& suggested_name,
                                  const QString& archive_filename);

private:
    QComboBox* name_combo_ = nullptr;
};

// Blocking helper that shows the dialog and returns the confirmed name, or
// nullopt when the user canceled. Safe to call from any thread: off the main
// thread it marshals the modal dialog onto the main thread and waits (same
// pattern as ask_overwrite).
std::optional<std::string> ask_install_name(const std::string& suggested_name,
                                            const std::string& archive_filename,
                                            QWidget* parent = nullptr);

}  // namespace ui
