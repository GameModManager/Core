#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;

namespace ui {

// Name + copy-source input for creating a profile (MO2's ProfileInputDialog).
// The copy-source combo lists "(fresh)" plus every existing profile; a fresh
// profile is created when copy_source() is empty.
class ProfileCreateDialog : public QDialog {
    Q_OBJECT
public:
    // `existing_profiles` are the current profile names (used both to offer
    // copy sources and to reject duplicate names). `copy_source` preselects
    // the copy source (used by the Copy button); empty = fresh.
    explicit ProfileCreateDialog(const QStringList& existing_profiles,
                                 const QString& copy_source = {},
                                 QWidget* parent = nullptr);

    [[nodiscard]] QString profile_name() const;
    // Empty = create a fresh profile; otherwise the source profile to copy.
    [[nodiscard]] QString copy_source() const;

private:
    void update_ok_button();

    QLineEdit* name_edit_ = nullptr;
    QComboBox* copy_combo_ = nullptr;
};

}  // namespace ui