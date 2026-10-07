#include "ui/widgets/category_filter_panel.h"

#include "engine/plugin_host/category_factory.h"
#include "ui/settings/settings.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QShowEvent>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace ui {

CategoryFilterPanel::CategoryFilterPanel(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 2, 4, 2);
  layout->setSpacing(4);

  setWhatsThis(tr("Narrow the mod list to the categories ticked here, to the "
                  "special filters ticked above them, or to both at once. The "
                  "category list is global - \"Edit...\" opens the editor for it."));

  // Special filters (MO2's CategoryFactory::SpecialCategories rows that the
  // mod list has data for). Their own group, above the category tree, so a
  // tick never has to be distinguished from a category id.
  auto *special_label = new QLabel(tr("Special filters:"), this);
  special_label->setEnabled(false);
  layout->addWidget(special_label);

  struct Entry {
    Special which;
    const char *label;
    const char *tip;
  };
  static constexpr Entry kSpecials[] = {
      {Special::Active, QT_TR_NOOP("Active"), QT_TR_NOOP("Only mods that are enabled")},
      {Special::Conflict, QT_TR_NOOP("Conflicted"),
       QT_TR_NOOP("Only mods that win or lose a file conflict")},
      {Special::HiddenFiles, QT_TR_NOOP("Hidden files"),
       QT_TR_NOOP("Only mods that ship files hidden from the game")},
  };
  for (const auto &entry : kSpecials) {
    auto *box = new QCheckBox(tr(entry.label), this);
    box->setToolTip(tr(entry.tip));
    connect(box, &QCheckBox::toggled, this, [this](bool) {
      emit filters_changed();
    });
    special_boxes_.emplace_back(entry.which, box);
    layout->addWidget(box);
  }

  tree_ = new QTreeWidget(this);
  tree_->setHeaderHidden(true);
  tree_->setMinimumWidth(160);
  layout->addWidget(tree_, 1);

  // AND/OR mode (MO2's filtersAnd / filtersOr radios). Applies to every
  // filter on the mod list - text, group, categories and the special
  // filters above - so it lives here, next to the filters it joins.
  auto *and_radio = new QRadioButton(tr("Match all filters"), this);
  and_radio->setToolTip(tr("Keep a mod only when every active filter matches it"));
  and_radio->setChecked(true);
  auto *or_radio = new QRadioButton(tr("Match any filter"), this);
  or_radio->setToolTip(tr("Keep a mod when at least one active filter matches it"));
  mode_group_ = new QButtonGroup(this);
  mode_group_->addButton(and_radio);
  mode_group_->addButton(or_radio);
  connect(mode_group_, &QButtonGroup::buttonToggled, this,
          [this](QAbstractButton *, bool) {
            emit filters_changed();
          });
  layout->addWidget(and_radio);
  layout->addWidget(or_radio);

  auto *buttons = new QHBoxLayout();
  buttons->setSpacing(4);
  auto *clear_btn = new QPushButton(tr("Clear"), this);
  clear_btn->setToolTip(tr("Clear the category filter"));
  buttons->addWidget(clear_btn);
  buttons->addStretch(1);
  auto *edit_btn = new QPushButton(tr("Edit..."), this);
  edit_btn->setToolTip(tr("Edit the category list"));
  buttons->addWidget(edit_btn);
  layout->addLayout(buttons);

  connect(tree_, &QTreeWidget::itemChanged, this,
          &CategoryFilterPanel::on_item_changed);
  connect(clear_btn, &QPushButton::clicked, this, &CategoryFilterPanel::clear_filter);
  connect(edit_btn, &QPushButton::clicked, this,
          &CategoryFilterPanel::edit_categories_clicked);

  rebuild();
  restore_state();
}

QSet<CategoryFilterPanel::Special> CategoryFilterPanel::checked_specials() const {
  QSet<Special> out;
  for (const auto &[which, box] : special_boxes_) {
    if (box->isChecked())
      out.insert(which);
  }
  return out;
}

engine::filter::Mode CategoryFilterPanel::filter_mode() const {
  // Button 0 is the And radio added first in the ctor, button 1 the Or one;
  // an unchecked group (only possible before the ctor finishes) reads as And.
  return mode_group_ && mode_group_->checkedId() == 1 ? engine::filter::Mode::Or
                                                      : engine::filter::Mode::And;
}

void CategoryFilterPanel::set_filter_mode(engine::filter::Mode mode) {
  if (!mode_group_)
    return;
  auto *want = mode_group_->button(mode == engine::filter::Mode::Or ? 1 : 0);
  if (want && !want->isChecked()) {
    want->setChecked(true);
    emit filters_changed();
  }
}

