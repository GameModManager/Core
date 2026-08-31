#include "ui/settings/categories_dialog.h"

#include "engine/pipeline/plugin_host/category_factory.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QValidator>

#include <climits>
#include <vector>

namespace ui {

namespace {

// Shared integer-cell validator for the ID / ParentID columns. Reads the
// current table contents live (not a snapshot), so edits made in one cell are
// immediately visible to the other validators. `current_row_` is set by the
// delegate when an editor opens, so a row never validates against itself.
class IntCellValidator : public QValidator {
public:
  explicit IntCellValidator(int min, int max, QTableWidget *table,
                            QObject *parent = nullptr)
      : QValidator(parent), int_validator_(min, max, this), table_(table) {}

  void set_current_row(int row) { current_row_ = row; }

protected:
  State validate_int(QString &input, int &pos) const {
    return int_validator_.validate(input, pos);
  }
  // All ids currently in column 0, excluding the row being edited.
  QSet<int> other_ids() const {
    QSet<int> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
      if (r == current_row_)
        continue;
      bool ok = false;
      const int id =
          table_->item(r, 0) ? table_->item(r, 0)->text().toInt(&ok) : 0;
      if (ok)
        out.insert(id);
    }
    return out;
  }

  QTableWidget *table_ = nullptr;
  int current_row_ = -1;
  QIntValidator int_validator_;
};

// ID column: a positive integer that no other row uses.
class NewIdValidator : public IntCellValidator {
public:
  explicit NewIdValidator(QTableWidget *table, QObject *parent = nullptr)
      : IntCellValidator(1, INT_MAX, table, parent) {}

  State validate(QString &input, int &pos) const override {
    State base = validate_int(input, pos);
    if (base != Acceptable)
      return base;
    bool ok = false;
    const int id = input.toInt(&ok);
    if (!ok || id <= 0 || other_ids().contains(id))
      return Intermediate;
    return Acceptable;
  }
};

// ParentID column: 0 (root) or an id present in another row.
class ParentIdValidator : public IntCellValidator {
public:
  explicit ParentIdValidator(QTableWidget *table, QObject *parent = nullptr)
      : IntCellValidator(0, INT_MAX, table, parent) {}

  State validate(QString &input, int &pos) const override {
    State base = validate_int(input, pos);
    if (base != Acceptable)
      return base;
    bool ok = false;
    const int id = input.toInt(&ok);
    if (!ok || id < 0)
      return Intermediate;
    if (id == 0 || other_ids().contains(id))
      return Acceptable;
    return Intermediate;
  }
};

// Editor delegate: a QLineEdit carrying the column validator. Invalid input is
// never committed (the cell keeps its previous value), mirroring MO2's
// ValidatingDelegate.
class ValidatingDelegate : public QStyledItemDelegate {
public:
  explicit ValidatingDelegate(IntCellValidator *validator,
                              QObject *parent = nullptr)
      : QStyledItemDelegate(parent), validator_(validator) {}

  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                        const QModelIndex &index) const override {
    auto *edit = new QLineEdit(parent);
    validator_->set_current_row(index.row());
    edit->setValidator(validator_);
    return edit;
  }

  void setModelData(QWidget *editor, QAbstractItemModel *model,
                    const QModelIndex &index) const override {
    auto *edit = qobject_cast<QLineEdit *>(editor);
    if (!edit)
      return;
    int pos = 0;
    QString text = edit->text();
    if (validator_->validate(text, pos) == QValidator::Acceptable) {
      // Store the ID / ParentID as ints so sorting works numerically.
      if (index.column() == 0 || index.column() == 2)
        model->setData(index, text.toInt());
      else
        QStyledItemDelegate::setModelData(editor, model, index);
    }
  }

private:
  IntCellValidator *validator_ = nullptr;
};

} // namespace

