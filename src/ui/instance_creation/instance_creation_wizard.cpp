#include "ui/instance_creation/instance_creation_wizard.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QFormLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "engine/game/detect/game_detector.h"
#include "engine/instance/instance_utils.h"
#include "engine/plugin_host/plugin_loader.h"
#include "ui/game_selection/game_card.h"
#include "ui/settings/settings.h"
#include "ui/widgets/file_dialog_memory.h"

namespace ui {

namespace {
  std::vector<GameEntry> build_installed(engine::PluginLoader *loader) {
    std::vector<GameEntry> out;
    if (!loader)
      return out;
    std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> specs;
    for (const auto &p : loader->game_plugins()) {
      if (p.steam_appid > 0)
        specs.push_back({p.steam_appid, {p.game_id, p.game_display_name}});
    }
    auto installed = engine::GameDetector::detect_steam_games_multi(specs);
    for (const auto &g : installed) {
      GameEntry e;
      e.game_id      = g.game_id;
      e.display_name = g.name;
      e.steam_appid  = g.steam_appid;
      e.installed    = true;
      e.install_path = g.install_path;
      out.push_back(e);
    }
    return out;
  }
  std::vector<GameEntry> build_available(engine::PluginLoader *loader,
                                         const std::vector<GameEntry> &installed) {
    std::vector<GameEntry> out;
    if (!loader)
      return out;
    for (const auto &p : loader->game_plugins()) {
      bool found = false;
      for (const auto &ie : installed)
        if (ie.game_id == p.game_id) {
          found = true;
          break;
        }
      if (found)
        continue;
      GameEntry e;
      e.game_id      = p.game_id;
      e.display_name = p.game_display_name;
      e.steam_appid  = p.steam_appid;
      e.installed    = false;
      out.push_back(e);
    }
    // Generic instance entry
    GameEntry g;
    g.game_id      = "";
    g.display_name = "Generic Instance";
    g.installed    = false;
    out.push_back(g);
    return out;
  }
}  // namespace

InstanceCreationWizard::InstanceCreationWizard(engine::PluginLoader *loader,
                                               QWidget *parent,
                                               const GameEntry *preselected)
    : QDialog(parent), loader_(loader) {
  setWindowTitle(tr("Create New Instance"));
  resize(760, 520);
  setMinimumSize(640, 420);

  installed_entries_ = build_installed(loader_);
  available_entries_ = build_available(loader_, installed_entries_);

  if (preselected)
    chosen_game_ = *preselected;
  else if (!installed_entries_.empty())
    chosen_game_ = installed_entries_.front();
  else if (!available_entries_.empty())
    chosen_game_ = available_entries_.front();

  build_steps();

  auto *layout   = new QHBoxLayout(this);
  auto *splitter = new QSplitter(Qt::Horizontal, this);
  layout->addWidget(splitter);

  sidebar_ = new QListWidget(splitter);
  sidebar_->setMinimumWidth(120);
  sidebar_->setMaximumWidth(200);
  for (const auto &step : steps_) {
    auto *item = new QListWidgetItem(step.title, sidebar_);
    if (step.skipped) {
      item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
    }
  }
  splitter->addWidget(sidebar_);

  auto *right        = new QWidget(splitter);
  auto *right_layout = new QVBoxLayout(right);
  right_layout->setContentsMargins(0, 0, 0, 0);
  stack_ = new QStackedWidget(right);
  build_pages();
  right_layout->addWidget(stack_, 1);

  auto *nav = new QHBoxLayout();
  nav->addStretch(1);
  auto *cancel_button = new QPushButton(tr("Cancel"), right);
  back_button_        = new QPushButton(tr("< Back"), right);
  next_button_        = new QPushButton(tr("Next >"), right);
  next_button_->setDefault(true);
  nav->addWidget(cancel_button);
  nav->addWidget(back_button_);
  nav->addWidget(next_button_);
  right_layout->addLayout(nav);
  splitter->addWidget(right);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 7);

  connect(back_button_, &QPushButton::clicked, this, &InstanceCreationWizard::on_back);
  connect(next_button_, &QPushButton::clicked, this, &InstanceCreationWizard::on_next);
  connect(cancel_button, &QPushButton::clicked, this,
          &InstanceCreationWizard::on_cancel);

  // Skip variants page when no variants registered for chosen game.
  // (Recomputed on game change; initial here.)
  go_to(0);
  refresh_chrome();
}

