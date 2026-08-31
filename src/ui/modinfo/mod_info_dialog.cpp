#include "ui/modinfo/mod_info_dialog.h"

#include "ui/modinfo/mod_info_tab.h"
#include "ui/settings/settings.h"

// Tabs (order matters - must match ModInfoTabId).
#include "ui/modinfo/categories_tab.h"
#include "ui/modinfo/config_files_tab.h"
#include "ui/modinfo/conflicts_tab.h"
#include "ui/modinfo/esps_tab.h"
#include "ui/modinfo/filetree_tab.h"
#include "ui/modinfo/images_tab.h"
#include "ui/modinfo/notes_tab.h"
#include "ui/modinfo/source_tab.h"
#include "ui/modinfo/text_files_tab.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

ModInfoDialog::ModInfoDialog(ModInfoData data,
                             std::vector<std::pair<QString, bool>> nav_list,
                             ModInfoTabId initial_tab, QWidget *parent)
    : QDialog(parent), current_mod_data_(std::move(data)),
      nav_list_(std::move(nav_list)), nav_index_(-1) {
  setWindowTitle(tr("Mod Information"));
  resize(735, 534);

  // Find nav_index_ for the current mod.
  for (int i = 0; i < static_cast<int>(nav_list_.size()); ++i) {
    if (nav_list_[static_cast<size_t>(i)].first == current_mod_data_.id) {
      nav_index_ = i;
      break;
    }
  }

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);

  tabs_ = new QTabWidget(this);
  tabs_->setDocumentMode(true);
  tabs_->setTabsClosable(false);
  tabs_->setMovable(true);
  layout->addWidget(tabs_, 1);

  const auto make_tab = [&](ModInfoTab *tab, const QString &title) {
    tab->set_tab_id(static_cast<ModInfoTabId>(tab_order_.size()));
    tab_order_.push_back(tab);
    tabs_->addTab(tab, title);
  };

  make_tab(new TextFilesTab(this), tr("Text Files"));
  make_tab(new ConfigFilesTab(this), tr("Config Files"));
  make_tab(new ImagesTab(this), tr("Images"));
  make_tab(new EspsTab(this), tr("ESP Files"));
  make_tab(new ConflictsInfoTab(this), tr("Conflicts"));
  make_tab(new CategoriesTab(this), tr("Categories"));
  make_tab(new SourceTab(this), tr("Source"));
  make_tab(new NotesTab(this), tr("Notes"));
  make_tab(new FiletreeTab(this), tr("Filetree"));

  tab_loaded_.assign(tab_order_.size(), false);
  tab_activated_.assign(tab_order_.size(), false);

  connect(tabs_, &QTabWidget::currentChanged, this, [this](int /*visual*/) {
    QWidget *cur = tabs_->currentWidget();
    auto it = std::find(tab_order_.begin(), tab_order_.end(), cur);
    if (it == tab_order_.end())
      return;
    const size_t idx = static_cast<size_t>(it - tab_order_.begin());
    auto *tab = *it;
    if (!tab_loaded_[idx]) {
      tab_loaded_[idx] = true;
      tab->set_current(current_mod_data_);
      tab->set_mod(current_mod_data_);
      tab->restore_state();
    }
    if (!tab_activated_[idx]) {
      tab_activated_[idx] = true;
      tab->first_activation();
    }
  });

  // --- bottom bar: prev / mod name / next | delete | close ---
  auto *bar = new QWidget(this);
  auto *bar_layout = new QHBoxLayout(bar);
  bar_layout->setContentsMargins(0, 4, 0, 0);

  prev_btn_ = new QPushButton(QChar(0x226A), bar); // "≪"
  prev_btn_->setToolTip(tr("Previous mod"));
  next_btn_ = new QPushButton(QChar(0x226B), bar); // "≫"
  next_btn_->setToolTip(tr("Next mod"));
  bar_layout->addWidget(prev_btn_);
  bar_layout->addWidget(next_btn_);

  mod_name_ = new QLabel(bar);
  QFont name_font = mod_name_->font();
  name_font.setBold(true);
  mod_name_->setFont(name_font);
  mod_name_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  bar_layout->addWidget(mod_name_, 1);

  delete_btn_ = new QPushButton(tr("Delete"), bar);
  delete_btn_->setObjectName("deleteModBtn");
  bar_layout->addWidget(delete_btn_);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, bar);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  bar_layout->addWidget(buttons);

  layout->addWidget(bar);

  connect(prev_btn_, &QPushButton::clicked, this, [this]() {
    const int target = next_nav_index(nav_index_, -1);
    if (target >= 0)
      switch_to(target);
  });
  connect(next_btn_, &QPushButton::clicked, this, [this]() {
    const int target = next_nav_index(nav_index_, +1);
    if (target >= 0)
      switch_to(target);
  });
  connect(delete_btn_, &QPushButton::clicked, this,
          &ModInfoDialog::on_delete_mod);

  restore_geometry();

  // Load the initial mod's data BEFORE activating any tab: setCurrentIndex
  // below fires first_activation(), which reads tab data (and switch_to()
  // early-returns when nav_index_ == nav_index_, so it can't be relied on
  // here).
  load_index(nav_index_);

  // Place on the requested tab, or the last-used one.
  int tab_index = static_cast<int>(initial_tab);
  if (tab_index < 0 || tab_index >= tabs_->count())
    tab_index = Settings::instance().modinfo_last_tab();
  tab_index = qBound(0, tab_index, tabs_->count() - 1);
  tabs_->setCurrentIndex(tab_index);

  update_tab_enabled_states();
}