void CategoryFilterPanel::rebuild() {
  // The checked set survives a rebuild: the tree is repopulated from scratch,
  // so the ticks are re-applied by id afterwards. Without this, reopening the
  // category editor (which rebuilds) silently dropped a remembered filter.
  const auto keep = checked_category_ids();
  rebuilding_     = true;
  tree_->clear();
  add_children(tree_->invisibleRootItem(), 0);
  tree_->expandAll();
  rebuilding_ = false;
  set_checked_category_ids(keep);
}

void CategoryFilterPanel::add_children(QTreeWidgetItem *root, int parent_id) {
  const auto &cats = engine::Category::Factory::instance().categories();

  // Children of `parent_id`, sorted by name (case-insensitive) so the panel
  // reads like MO2's alphabetized category list rather than raw id order.
  std::vector<const engine::Category::Factory::Entry *> children;
  for (const auto &[id, cat] : cats) {
    if (cat.parent_id == parent_id)
      children.push_back(&cat);
  }
  std::sort(children.begin(), children.end(), [](const auto *a, const auto *b) {
    return QString::fromStdString(a->name).compare(QString::fromStdString(b->name),
                                                   Qt::CaseInsensitive) < 0;
  });

  for (const auto *cat : children) {
    auto *item = new QTreeWidgetItem(root);
    item->setText(0, QString::fromStdString(cat->name));
    item->setData(0, Qt::UserRole, cat->id);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, Qt::Unchecked);
    add_children(item, cat->id);
  }
}

QSet<int> CategoryFilterPanel::checked_category_ids() const {
  QSet<int> out;
  collect_checked(tree_->invisibleRootItem(), out);
  return out;
}

void CategoryFilterPanel::set_checked_category_ids(const QSet<int> &ids) {
  rebuilding_ = true;
  apply_checked(tree_->invisibleRootItem(), ids);
  rebuilding_ = false;
}

void CategoryFilterPanel::apply_checked(QTreeWidgetItem *node, const QSet<int> &ids) {
  for (int i = 0; i < node->childCount(); ++i) {
    QTreeWidgetItem *child = node->child(i);
    child->setCheckState(0, ids.contains(child->data(0, Qt::UserRole).toInt())
                                ? Qt::Checked
                                : Qt::Unchecked);
    apply_checked(child, ids);
  }
}

void CategoryFilterPanel::collect_checked(QTreeWidgetItem *node, QSet<int> &out) const {
  for (int i = 0; i < node->childCount(); ++i) {
    QTreeWidgetItem *child = node->child(i);
    if (child->checkState(0) == Qt::Checked)
      out.insert(child->data(0, Qt::UserRole).toInt());
    collect_checked(child, out);
  }
}

void CategoryFilterPanel::clear_filter() {
  rebuilding_ = true;
  set_all_unchecked(tree_->invisibleRootItem());
  rebuilding_ = false;
  save_state();
  emit category_filter_changed();
}

// Settings > Mod List > "Remember filter settings". Written on every change
// while the setting is on, so the ticked set reaches the disk before the app
// can be closed - which is also what makes it survive a crash.
void CategoryFilterPanel::save_state() {
  if (!Settings::instance().save_filters())
    return;
  QStringList ids;
  for (int id : checked_category_ids())
    ids.append(QString::number(id));
  ids.sort();
  Settings::instance().set_modlist_filter_categories(ids);
}

void CategoryFilterPanel::restore_state() {
  if (!Settings::instance().save_filters())
    return;
  QSet<int> ids;
  for (const auto &raw : Settings::instance().modlist_filter_categories()) {
    bool ok      = false;
    const int id = raw.trimmed().toInt(&ok);
    if (ok)
      ids.insert(id);
  }
  // An id with no item here (the category was deleted, or its plugin has not
  // registered it yet) is simply not ticked - a stale remembered filter can
  // never narrow the mod list to nothing.
  set_checked_category_ids(ids);
}

void CategoryFilterPanel::set_all_unchecked(QTreeWidgetItem *node) {
  for (int i = 0; i < node->childCount(); ++i) {
    QTreeWidgetItem *child = node->child(i);
    child->setCheckState(0, Qt::Unchecked);
    set_all_unchecked(child);
  }
}

void CategoryFilterPanel::on_item_changed(QTreeWidgetItem *item, int column) {
  Q_UNUSED(item)
  Q_UNUSED(column)
  if (rebuilding_)
    return;
  save_state();
  emit category_filter_changed();
}

void CategoryFilterPanel::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  // Plugins register categories at load time (before the UI is built), but
  // rebuild on the first show anyway so late registrations appear. The
  // checked state survives hide/show cycles (rebuild only when empty).
  if (tree_->topLevelItemCount() == 0) {
    rebuild();
    restore_state();
  }
}

}  // namespace ui