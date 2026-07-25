#include "ui/widgets/mod_filter_bar.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>

namespace ui {

ModFilterBar::ModFilterBar(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Expand/Collapse all button [>>]
    expand_btn_ = new QToolButton(this);
    expand_btn_->setText(">>");
    expand_btn_->setToolTip("Expand / Collapse all groups");
    expand_btn_->setFixedWidth(30);
    layout->addWidget(expand_btn_);

    connect(expand_btn_, &QToolButton::clicked, this, &ModFilterBar::expand_all_clicked);

    // Filter text input
    filter_edit_ = new QLineEdit(this);
    filter_edit_->setPlaceholderText("Filter...");
    filter_edit_->setClearButtonEnabled(true);
    layout->addWidget(filter_edit_, 1);

    connect(filter_edit_, &QLineEdit::textChanged,
            this, &ModFilterBar::filter_changed);

    // Groups dropdown
    group_combo_ = new QComboBox(this);
    group_combo_->addItem("All");
    group_combo_->addItem("Enabled");
    group_combo_->addItem("Disabled");
    group_combo_->addItem("Conflicts");
    group_combo_->addItem("Separators");
    group_combo_->setMinimumWidth(100);
    layout->addWidget(group_combo_);

    connect(group_combo_, &QComboBox::currentTextChanged,
            this, &ModFilterBar::group_changed);
}

QString ModFilterBar::filter_text() const {
    return filter_edit_->text();
}

QString ModFilterBar::current_group() const {
    return group_combo_->currentText();
}

}  // namespace ui