ModInfoDialog::~ModInfoDialog() = default;

void ModInfoDialog::load_index(int index) {
  nav_index_ = index;

  mod_name_->setText(current_mod_data_.name);
  prev_btn_->setEnabled(next_nav_index(nav_index_, -1) >= 0);
  next_btn_->setEnabled(next_nav_index(nav_index_, +1) >= 0);

  const bool deletable =
      !current_mod_data_.is_separator && !current_mod_data_.is_overwrite &&
      !current_mod_data_.is_merged && !current_mod_data_.is_game_native &&
      static_cast<bool>(current_mod_data_.delete_mod);
  delete_btn_->setEnabled(deletable);
}

void ModInfoDialog::update_tab_enabled_states() {
  // Tabs that should be disabled when they have no content.
  static constexpr ModInfoTabId kDisableable[] = {
      ModInfoTabId::TextFiles, ModInfoTabId::ConfigFiles, ModInfoTabId::Images,
      ModInfoTabId::Esps,      ModInfoTabId::Conflicts,
  };
  for (ModInfoTabId id : kDisableable) {
    const int order_idx = static_cast<int>(id);
    if (order_idx < 0 || order_idx >= static_cast<int>(tab_order_.size()))
      continue;
    const size_t idx = static_cast<size_t>(order_idx);
    if (!tab_loaded_[idx]) {
      tab_order_[idx]->set_current(current_mod_data_);
      tab_order_[idx]->set_mod(current_mod_data_);
      tab_loaded_[idx] = true;
    }
    const bool enabled = tab_order_[idx]->has_data();
    const int visual = tabs_->indexOf(tab_order_[idx]);
    if (visual >= 0)
      tabs_->setTabEnabled(visual, enabled);
  }
  // Remaining tabs are always enabled.
  for (size_t i = 5; i < tab_order_.size(); ++i) {
    const int visual = tabs_->indexOf(tab_order_[i]);
    if (visual >= 0)
      tabs_->setTabEnabled(visual, true);
  }
}

void ModInfoDialog::switch_to(int index) {
  if (index == nav_index_)
    return;
  if (!can_switch())
    return;

  // Only tabs that were actually initialized (set_current() + set_mod())
  // hold a valid ModInfoData; unvisited tabs have null std::function
  // members and would crash in save_state() (std::bad_function_call).
  for (size_t i = 0; i < tab_order_.size(); ++i)
    if (tab_loaded_[i])
      tab_order_[i]->save_state();

  const QString &target_id = nav_list_[static_cast<size_t>(index)].first;
  if (data_builder_) {
    current_mod_data_ = data_builder_(target_id);
    tab_loaded_.assign(tab_order_.size(), false);
    tab_activated_.assign(tab_order_.size(), false);
    // Re-init the currently visible tab: currentChanged only fires on index
    // change, so without this the visible tab would keep stale data and its
    // save_state() would be skipped (tab_loaded_ reset to false).
    // Resolve via currentWidget() to get the logical index - currentIndex()
    // is visual and shifts when tabs are dragged (setMovable(true)).
    QWidget *cur = tabs_->currentWidget();
    auto it = std::find(tab_order_.begin(), tab_order_.end(), cur);
    if (it != tab_order_.end()) {
      const size_t logical = static_cast<size_t>(it - tab_order_.begin());
      tab_loaded_[logical] = true;
      (*it)->set_current(current_mod_data_);
      (*it)->set_mod(current_mod_data_);
      // Re-activate the visible tab: first_activation() is the only place
      // ImagesTab loads its pixmaps. currentChanged does not fire here
      // (same tab index), so without this thumbnails stay text-only.
      tab_activated_[logical] = true;
      (*it)->first_activation();
    }
  }
  // Always refresh enabled states so they do not go stale when
  // data_builder_ is null (e.g. preview harness) - update handles
  // not-yet-loaded tabs via eager set_mod.
  update_tab_enabled_states();

  load_index(index);
}