void InstanceCreationWizard::build_steps() {
  steps_.clear();
  auto add = [&](int id, const QString &title, bool skipped) {
    StepState s;
    s.id      = id;
    s.title   = title;
    s.skipped = skipped;
    steps_.append(s);
  };
  // Variants skipped unless chosen game has variants.
  bool has_variants = false;
  if (loader_ && !chosen_game_.game_id.empty())
    has_variants = !loader_->game_variants(chosen_game_.game_id).empty();

  add(PageGame, tr("Game"), false);
  add(PageVariants, tr("Variants"), !has_variants);
  add(PageName, tr("Name"), false);
  add(PageProfile, tr("Profile"), false);
  add(PagePaths, tr("Paths"), false);
  add(PageConfirm, tr("Confirmation"), false);
}

void InstanceCreationWizard::build_pages() {
  stack_->addWidget(build_game_page());
  stack_->addWidget(build_variants_page());
  stack_->addWidget(build_name_page());
  stack_->addWidget(build_profile_page());
  stack_->addWidget(build_paths_page());
  stack_->addWidget(build_confirm_page());
}

void InstanceCreationWizard::go_to(int index) {
  if (index < 0 || index >= steps_.size())
    return;
  if (steps_[index].skipped)
    return;
  current_                 = index;
  steps_[current_].visited = true;
  stack_->setCurrentIndex(current_);
  on_page_entered(current_);
  refresh_chrome();
}

void InstanceCreationWizard::on_next() {
  if (current_ == PageName && !name_valid()) {
    QMessageBox::information(this, tr("Name"),
                             tr("Please enter a valid instance name."));
    return;
  }
  if (current_ == PagePaths && !game_dir_valid()) {
    QMessageBox::information(this, tr("Paths"),
                             tr("Please choose a valid game directory."));
    return;
  }
  if (current_ >= steps_.size() - 1) {
    // Finish: create instance.
    // Collect options.
    options_.game_variant = variant_combo_
                                ? variant_combo_->currentData().toString().toStdString()
                                : std::string{};
    if (variant_combo_ && options_.game_variant.empty())
      options_.game_variant = variant_combo_->currentText().toStdString();
    // If "none" selected, clear.
    // Name
    QString name = name_edit_ ? name_edit_->text().trimmed() : QString();
    if (name.isEmpty()) {
      QMessageBox::warning(this, tr("Error"), tr("Instance name is required."));
      return;
    }
    std::string sanitized = engine::sanitize_directory_name(name.toStdString());
    if (sanitized.empty()) {
      QMessageBox::warning(this, tr("Error"), tr("Invalid instance name."));
      return;
    }
    // uniqueness
    auto existing = engine::scan_instances();
    if (std::find(existing.begin(), existing.end(), sanitized) != existing.end()) {
      QMessageBox::warning(
          this, tr("Error"),
          tr("An instance named \"%1\" already exists.").arg(sanitized));
      return;
    }
    // game dir
    if (game_dir_edit_) {
      QString gd = game_dir_edit_->text().trimmed();
      if (!gd.isEmpty()) {
        // validate via plugin loader if available
        if (loader_ && !chosen_game_.game_id.empty()) {
          if (!loader_->validate_game_dir(chosen_game_.game_id,
                                          std::filesystem::path(gd.toStdString()))) {
            QMessageBox::warning(this, tr("Error"), tr("Invalid game directory."));
            return;
          }
        }
        // store into chosen_game_ install_path? We'll set via DetectedGame in finish.
      }
    }
    // Profile seed
    options_.profile_seed.local_settings =
        local_settings_check_ ? local_settings_check_->isChecked() : false;
    options_.profile_seed.local_saves =
        local_saves_check_ ? local_saves_check_->isChecked() : false;
    options_.profile_seed.archive_invalidation =
        archive_invalidation_check_ ? archive_invalidation_check_->isChecked() : true;
    // Paths overrides
    if (mods_dir_edit_ && !mods_dir_edit_->text().trimmed().isEmpty())
      options_.mods_dir =
          std::filesystem::path(mods_dir_edit_->text().trimmed().toStdString());
    if (downloads_dir_edit_ && !downloads_dir_edit_->text().trimmed().isEmpty())
      options_.downloads_dir =
          std::filesystem::path(downloads_dir_edit_->text().trimmed().toStdString());
    if (cache_dir_edit_ && !cache_dir_edit_->text().trimmed().isEmpty())
      options_.cache_dir =
          std::filesystem::path(cache_dir_edit_->text().trimmed().toStdString());
    if (profiles_dir_edit_ && !profiles_dir_edit_->text().trimmed().isEmpty())
      options_.profiles_dir =
          std::filesystem::path(profiles_dir_edit_->text().trimmed().toStdString());
    if (overwrite_dir_edit_ && !overwrite_dir_edit_->text().trimmed().isEmpty())
      options_.overwrite_dir =
          std::filesystem::path(overwrite_dir_edit_->text().trimmed().toStdString());

    // Create instance
    engine::DetectedGame dg;
    dg.game_id     = chosen_game_.game_id;
    dg.name        = chosen_game_.display_name;
    dg.steam_appid = chosen_game_.steam_appid;
    if (game_dir_edit_ && !game_dir_edit_->text().trimmed().isEmpty())
      dg.install_path =
          std::filesystem::path(game_dir_edit_->text().trimmed().toStdString());
    else
      dg.install_path = chosen_game_.install_path;

    auto instances_dir = engine::default_instances_dir();
    auto inst = engine::create_instance_for_game(dg, instances_dir, name.toStdString(),
                                                 options_);
    if (inst.info().game_id.empty() && chosen_game_.game_id.empty() &&
        chosen_game_.display_name != "Generic Instance") {
      // generic instance has empty game_id; check root
    }
    if (inst.info().root.empty()) {
      QMessageBox::warning(this, tr("Error"), tr("Failed to create instance."));
      return;
    }
    // Create default profile with seed
    if (options_.create_default_profile) {
      auto profiles_dir = inst.path_for(engine::InstanceKind::Profiles);
      engine::profile::create_fresh_profile(profiles_dir, "Default", {},
                                            &options_.profile_seed);
    }
    created_name_ = QString::fromStdString(inst.info().root.filename().string());
    accept();
    return;
  }
  int next = current_ + 1;
  while (next < steps_.size() && steps_[next].skipped)
    ++next;
  if (next >= steps_.size()) {
    accept();
    return;
  }
  go_to(next);
}

