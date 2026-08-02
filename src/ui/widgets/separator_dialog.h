#pragma once

#include <QColor>
#include <QDialog>
#include <QString>

class QColorDialog;
class QDialogButtonBox;
class QLineEdit;

namespace ui {

// Single dialog for creating/editing a separator: a name field and an inline
// color picker together (no separate color step after naming).
class SeparatorDialog : public QDialog {
    Q_OBJECT
public:
    explicit SeparatorDialog(const QString& title,
                             const QString& initial_name = {},
                             const QColor& initial_color = QColor("#888888"),
                             QWidget* parent = nullptr);

    [[nodiscard]] QString name() const;
    [[nodiscard]] QColor color() const;

private:
    QLineEdit* name_edit_ = nullptr;
    QColorDialog* color_picker_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

}  // namespace ui
