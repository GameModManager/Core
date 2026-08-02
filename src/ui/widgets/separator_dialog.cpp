#include "ui/widgets/separator_dialog.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace ui {

SeparatorDialog::SeparatorDialog(const QString& title,
                                 const QString& initial_name,
                                 const QColor& initial_color,
                                 QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    setModal(true);

    auto* main = new QVBoxLayout(this);

    auto* name_row = new QHBoxLayout;
    name_row->addWidget(new QLabel(tr("Insert name:"), this));
    name_edit_ = new QLineEdit(this);
    name_edit_->setText(initial_name);
    name_edit_->setMinimumWidth(200);
    name_row->addWidget(name_edit_, 1);
    main->addLayout(name_row);

    color_picker_ = new QColorDialog(initial_color, this);
    color_picker_->setOptions(QColorDialog::NoButtons);
    main->addWidget(color_picker_);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this]() {
        if (!name_edit_->text().trimmed().isEmpty())
            accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    main->addWidget(buttons_);

    name_edit_->setFocus();
}

QString SeparatorDialog::name() const {
    return name_edit_->text().trimmed();
}

QColor SeparatorDialog::color() const {
    return color_picker_->currentColor();
}

}  // namespace ui