int ModInfoDialog::next_nav_index(int from, int dir) const {
  for (int i = from + dir; i >= 0 && i < static_cast<int>(nav_list_.size());
       i += dir) {
    if (!nav_list_[static_cast<size_t>(i)].second)
      return i;
  }
  return -1;
}

void ModInfoDialog::reload_current(ModInfoData data) {
  if (!can_switch())
    return;
  // Only tabs that were actually initialized (set_current() + set_mod())
  // hold a valid ModInfoData; unvisited tabs have null std::function
  // members and would crash in save_state() (std::bad_function_call).
  for (size_t i = 0; i < tab_order_.size(); ++i)
    if (tab_loaded_[i])
      tab_order_[i]->save_state();
  current_mod_data_ = std::move(data);

  // Mark all tabs as needing reload.
  tab_loaded_.assign(tab_order_.size(), false);
  tab_activated_.assign(tab_order_.size(), false);
  // Re-init the currently visible tab (same rationale as switch_to()).
  // Use currentWidget() to derive logical index - currentIndex() is visual.
  QWidget *cur = tabs_->currentWidget();
  auto it = std::find(tab_order_.begin(), tab_order_.end(), cur);
  if (it != tab_order_.end()) {
    const size_t logical = static_cast<size_t>(it - tab_order_.begin());
    tab_loaded_[logical] = true;
    (*it)->set_current(current_mod_data_);
    (*it)->set_mod(current_mod_data_);
    // Re-activate the visible tab (see switch_to() for rationale).
    tab_activated_[logical] = true;
    (*it)->first_activation();
  }

  update_tab_enabled_states();

  load_index(nav_index_);
}

QString ModInfoDialog::current_mod_id() const { return current_mod_data_.id; }

bool ModInfoDialog::can_switch() const {
  for (auto *tab : tab_order_) {
    if (!tab->can_close())
      return false;
  }
  return true;
}

void ModInfoDialog::on_delete_mod() {
  if (current_mod_data_.is_separator || current_mod_data_.is_overwrite ||
      current_mod_data_.is_merged || current_mod_data_.is_game_native ||
      !current_mod_data_.delete_mod) {
    return;
  }

  if (QMessageBox::question(
          this, tr("Delete Mod"),
          tr("Delete the mod \"%1\"?").arg(current_mod_data_.name),
          QMessageBox::Yes | QMessageBox::No,
          QMessageBox::No) != QMessageBox::Yes) {
    return;
  }

  if (current_mod_data_.delete_mod()) {
    persist_geometry();
    accept(); // the mod list refresh invalidates the data we hold
  }
}

void ModInfoDialog::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Delete) {
    on_delete_mod();
    event->accept();
    return;
  }
  QDialog::keyPressEvent(event);
}

void ModInfoDialog::persist_geometry() {
  Settings::instance().set_modinfo_last_tab(tabs_->currentIndex());
  Settings::instance().set_modinfo_window_geometry(saveGeometry());
}

void ModInfoDialog::restore_geometry() {
  const auto geo = Settings::instance().modinfo_window_geometry();
  if (!geo.isEmpty())
    restoreGeometry(geo);
}

void ModInfoDialog::closeEvent(QCloseEvent *event) {
  if (!can_switch()) {
    event->ignore();
    return;
  }
  // Only tabs that were actually initialized (set_current() + set_mod())
  // hold a valid ModInfoData; unvisited tabs have null std::function
  // members and would crash in save_state() (std::bad_function_call).
  for (size_t i = 0; i < tab_order_.size(); ++i)
    if (tab_loaded_[i])
      tab_order_[i]->save_state();
  persist_geometry();
  QDialog::closeEvent(event);
}

void ModInfoDialog::moveEvent(QMoveEvent *event) {
  QDialog::moveEvent(event);
  // Persist on move so a crashed/dropped dialog still restores its spot.
  Settings::instance().set_modinfo_window_geometry(saveGeometry());
}

} // namespace ui