void InstanceCreationWizard::on_back() {
  int prev = current_ - 1;
  while (prev >= 0 && steps_[prev].skipped)
    --prev;
  if (prev >= 0)
    go_to(prev);
}

void InstanceCreationWizard::on_cancel() {
  reject();
}

void InstanceCreationWizard::refresh_chrome() {
  const QPalette palette = this->palette();
  const QColor dim       = palette.color(QPalette::Disabled, QPalette::WindowText);
  for (int i = 0; i < steps_.size(); ++i) {
    auto *item = sidebar_->item(i);
    if (!item)
      continue;
    QString label = steps_[i].title;
    QFont font    = item->font();
    if (steps_[i].skipped) {
      font.setStrikeOut(true);
      item->setForeground(dim);
    } else if (i == current_) {
      font.setBold(true);
      label = QStringLiteral("\u25CF ") + label;
    } else if (steps_[i].visited) {
      font.setBold(false);
      label = QStringLiteral("\u2713 ") + label;
    } else {
      font.setBold(false);
      label = QStringLiteral("\u25CB ") + label;
    }
    item->setFont(font);
    item->setText(label);
  }
  sidebar_->setCurrentRow(current_);
  int first = 0;
  while (first < steps_.size() && steps_[first].skipped)
    ++first;
  back_button_->setVisible(current_ != first);
  const bool is_last = current_ == steps_.size() - 1;
  next_button_->setText(is_last ? tr("Finish") : tr("Next >"));
  refresh_next_enabled();
}

void InstanceCreationWizard::refresh_next_enabled() {
  if (current_ == PageName) {
    next_button_->setEnabled(name_valid());
  } else if (current_ == PagePaths) {
    next_button_->setEnabled(game_dir_valid() ||
                             (game_dir_edit_ &&
                              game_dir_edit_->text().trimmed().isEmpty() &&
                              chosen_game_.installed));
  } else {
    next_button_->setEnabled(true);
  }
}