CategoriesDialog::CategoriesDialog(const std::filesystem::path &instance_root,
                                   QWidget *parent)
    : QDialog(parent), instance_root_(instance_root) {
  setWindowTitle(tr("Categories"));
  setMinimumSize(480, 360);

  auto *layout = new QVBoxLayout(this);

  table_ = new QTableWidget(this);
  table_->setColumnCount(3);
  table_->setHorizontalHeaderLabels({tr("ID"), tr("Name"), tr("ParentID")});
  table_->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(
      2, QHeaderView::ResizeToContents);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setSortingEnabled(true);
  layout->addWidget(table_, 1);

  // Column validators (MO2 parity): unique positive IDs, parents that are 0
  // or an existing id.
  auto *new_id_validator = new NewIdValidator(table_, this);
  auto *parent_validator = new ParentIdValidator(table_, this);
  table_->setItemDelegateForColumn(
      0, new ValidatingDelegate(new_id_validator, this));
  table_->setItemDelegateForColumn(
      2, new ValidatingDelegate(parent_validator, this));

  auto *buttons = new QHBoxLayout();
  buttons->setSpacing(4);
  auto *add_btn = new QPushButton(tr("Add"), this);
  add_btn->setToolTip(tr("Add a new category"));
  remove_btn_ = new QPushButton(tr("Remove"), this);
  remove_btn_->setToolTip(tr("Remove the selected category"));
  remove_btn_->setEnabled(false);
  buttons->addWidget(add_btn);
  buttons->addWidget(remove_btn_);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  auto *dialog_buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(dialog_buttons, &QDialogButtonBox::accepted, this, [this]() {
    const QString error = validate_table();
    if (!error.isEmpty()) {
      QMessageBox::warning(this, tr("Categories"), error);
      return;
    }
    commit_changes();
    accept();
  });
  connect(dialog_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(dialog_buttons);

  connect(add_btn, &QPushButton::clicked, this,
          &CategoriesDialog::on_add_clicked);
  connect(remove_btn_, &QPushButton::clicked, this,
          &CategoriesDialog::on_remove_clicked);
  connect(table_, &QTableWidget::itemSelectionChanged, this,
          [this]() { remove_btn_->setEnabled(table_->currentRow() >= 0); });

  fill_table();
}

bool CategoriesDialog::commit_changes() {
  if (!validate_table().isEmpty())
    return false;

  auto &factory = engine::Category::Factory::instance();

  // Snapshot the table (row order is irrelevant; the factory is keyed by id).
  struct Row {
    int id = 0;
    QString name;
    int parent_id = 0;
  };
  QVector<Row> rows;
  QSet<int> table_ids;
  rows.reserve(table_->rowCount());
  for (int r = 0; r < table_->rowCount(); ++r) {
    Row row;
    row.id = table_->item(r, 0)->text().toInt();
    row.name = table_->item(r, 1) ? table_->item(r, 1)->text() : QString();
    row.parent_id = table_->item(r, 2)->text().toInt();
    rows.push_back(row);
    table_ids.insert(row.id);
  }

  // Add / update the rows present in the table.
  for (const auto &row : rows) {
    if (factory.categoryExists(row.id))
      factory.updateCategory(row.id, row.name.toStdString(), row.parent_id);
    else
      factory.addCategory(row.id, row.name.toStdString(), row.parent_id);
  }

  // Remove factory categories that are no longer in the table. validate_table
  // guarantees no remaining row references a removed id, so the factory's
  // re-parent-to-root is a no-op for the survivors. Ids are collected first —
  // removeCategory erases from the map, so iterating while removing would
  // invalidate the iterator.
  std::vector<int> removed_ids;
  for (const auto &[id, cat] : factory.categories()) {
    Q_UNUSED(cat)
    if (id != 0 && !table_ids.contains(id))
      removed_ids.push_back(id);
  }
  for (int id : removed_ids)
    factory.removeCategory(id);

  // Persist when an instance root is known (MO2 saves categories.dat on
  // commit). A missing file on the next load leaves the registry unchanged.
  if (!instance_root_.empty())
    factory.save(instance_root_ / "categories.dat");

  return true;
}

void CategoriesDialog::fill_table() {
  const auto &cats = engine::Category::Factory::instance().categories();

  table_->setRowCount(0);
  table_->setSortingEnabled(false);
  int row = 0;
  for (const auto &[id, cat] : cats) {
    if (id == 0)
      continue; // "None" is implicit
    table_->insertRow(row);
    auto *id_item = new QTableWidgetItem();
    id_item->setData(Qt::DisplayRole, id);
    auto *name_item = new QTableWidgetItem(QString::fromStdString(cat.name));
    auto *parent_item = new QTableWidgetItem();
    parent_item->setData(Qt::DisplayRole, cat.parent_id);
    table_->setItem(row, 0, id_item);
    table_->setItem(row, 1, name_item);
    table_->setItem(row, 2, parent_item);
    ++row;
  }
  table_->setSortingEnabled(true);
}

int CategoriesDialog::next_free_id() const {
  int highest = 0;
  for (int r = 0; r < table_->rowCount(); ++r) {
    bool ok = false;
    const int id =
        table_->item(r, 0) ? table_->item(r, 0)->text().toInt(&ok) : 0;
    if (ok && id > highest)
      highest = id;
  }
  return highest + 1;
}

void CategoriesDialog::on_add_clicked() {
  const int row = table_->rowCount();
  table_->setSortingEnabled(false);
  table_->insertRow(row);
  auto *id_item = new QTableWidgetItem();
  id_item->setData(Qt::DisplayRole, next_free_id());
  auto *name_item = new QTableWidgetItem(tr("new"));
  auto *parent_item = new QTableWidgetItem();
  parent_item->setData(Qt::DisplayRole, 0);
  table_->setItem(row, 0, id_item);
  table_->setItem(row, 1, name_item);
  table_->setItem(row, 2, parent_item);
  table_->setSortingEnabled(true);

  // Start editing the name of the new row (item-based so it survives the
  // re-sort that insertRow triggers).
  table_->setCurrentItem(name_item);
  table_->editItem(name_item);
}

void CategoriesDialog::on_remove_clicked() {
  const int row = table_->currentRow();
  if (row < 0)
    return;
  table_->removeRow(row);
  remove_btn_->setEnabled(table_->currentRow() >= 0);
}

QString CategoriesDialog::validate_table() const {
  // IDs: positive and unique.
  QSet<int> ids;
  for (int r = 0; r < table_->rowCount(); ++r) {
    bool ok = false;
    const int id =
        table_->item(r, 0) ? table_->item(r, 0)->text().toInt(&ok) : 0;
    if (!ok || id <= 0) {
      table_->setCurrentCell(r, 0);
      return tr("Row %1 has an invalid ID. IDs must be positive integers.")
          .arg(r + 1);
    }
    if (ids.contains(id)) {
      table_->setCurrentCell(r, 0);
      return tr("Row %1 uses ID %2, which is already used by another row.")
          .arg(r + 1)
          .arg(id);
    }
    ids.insert(id);
  }

  // Parents: 0 (root) or an existing id, never the row's own id.
  for (int r = 0; r < table_->rowCount(); ++r) {
    bool ok = false;
    const int parent =
        table_->item(r, 2) ? table_->item(r, 2)->text().toInt(&ok) : -1;
    const int id = table_->item(r, 0)->text().toInt();
    if (!ok || parent < 0 || (parent != 0 && !ids.contains(parent)) ||
        parent == id) {
      table_->setCurrentCell(r, 2);
      return tr("Row %1 has an invalid ParentID. Use 0 for a root "
                "category or the ID of an existing category.")
          .arg(r + 1);
    }
  }
  return QString();
}

} // namespace ui