#include "ui/widgets/right_filter_bar.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTableWidget>

namespace ui {

RightFilterBar::RightFilterBar(QWidget *parent) : QWidget(parent) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(4, 2, 4, 2);
  layout->setSpacing(4);

  // MO2's right-hand lists use the same plain "Filter" placeholder as the
  // mod list (references/modorganizer/src/mainwindow.ui:997).
  filter_edit_ = new QLineEdit(this);
  filter_edit_->setPlaceholderText(tr("Filter"));
  filter_edit_->setClearButtonEnabled(true);
  layout->addWidget(filter_edit_, 1);

  sort_button_ = new QPushButton(tr("Sort"), this);
  sort_button_->setToolTip(
      tr("This will invoke loot to sort the plugins using it's masterlists"));
  sort_button_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  layout->addWidget(sort_button_);
  sort_button_->hide();

  connect(filter_edit_, &QLineEdit::textChanged, this, &RightFilterBar::filter_changed);
  connect(sort_button_, &QPushButton::clicked, this, &RightFilterBar::sort_requested);
}

void RightFilterBar::set_sort_visible(bool visible) {
  sort_button_->setVisible(visible);
}

QString RightFilterBar::filter_text() const {
  return filter_edit_->text();
}

void RightFilterBar::focus_filter() {
  filter_edit_->setFocus();
  filter_edit_->selectAll();
}

bool RightFilterBar::clear_filter() {
  if (filter_edit_->text().isEmpty())
    return false;
  filter_edit_->clear();
  return true;
}

bool RightFilterBar::filter_has_focus() const {
  return filter_edit_->hasFocus();
}

void RightFilterBar::apply_to(QTableWidget *table) const {
  if (!table)
    return;

  const QString text = filter_edit_->text().trimmed().toLower();

  for (int row = 0; row < table->rowCount(); ++row) {
    if (text.isEmpty()) {
      table->setRowHidden(row, false);
      continue;
    }

    bool match = false;
    for (int col = 0; col < table->columnCount(); ++col) {
      auto *item = table->item(row, col);
      if (item && item->text().toLower().contains(text)) {
        match = true;
        break;
      }
    }
    table->setRowHidden(row, !match);
  }
}

}  // namespace ui