void InstanceCreationWizard::on_page_entered(int index) {
  if (index == PageVariants && variant_combo_) {
    variant_combo_->clear();
    if (loader_ && !chosen_game_.game_id.empty()) {
      auto variants = loader_->game_variants(chosen_game_.game_id);
      // Dedup by variant_id (defensive; game_variants already dedups).
      std::vector<std::string> seen;
      for (const auto &v : variants) {
        if (std::find(seen.begin(), seen.end(), v.variant_id) != seen.end())
          continue;
        seen.push_back(v.variant_id);
        variant_combo_->addItem(QString::fromStdString(v.display_name),
                                QString::fromStdString(v.variant_id));
      }
    }
    if (variant_combo_->count() == 0) {
      // skip page
      steps_[index].skipped = true;
    }
  }
  if (index == PageName && name_edit_) {
    if (name_edit_->text().isEmpty())
      name_edit_->setText(QString::fromStdString(chosen_game_.display_name));
    // validate
  }
  if (index == PageProfile && archive_invalidation_check_) {
    archive_invalidation_check_->setChecked(true);
  }
}

QString InstanceCreationWizard::name_validation_message() const {
  if (!name_edit_)
    return QString();
  QString text = name_edit_->text().trimmed();
  if (text.isEmpty())
    return tr("Name cannot be empty.");
  if (text == "." || text == "..")
    return tr("Name cannot be \".\" or \"..\".");
  const QString bad_chars = QStringLiteral("\\/:*?\"<>|");
  QString found;
  bool has_control = false;
  for (const QChar c : text) {
    if (c.unicode() < 32) {
      has_control = true;
    } else if (bad_chars.contains(c) && !found.contains(c)) {
      if (!found.isEmpty())
        found += QLatin1Char(' ');
      found += c;
    }
  }
  if (!found.isEmpty() || has_control) {
    QString chars = found;
    if (has_control) {
      if (!chars.isEmpty())
        chars += QLatin1Char(' ');
      chars += tr("control characters");
    }
    return tr("Name cannot contain: %1").arg(chars);
  }
  std::string sanitized = engine::sanitize_directory_name(text.toStdString());
  if (sanitized.empty())
    return tr("Name cannot be empty.");
  auto existing = engine::scan_instances();
  if (std::find(existing.begin(), existing.end(), sanitized) != existing.end())
    return tr("An instance named \"%1\" already exists.")
        .arg(QString::fromStdString(sanitized));
  return QString();
}

bool InstanceCreationWizard::name_valid() const {
  return name_validation_message().isEmpty();
}

bool InstanceCreationWizard::game_dir_valid() const {
  if (!game_dir_edit_)
    return true;
  QString text = game_dir_edit_->text().trimmed();
  if (text.isEmpty())
    return chosen_game_
        .installed;  // installed games have known dir; empty ok for installed?
  // For not-installed, require valid
  if (loader_ && !chosen_game_.game_id.empty()) {
    return loader_->validate_game_dir(chosen_game_.game_id,
                                      std::filesystem::path(text.toStdString()));
  }
  return true;
}

QWidget *InstanceCreationWizard::build_game_page() {
  auto *page   = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(new QLabel(tr("Select a game:"), page));
  auto *list = new QListWidget(page);
  for (const auto &e : installed_entries_) {
    auto *item = new QListWidgetItem(
        QString::fromStdString(e.display_name) + tr(" (installed)"), list);
    item->setData(Qt::UserRole, QString::fromStdString(e.game_id));
  }
  for (const auto &e : available_entries_) {
    auto *item = new QListWidgetItem(QString::fromStdString(e.display_name), list);
    item->setData(Qt::UserRole, QString::fromStdString(e.game_id));
  }
  layout->addWidget(list, 1);
  connect(list, &QListWidget::itemClicked, this, [this, list](QListWidgetItem *item) {
    QString gid = item->data(Qt::UserRole).toString();
    // find entry
    for (const auto &e : installed_entries_)
      if (QString::fromStdString(e.game_id) == gid) {
        chosen_game_ = e;
        chosen_ok_   = true;
        break;
      }
    if (!chosen_ok_) {
      for (const auto &e : available_entries_)
        if (QString::fromStdString(e.game_id) == gid) {
          chosen_game_ = e;
          chosen_ok_   = true;
          break;
        }
    }
    // rebuild steps for variants
    build_steps();
  });
  return page;
}

QWidget *InstanceCreationWizard::build_variants_page() {
  auto *page   = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(new QLabel(tr("Game variant:"), page));
  variant_combo_ = new QComboBox(page);
  layout->addWidget(variant_combo_);
  layout->addStretch(1);
  return page;
}

