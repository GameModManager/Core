#include "ui/settings/categories_dialog.h"

#include "engine/plugin_host/category_factory.h"
#include "ui/settings/category_import_dialog.h"
#include "engine/source/nexus/categories.h"

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
        bool ok      = false;
        const int id = table_->item(r, 0) ? table_->item(r, 0)->text().toInt(&ok) : 0;
        if (ok)
          out.insert(id);
      }
      return out;
    }

    QTableWidget *table_ = nullptr;
    int current_row_     = -1;
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
      bool ok      = false;
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
      bool ok      = false;
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
    explicit ValidatingDelegate(IntCellValidator *validator, QObject *parent = nullptr)
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
      int pos      = 0;
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

}  // namespace

CategoriesDialog::CategoriesDialog(const std::filesystem::path &instance_root,
                                   QWidget *parent)
    : QDialog(parent), instance_root_(instance_root) {
  setWindowTitle(tr("Categories"));
  setMinimumSize(480, 360);

  auto *layout = new QVBoxLayout(this);

  table_ = new QTableWidget(this);
  table_->setColumnCount(3);
  table_->setHorizontalHeaderLabels({tr("ID"), tr("Name"), tr("ParentID")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setSortingEnabled(true);
  layout->addWidget(table_, 1);

  // Column validators (MO2 parity): unique positive IDs, parents that are 0
  // or an existing id.
  auto *new_id_validator = new NewIdValidator(table_, this);
  auto *parent_validator = new ParentIdValidator(table_, this);
  table_->setItemDelegateForColumn(0, new ValidatingDelegate(new_id_validator, this));
  table_->setItemDelegateForColumn(2, new ValidatingDelegate(parent_validator, this));

  auto *buttons = new QHBoxLayout();
  buttons->setSpacing(4);
  auto *add_btn = new QPushButton(tr("Add"), this);
  add_btn->setToolTip(tr("Add a new category"));
  remove_btn_ = new QPushButton(tr("Remove"), this);
  remove_btn_->setToolTip(tr("Remove the selected category"));
  remove_btn_->setEnabled(false);
  buttons->addWidget(add_btn);
  buttons->addWidget(remove_btn_);
  // MO2 parity: CategoriesDialog has a nexus refresh + import pair, and both
  // are only usable once Nexus category mappings are turned on
  // (references/modorganizer/src/categoriesdialog.cpp:120-127). Here the
  // import is the whole feature, so the button is always present and gates
  // itself on the game's Nexus domain instead.
  import_btn_ = new QPushButton(tr("Import from Nexus"), this);
  import_btn_->setToolTip(tr("Add this game's Nexus categories to the list below"));
  import_btn_->setEnabled(false);  // no domain yet
  buttons->addWidget(import_btn_);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  auto *dialog_buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
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

  connect(add_btn, &QPushButton::clicked, this, &CategoriesDialog::on_add_clicked);
  connect(remove_btn_, &QPushButton::clicked, this,
          &CategoriesDialog::on_remove_clicked);
  connect(import_btn_, &QPushButton::clicked, this,
          &CategoriesDialog::on_import_nexus_clicked);
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
    remove_btn_->setEnabled(table_->currentRow() >= 0);
  });

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
    row.id        = table_->item(r, 0)->text().toInt();
    row.name      = table_->item(r, 1) ? table_->item(r, 1)->text() : QString();
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
  // re-parent-to-root is a no-op for the survivors. Ids are collected first -
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
      continue;  // "None" is implicit
    table_->insertRow(row);
    auto *id_item = new QTableWidgetItem();
    id_item->setData(Qt::DisplayRole, id);
    auto *name_item   = new QTableWidgetItem(QString::fromStdString(cat.name));
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
    bool ok      = false;
    const int id = table_->item(r, 0) ? table_->item(r, 0)->text().toInt(&ok) : 0;
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
  auto *name_item   = new QTableWidgetItem(tr("new"));
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

void CategoriesDialog::set_nexus_domain(std::string domain) {
  nexus_domain_ = std::move(domain);
  if (import_btn_)
    import_btn_->setEnabled(!nexus_domain_.empty());
}

void CategoriesDialog::on_import_nexus_clicked() {
  if (nexus_domain_.empty())
    return;

  const auto list = engine::Source::Nexus::fetch_categories(nexus_domain_);
  if (!list.ok) {
    // One message covers every reason: no API key configured, an HTTP error,
    // or a body we could not read. The engine logs the reason.
    QMessageBox::warning(
        this, tr("Import from Nexus"),
        tr("Nexus returned no categories for this game. Check that a Nexus API "
           "key is set in Settings -> Sources."));
    return;
  }

  // Commit whatever the user has already typed before touching the factory:
  // fill_table() below rebuilds the table from the factory, so any uncommitted
  // edit would otherwise be silently discarded by the press of this button.
  const QString pending = validate_table();
  if (!pending.isEmpty()) {
    QMessageBox::warning(this, tr("Categories"), pending);
    return;
  }
  commit_changes();

  // MO2 asks before it imports (CategoryImportDialog,
  // references/modorganizer/src/categoryimportdialog.cpp:37-55): the strategy,
  // whether the rows this import adds carry their Nexus mapping, and whether
  // rows already in the list should be remapped onto it. A single button that
  // silently picked Merge could only ever express one of the four answers.
  CategoryImportDialog ask(this);
  if (ask.exec() != QDialog::Accepted || ask.strategy() == CategoryImportDialog::None)
    return;

  std::vector<engine::Category::Factory::Entry> entries;
  entries.reserve(list.categories.size());
  for (const auto &cat : list.categories) {
    engine::Category::Factory::Entry entry;
    entry.id        = cat.id;
    entry.name      = cat.name;
    entry.parent_id = cat.parent_id;
    entries.push_back(std::move(entry));
  }
  const int added = engine::Category::Factory::instance().import_external(
      entries,
      ask.strategy() == CategoryImportDialog::Overwrite
          ? engine::Category::Factory::ImportMode::Replace
          : engine::Category::Factory::ImportMode::Merge,
      ask.assign(), ask.remap());

  // The mapping now has to outlive this session, or resolve_nexus_id() answers
  // 0 on the next run and the installed mod falls back to no category at all.
  // MO2 writes nexuscatmap.dat from the same save as categories.dat
  // (references/modorganizer/src/categories.cpp:213-226).
  if (!instance_root_.empty())
    engine::Category::Factory::instance().save(instance_root_ / "categories.dat");

  fill_table();
  QMessageBox::information(
      this, tr("Import from Nexus"),
      added ? tr("Added %n category/categories from Nexus.", nullptr, added)
            : tr("Every Nexus category for this game is already in the list."));
}

QString CategoriesDialog::validate_table() const {
  // IDs: positive and unique.
  QSet<int> ids;
  for (int r = 0; r < table_->rowCount(); ++r) {
    bool ok      = false;
    const int id = table_->item(r, 0) ? table_->item(r, 0)->text().toInt(&ok) : 0;
    if (!ok || id <= 0) {
      table_->setCurrentCell(r, 0);
      return tr("Row %1 has an invalid ID. IDs must be positive integers.").arg(r + 1);
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
    bool ok          = false;
    const int parent = table_->item(r, 2) ? table_->item(r, 2)->text().toInt(&ok) : -1;
    const int id     = table_->item(r, 0)->text().toInt();
    if (!ok || parent < 0 || (parent != 0 && !ids.contains(parent)) || parent == id) {
      table_->setCurrentCell(r, 2);
      return tr("Row %1 has an invalid ParentID. Use 0 for a root "
                "category or the ID of an existing category.")
          .arg(r + 1);
    }
  }
  return QString();
}

}  // namespace ui