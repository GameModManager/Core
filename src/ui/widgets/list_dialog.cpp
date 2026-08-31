#include "ui/widgets/list_dialog.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/settings/settings.h"

namespace ui {

ListDialog::ListDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Select an item..."));
  setMinimumSize(260, 380);

  auto *layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  list_ = new QListWidget(this);
  list_->setMinimumSize(240, 280);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  layout->addWidget(list_);

  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(tr("Filter"));
  filter_->setClearButtonEnabled(true);
  layout->addWidget(filter_);

  buttons_ = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons_, &QDialogButtonBox::accepted, this, &ListDialog::accept);
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons_);

  connect(list_, &QListWidget::itemDoubleClicked, this, &ListDialog::accept);
  connect(filter_, &QLineEdit::textChanged, this,
          &ListDialog::on_filter_textChanged);

  filter_->setFocus();
}

int ListDialog::exec() {
  const auto geo = Settings::instance().listdialog_window_geometry();
  if (!geo.isEmpty())
    restoreGeometry(geo);
  const int rc = QDialog::exec();
  Settings::instance().set_listdialog_window_geometry(saveGeometry());
  return rc;
}

void ListDialog::setChoices(const QStringList &choices) {
  choices_ = choices;
  data_.clear();
  apply_filter();
}

void ListDialog::setChoiceData(const QList<QVariant> &data) {
  data_ = data;
  apply_filter();
}

void ListDialog::setCurrentRow(int row) {
  if (row >= 0 && row < list_->count()) {
    list_->setCurrentRow(row);
    list_->scrollToItem(list_->currentItem());
  }
}

QString ListDialog::getChoice() const {
  auto *item = list_->currentItem();
  if (!item)
    return {};
  return item->text();
}

QVariant ListDialog::getChoiceData() const {
  auto *item = list_->currentItem();
  if (!item)
    return {};
  return item->data(Qt::UserRole);
}

void ListDialog::on_filter_textChanged(const QString &filter) {
  apply_filter();
}

void ListDialog::apply_filter() {
  const QString filter = filter_->text().trimmed();
  const int count = choices_.size();

  QStringList newChoices;
  QList<QVariant> newData;
  newChoices.reserve(count);
  for (int i = 0; i < count; ++i) {
    if (filter.isEmpty() ||
        choices_.at(i).contains(filter, Qt::CaseInsensitive)) {
      newChoices.append(choices_.at(i));
      if (i < data_.size())
        newData.append(data_[i]);
    }
  }

  list_->clear();

  for (int i = 0; i < newChoices.size(); ++i) {
    auto *item = new QListWidgetItem(newChoices[i], list_);
    if (i < newData.size())
      item->setData(Qt::UserRole, newData[i]);
  }

  if (newChoices.size() == 1) {
    list_->setCurrentRow(0);
  }

  if (!filter.isEmpty()) {
    list_->setStyleSheet("QListWidget { border: 2px ridge #f00; }");
  } else {
    list_->setStyleSheet(QString());
  }

  auto *box = buttons_;
  if (box) {
    auto *ok = box->button(QDialogButtonBox::Ok);
    if (ok)
      ok->setEnabled(list_->count() > 0);
  }
}

} // namespace ui