QWidget *InstanceCreationWizard::build_name_page() {
  auto *page   = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(new QLabel(tr("Instance name:"), page));
  name_edit_ = new QLineEdit(page);
  layout->addWidget(name_edit_);
  name_error_ = new QLabel(page);
  name_error_->setStyleSheet(QStringLiteral("color: red;"));
  name_error_->setWordWrap(true);
  name_error_->hide();
  layout->addWidget(name_error_);
  connect(name_edit_, &QLineEdit::textChanged, this, [this]() {
    if (!name_error_)
      return;
    QString msg = name_validation_message();
    if (msg.isEmpty()) {
      name_error_->hide();
    } else {
      name_error_->setText(msg);
      name_error_->show();
    }
    refresh_next_enabled();
  });
  layout->addStretch(1);
  return page;
}

QWidget *InstanceCreationWizard::build_profile_page() {
  auto *page   = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(new QLabel(tr("Profile settings:"), page));
  archive_invalidation_check_ =
      new QCheckBox(tr("Automatic archive invalidation"), page);
  archive_invalidation_check_->setChecked(true);
  local_settings_check_ = new QCheckBox(tr("Local settings"), page);
  local_saves_check_    = new QCheckBox(tr("Local saves"), page);
  layout->addWidget(archive_invalidation_check_);
  layout->addWidget(local_settings_check_);
  layout->addWidget(local_saves_check_);
  layout->addStretch(1);
  return page;
}

QWidget *InstanceCreationWizard::build_paths_page() {
  auto *page   = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(new QLabel(tr("Game directory:"), page));
  auto *gd_row   = new QHBoxLayout();
  game_dir_edit_ = new QLineEdit(page);
  if (chosen_game_.installed)
    game_dir_edit_->setText(QString::fromStdString(chosen_game_.install_path.string()));
  gd_row->addWidget(game_dir_edit_, 1);
  auto *browse = new QPushButton(tr("Browse..."), page);
  gd_row->addWidget(browse);
  layout->addLayout(gd_row);
  game_dir_error_ = new QLabel(page);
  game_dir_error_->setStyleSheet(QStringLiteral("color: red;"));
  game_dir_error_->hide();
  layout->addWidget(game_dir_error_);
  connect(game_dir_edit_, &QLineEdit::textChanged, this, [this]() {
    if (game_dir_valid())
      game_dir_error_->hide();
    else {
      game_dir_error_->setText(tr("Invalid game directory."));
      game_dir_error_->show();
    }
    refresh_next_enabled();
  });
  connect(browse, &QPushButton::clicked, this, [this]() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Choose game directory"),
                                                    game_dir_edit_->text());
    if (!dir.isEmpty())
      game_dir_edit_->setText(dir);
  });

  auto *form          = new QFormLayout();
  mods_dir_edit_      = new QLineEdit(page);
  downloads_dir_edit_ = new QLineEdit(page);
  cache_dir_edit_     = new QLineEdit(page);
  profiles_dir_edit_  = new QLineEdit(page);
  overwrite_dir_edit_ = new QLineEdit(page);
  mods_dir_edit_->setPlaceholderText(QStringLiteral("$BASE_DIRECTORY/mods"));
  downloads_dir_edit_->setPlaceholderText(QStringLiteral("$BASE_DIRECTORY/downloads"));
  cache_dir_edit_->setPlaceholderText(QStringLiteral("$BASE_DIRECTORY/cache"));
  profiles_dir_edit_->setPlaceholderText(QStringLiteral("$BASE_DIRECTORY/profiles"));
  overwrite_dir_edit_->setPlaceholderText(QStringLiteral("$BASE_DIRECTORY/overwrite"));
  form->addRow(tr("Mods"), mods_dir_edit_);
  form->addRow(tr("Downloads"), downloads_dir_edit_);
  form->addRow(tr("Cache"), cache_dir_edit_);
  form->addRow(tr("Profiles"), profiles_dir_edit_);
  form->addRow(tr("Overwrite"), overwrite_dir_edit_);
  layout->addLayout(form);
  layout->addStretch(1);
  return page;
}

QWidget *InstanceCreationWizard::build_confirm_page() {
  auto *page   = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(new QLabel(tr("Review and click Finish to create."), page));
  layout->addStretch(1);
  return page;
}

}  // namespace ui