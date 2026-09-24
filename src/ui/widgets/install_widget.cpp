#include "ui/widgets/install_widget.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>
#include <Qt>

#include <algorithm>
#include <set>

#include "engine/collection/choice_groups.h"
#include "engine/gmmpack/ini_edit_parser.h"
#include "engine/modpack/ini_edits.h"

namespace ui {
namespace {

  // Display glyphs for each status in the step list.
  QString status_glyph(StepStatus status) {
    switch (status) {
    case StepStatus::Pending:
      return QStringLiteral("\u25cb");
    case StepStatus::Running:
      return QStringLiteral("\u25b6");
    case StepStatus::Done:
      return QStringLiteral("\u2713");
    case StepStatus::Error:
      return QStringLiteral("\u2717");
    case StepStatus::Skipped:
      return QStringLiteral("\u2013");
    }
    return QStringLiteral("?");
  }

  // gmmpack choice groups (string mode) -> engine Collection groups (enum
  // mode). "exactly-one" maps to ExactlyOne; anything else is AtMostOne.
  std::vector<engine::Collection::ChoiceGroup>
  to_collection_groups(const std::vector<engine::gmmpack::ChoiceGroup> &raw) {
    std::vector<engine::Collection::ChoiceGroup> out;
    out.reserve(raw.size());
    for (const auto &g : raw) {
      engine::Collection::ChoiceGroup c;
      c.id   = g.id;
      c.name = g.name;
      c.mode = g.mode == "exactly-one" ? engine::Collection::ChoiceMode::ExactlyOne
                                       : engine::Collection::ChoiceMode::AtMostOne;
      c.member_mod_ids = g.member_mod_ids;
      out.push_back(std::move(c));
    }
    return out;
  }

  // UpdatePlan diagnostic path -> owning step (mirrors set_update_plan docs).
  InstallStep step_for_path(const std::string &path) {
    if (path.rfind("mods/", 0) == 0)
      return InstallStep::Resolve;
    if (path.rfind("patches/", 0) == 0)
      return InstallStep::PatchConsent;
    if (path.rfind("ini/", 0) == 0)
      return InstallStep::IniEdits;
    if (path.rfind("tree", 0) == 0)
      return InstallStep::BuildTree;
    if (path.rfind("executables/", 0) == 0)
      return InstallStep::SetupExes;
    return InstallStep::Loot;
  }

}  // namespace

// ---------------------------------------------------------------------------
// InstallStepModel
// ---------------------------------------------------------------------------

InstallStepModel::InstallStepModel(QObject *parent) : QAbstractListModel(parent) {
  for (int i = 0; i < kStepCount; ++i) {
    statuses_[i]    = StepStatus::Pending;
    diag_counts_[i] = 0;
  }
}

QString InstallStepModel::title(InstallStep step) {
  switch (step) {
  case InstallStep::Resolve:
    return QStringLiteral("Resolving sources");
  case InstallStep::Download:
    return QStringLiteral("Downloading");
  case InstallStep::PatchConsent:
    return QStringLiteral("Patch consent");
  case InstallStep::Install:
    return QStringLiteral("Installing mods");
  case InstallStep::IniEdits:
    return QStringLiteral("Applying INI edits");
  case InstallStep::BuildTree:
    return QStringLiteral("Building tree");
  case InstallStep::SetupExes:
    return QStringLiteral("Running setup executables");
  case InstallStep::Loot:
    return QStringLiteral("Running LOOT");
  case InstallStep::ChoiceGroups:
    return QStringLiteral("Choice groups");
  case InstallStep::Done:
    return QStringLiteral("Done");
  }
  return {};
}

int InstallStepModel::row_of(InstallStep step) {
  return static_cast<int>(step);
}

int InstallStepModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : kStepCount;
}

QVariant InstallStepModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= kStepCount) {
    return {};
  }
  const auto step = static_cast<InstallStep>(index.row());
  if (role == Qt::DisplayRole) {
    QString text = status_glyph(statuses_[index.row()])
                       .append(QLatin1Char(' '))
                       .append(title(step));
    if (diag_counts_[index.row()] > 0) {
      text.append(QStringLiteral(" (%1)").arg(diag_counts_[index.row()]));
    }
    if (!diag_previews_[index.row()].isEmpty()) {
      text.append(QStringLiteral(" - ")).append(diag_previews_[index.row()]);
    }
    return text;
  }
  if (role == Qt::ToolTipRole) {
    return diag_previews_[index.row()];
  }
  if (role == StatusRole) {
    return static_cast<int>(statuses_[index.row()]);
  }
  if (role == StepRole) {
    return index.row();
  }
  return {};
}

void InstallStepModel::notify_row(int row) {
  const auto idx = index(row, 0);
  emit dataChanged(idx, idx, {Qt::DisplayRole, Qt::ToolTipRole, StatusRole, StepRole});
}

void InstallStepModel::set_status(InstallStep step, StepStatus status) {
  const int row = row_of(step);
  if (statuses_[row] == status) {
    return;
  }
  statuses_[row] = status;
  notify_row(row);
}

StepStatus InstallStepModel::status(InstallStep step) const {
  return statuses_[row_of(step)];
}

void InstallStepModel::set_diagnostic_count(InstallStep step, int count) {
  const int row = row_of(step);
  if (diag_counts_[row] == count) {
    return;
  }
  diag_counts_[row] = count;
  notify_row(row);
}

void InstallStepModel::set_diagnostic_preview(InstallStep step,
                                              const QString &preview) {
  const int row = row_of(step);
  if (diag_previews_[row] == preview) {
    return;
  }
  diag_previews_[row] = preview;
  notify_row(row);
}

// ---------------------------------------------------------------------------
// InstallWidget
// ---------------------------------------------------------------------------

InstallWidget::InstallWidget(QWidget *parent) : QWidget(parent) {
  build_ui();
}

void InstallWidget::build_ui() {
  auto *outer = new QVBoxLayout(this);

  auto *splitter = new QSplitter(Qt::Horizontal, this);
  outer->addWidget(splitter, 1);

  // Left pane: step list on top, per-step detail below.
  auto *left        = new QWidget(splitter);
  auto *left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 0, 0);
  model_     = new InstallStepModel(this);
  step_list_ = new QListView(left);
  step_list_->setObjectName(QStringLiteral("install_step_list"));
  step_list_->setModel(model_);
  step_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  left_layout->addWidget(step_list_, 1);
  detail_ = new QStackedWidget(left);
  left_layout->addWidget(detail_, 2);
  for (int row = 0; row < InstallStepModel::kStepCount; ++row) {
    build_step_page(static_cast<InstallStep>(row));
  }
  connect(step_list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &InstallWidget::on_step_selected);
  step_list_->setCurrentIndex(model_->index(0, 0));
  splitter->addWidget(left);

  // Right pane: static instructions for the whole install.
  instructions_ = new QTextBrowser(splitter);
  instructions_->setOpenExternalLinks(true);
  splitter->addWidget(instructions_);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);

  // Bottom: persistent diagnostics badge + expandable full list.
  auto *diag_bar = new QHBoxLayout();
  outer->addLayout(diag_bar);
  diag_badge_ = new QToolButton(this);
  diag_badge_->setCheckable(true);
  diag_badge_->setChecked(false);
  diag_bar->addWidget(diag_badge_);
  diag_bar->addStretch(1);
  diag_list_ = new QListWidget(this);
  diag_list_->setVisible(false);
  outer->addWidget(diag_list_);
  connect(diag_badge_, &QToolButton::toggled, this,
          &InstallWidget::on_diagnostics_toggled);
  refresh_badge();
}

// One detail page per step: a status line plus that step's inline
// diagnostics. Interactive steps get their controls rebuilt by
// rebuild_*_page() on top of this shell.
void InstallWidget::build_step_page(InstallStep step) {
  const int row            = static_cast<int>(step);
  auto *page               = new QWidget(detail_);
  auto *layout             = new QVBoxLayout(page);
  step_status_labels_[row] = new QLabel(InstallStepModel::title(step), page);
  layout->addWidget(step_status_labels_[row]);
  step_diag_lists_[row] = new QListWidget(page);
  step_diag_lists_[row]->setMaximumHeight(96);
  layout->addWidget(step_diag_lists_[row]);
  if (step == InstallStep::PatchConsent) {
    patch_list_ = new QListWidget(page);
    layout->addWidget(patch_list_, 1);
    auto *buttons = new QHBoxLayout();
    allow_button_ = new QPushButton(tr("Allow"), page);
    deny_button_  = new QPushButton(tr("Do Not Allow"), page);
    buttons->addWidget(allow_button_);
    buttons->addWidget(deny_button_);
    layout->addLayout(buttons);
    connect(allow_button_, &QPushButton::clicked, this,
            &InstallWidget::on_allow_patches);
    connect(deny_button_, &QPushButton::clicked, this, &InstallWidget::on_deny_patches);
  }
  if (step == InstallStep::IniEdits) {
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    ini_body_         = new QWidget(scroll);
    auto *body_layout = new QVBoxLayout(ini_body_);
    body_layout->addStretch(1);
    scroll->setWidget(ini_body_);
    layout->addWidget(scroll, 1);
  }
  if (step == InstallStep::ChoiceGroups) {
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    choice_body_      = new QWidget(scroll);
    auto *body_layout = new QVBoxLayout(choice_body_);
    body_layout->addStretch(1);
    scroll->setWidget(choice_body_);
    layout->addWidget(scroll, 1);
  }
  step_pages_[row] = page;
  detail_->addWidget(page);
}

void InstallWidget::on_step_selected(const QModelIndex &current) {
  if (current.isValid()) {
    detail_->setCurrentIndex(current.row());
  }
}

// -- pack content ----------------------------------------------------------

void InstallWidget::set_pack(const engine::gmmpack::Gmmpack &pack) {
  set_fresh_install();
  // Patch consent: sorted unique patched mod ids.
  std::set<std::string> mods;
  for (const auto &p : pack.patches) {
    mods.insert(p.mod_id);
  }
  patch_mods_.assign(mods.begin(), mods.end());
  rebuild_patch_page();
  // INI tweaks grouped by target file, with overridden badges from the
  // shared merge engine (zero new INI logic here).
  std::vector<engine::modpack::IniEditFile> files;
  files.reserve(pack.ini_edits.size());
  for (const auto &entry : pack.ini_edits) {
    files.push_back(engine::gmmpack::to_edit_file(entry));
  }
  const auto merged = engine::modpack::merge_ini_edits(files);
  std::set<std::string> overridden;
  for (const auto &target : merged) {
    for (const auto &conflict : target.conflicts) {
      for (const auto &id : conflict.overridden_tweak_id) {
        if (!id.empty()) {
          overridden.insert(id);
        }
      }
    }
  }
  ini_order_.clear();
  ini_rows_.clear();
  // Rebuild grouped by the pack's own entries (first-seen file order).
  struct TweakView {
    std::string id;
    std::string name;
    bool required;
    std::string source;
    bool enabled;
    bool conflict;
  };
  std::vector<std::pair<std::string, std::vector<TweakView>>> grouped;
  for (const auto &entry : pack.ini_edits) {
    const auto file = engine::gmmpack::to_edit_file(entry);
    std::vector<TweakView> views;
    for (const auto &tweak : file.tweaks) {
      const bool required      = tweak.status == engine::modpack::TweakStatus::Required;
      const std::string source = tweak.source_mod_id.value_or("pack author");
      views.push_back({tweak.id, tweak.name, required, source, tweak.enabled,
                       overridden.count(tweak.id) > 0});
      ini_order_.push_back(tweak.id);
    }
    grouped.emplace_back(file.target_file, std::move(views));
  }
  // Build widgets from the grouped views.
  auto *body_layout = qobject_cast<QVBoxLayout *>(ini_body_->layout());
  for (const auto &[target_file, views] : grouped) {
    auto *group        = new QGroupBox(QString::fromStdString(target_file), ini_body_);
    auto *group_layout = new QVBoxLayout(group);
    for (const auto &view : views) {
      QString label = QString::fromStdString(view.name);
      label.append(view.required ? QStringLiteral(" [required]")
                                 : QStringLiteral(" [recommended]"));
      label.append(QStringLiteral(" (%1)").arg(QString::fromStdString(view.source)));
      if (view.conflict) {
        label.append(QStringLiteral(" [overridden]"));
      }
      auto *box = new QCheckBox(label, group);
      box->setChecked(view.enabled);
      box->setEnabled(!view.required);  // required tweaks are locked
      const QString id = QString::fromStdString(view.id);
      connect(box, &QCheckBox::toggled, this, [this, id](bool checked) {
        set_ini_tweak_enabled(id.toStdString(), checked);
      });
      group_layout->addWidget(box);
      ini_rows_[view.id] = IniRow{box, view.required};
    }
    body_layout->insertWidget(body_layout->count() - 1, group);
  }
  // Choice groups: convert to engine shape, carry no prior picks.
  choice_groups_ = to_collection_groups(pack.manifest.choice_groups);
  picks_.clear();
  rebuild_choice_page();
  // Instructions stay up the whole time.
  instructions_source_ =
      QString::fromStdString(pack.instructions.value_or(std::string()));
  instructions_->setMarkdown(instructions_source_);
}

void InstallWidget::set_fresh_install() {
  for (int row = 0; row < InstallStepModel::kStepCount; ++row) {
    set_step_status(static_cast<InstallStep>(row), StepStatus::Pending);
  }
  clear_diagnostics();
  patch_consent_.reset();
  refresh_badge();
}

void InstallWidget::set_update_plan(const engine::modpack::UpdatePlan &plan) {
  set_fresh_install();
  const bool mods_touched = !plan.fresh_installs.empty() || !plan.reinstalls.empty() ||
                            !plan.removals.empty();
  if (!mods_touched) {
    set_step_status(InstallStep::Resolve, StepStatus::Skipped);
    set_step_status(InstallStep::Download, StepStatus::Skipped);
    set_step_status(InstallStep::Install, StepStatus::Skipped);
  }
  if (plan.patch_consent_mods.empty()) {
    set_step_status(InstallStep::PatchConsent, StepStatus::Skipped);
  }
  if (plan.ini_changes.empty()) {
    set_step_status(InstallStep::IniEdits, StepStatus::Skipped);
  }
  if (plan.tree_changes.empty()) {
    set_step_status(InstallStep::BuildTree, StepStatus::Skipped);
  }
  for (const auto &diag : plan.diagnostics) {
    add_diagnostic(step_for_path(diag.path), QString::fromStdString(diag.message));
  }
}

// -- step state ------------------------------------------------------------

void InstallWidget::set_step_status(InstallStep step, StepStatus status) {
  model_->set_status(step, status);
  const int row = static_cast<int>(step);
  step_status_labels_[row]->setText(
      QStringLiteral("%1 %2").arg(status_glyph(status), InstallStepModel::title(step)));
  emit step_status_changed(step);
}

StepStatus InstallWidget::step_status(InstallStep step) const {
  return model_->status(step);
}

QString InstallWidget::step_title(InstallStep step) {
  return InstallStepModel::title(step);
}

// -- diagnostics -------------------------------------------------------------

void InstallWidget::add_diagnostic(InstallStep step, const QString &message) {
  diagnostics_.push_back({step, message});
  refresh_model_for(step);
  refresh_badge();
  emit diagnostics_changed();
}

void InstallWidget::clear_diagnostics() {
  diagnostics_.clear();
  for (int row = 0; row < InstallStepModel::kStepCount; ++row) {
    const auto step = static_cast<InstallStep>(row);
    model_->set_diagnostic_count(step, 0);
    model_->set_diagnostic_preview(step, {});
    step_diag_lists_[row]->clear();
  }
  diag_list_->clear();
  refresh_badge();
  emit diagnostics_changed();
}

const std::vector<StepDiagnostic> &InstallWidget::diagnostics() const {
  return diagnostics_;
}

int InstallWidget::diagnostic_count() const {
  return static_cast<int>(diagnostics_.size());
}

void InstallWidget::refresh_model_for(InstallStep step) {
  int count = 0;
  QString first;
  const int row = static_cast<int>(step);
  step_diag_lists_[row]->clear();
  for (const auto &diag : diagnostics_) {
    if (diag.step != step) {
      continue;
    }
    ++count;
    if (first.isEmpty()) {
      first = diag.message;
    }
    step_diag_lists_[row]->addItem(
        QStringLiteral("%1: %2").arg(InstallStepModel::title(step), diag.message));
    diag_list_->addItem(
        QStringLiteral("[%1] %2").arg(InstallStepModel::title(step), diag.message));
  }
  model_->set_diagnostic_count(step, count);
  model_->set_diagnostic_preview(step, first);
}

void InstallWidget::refresh_badge() {
  diag_badge_->setText(QStringLiteral("Diagnostics (%1)").arg(diagnostic_count()));
}

void InstallWidget::on_diagnostics_toggled(bool expanded) {
  diag_list_->setVisible(expanded);
}

// -- patch consent -------------------------------------------------------------

bool InstallWidget::has_patches() const {
  return !patch_mods_.empty();
}

std::optional<bool> InstallWidget::patch_consent() const {
  return patch_consent_;
}

void InstallWidget::set_patch_consent(bool allowed) {
  patch_consent_ = allowed;
  allow_button_->setEnabled(!allowed);
  deny_button_->setEnabled(allowed);
  if (!allowed) {
    add_diagnostic(InstallStep::PatchConsent,
                   tr("Patches declined: mods install unpatched."));
  }
  emit patch_consent_changed(allowed);
}

void InstallWidget::rebuild_patch_page() {
  patch_list_->clear();
  for (const auto &mod : patch_mods_) {
    patch_list_->addItem(QString::fromStdString(mod));
  }
  allow_button_->setEnabled(true);
  deny_button_->setEnabled(true);
}

void InstallWidget::on_allow_patches() {
  set_patch_consent(true);
}

void InstallWidget::on_deny_patches() {
  set_patch_consent(false);
}

// -- INI tweaks ------------------------------------------------------------------

std::vector<std::string> InstallWidget::ini_tweak_ids() const {
  return ini_order_;
}

bool InstallWidget::ini_tweak_enabled(const std::string &tweak_id) const {
  const auto it = ini_rows_.find(tweak_id);
  if (it == ini_rows_.end()) {
    return false;
  }
  return it->second.box->isChecked();
}

void InstallWidget::set_ini_tweak_enabled(const std::string &tweak_id, bool enabled) {
  const auto it = ini_rows_.find(tweak_id);
  if (it == ini_rows_.end()) {
    return;
  }
  if (it->second.required && !enabled) {
    return;  // required tweaks are locked on
  }
  const QSignalBlocker blocker(it->second.box);
  it->second.box->setChecked(enabled);
  emit ini_tweak_toggled(QString::fromStdString(tweak_id), enabled);
}

// -- choice groups -----------------------------------------------------------------

void InstallWidget::rebuild_choice_page() {
  qDeleteAll(choice_button_groups_);
  choice_button_groups_.clear();
  // Drop old group boxes, keep the trailing stretch.
  auto *layout      = choice_body_->layout();
  QLayoutItem *item = nullptr;
  while ((item = layout->takeAt(0)) != nullptr) {
    delete item->widget();
    delete item;
  }
  auto *body_layout = qobject_cast<QVBoxLayout *>(layout);
  for (const auto &group : choice_groups_) {
    auto *box        = new QGroupBox(QString::fromStdString(group.name), choice_body_);
    auto *box_layout = new QVBoxLayout(box);
    // Owned by the widget (qDeleteAll in rebuild), NOT by the group box:
    // the box deletes its radio children itself, so parenting the button
    // group there would double-delete it on the next rebuild.
    auto *buttons = new QButtonGroup(this);
    buttons->setExclusive(true);
    const auto picked         = picks_.find(group.id);
    const std::string current = (picked != picks_.end() && !picked->second.empty())
                                    ? picked->second.front()
                                    : std::string();
    for (const auto &member : group.member_mod_ids) {
      auto *radio = new QRadioButton(QString::fromStdString(member), box);
      radio->setProperty("gmm_group", QString::fromStdString(group.id));
      radio->setProperty("gmm_member", QString::fromStdString(member));
      radio->setChecked(!current.empty() && current == member);
      buttons->addButton(radio);
      box_layout->addWidget(radio);
      connect(radio, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
          sync_picks_from_ui();
        }
      });
    }
    if (group.mode == engine::Collection::ChoiceMode::AtMostOne) {
      auto *none = new QRadioButton(tr("None"), box);
      none->setProperty("gmm_group", QString::fromStdString(group.id));
      none->setProperty("gmm_member", QString());
      none->setChecked(current.empty());
      buttons->addButton(none);
      box_layout->addWidget(none);
      connect(none, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
          sync_picks_from_ui();
        }
      });
    }
    choice_button_groups_.push_back(buttons);
    body_layout->addWidget(box);
  }
  body_layout->addStretch(1);
}

engine::Collection::ChoicePicks InstallWidget::current_picks_from_ui() const {
  engine::Collection::ChoicePicks picks;
  for (const auto *buttons : choice_button_groups_) {
    for (const auto *button : buttons->buttons()) {
      const auto *radio = qobject_cast<const QRadioButton *>(button);
      if (radio != nullptr && radio->isChecked()) {
        const std::string group = radio->property("gmm_group").toString().toStdString();
        const std::string member =
            radio->property("gmm_member").toString().toStdString();
        if (!member.empty()) {
          picks[group] = {member};
        }
      }
    }
  }
  return picks;
}

void InstallWidget::sync_picks_from_ui() {
  picks_ = current_picks_from_ui();
  emit choice_picks_changed();
}

void InstallWidget::set_choice_picks(const engine::Collection::ChoicePicks &picks) {
  picks_ = picks;
  // Reflect into the radios without re-emitting per-button toggles.
  for (auto *buttons : choice_button_groups_) {
    const QSignalBlocker blocker(buttons);
    for (auto *button : buttons->buttons()) {
      auto *radio = qobject_cast<QRadioButton *>(button);
      if (radio == nullptr) {
        continue;
      }
      const std::string group  = radio->property("gmm_group").toString().toStdString();
      const std::string member = radio->property("gmm_member").toString().toStdString();
      const auto it            = picks_.find(group);
      const std::string current = (it != picks_.end() && !it->second.empty())
                                      ? it->second.front()
                                      : std::string();
      radio->setChecked(member == current);
    }
  }
  emit choice_picks_changed();
}

engine::Collection::ChoicePicks InstallWidget::choice_picks() const {
  return picks_;
}

void InstallWidget::set_prior_choices(
    const engine::Collection::PriorChoiceState &prior) {
  picks_ = engine::Collection::reconcile_prior_choices(choice_groups_, prior, picks_);
  rebuild_choice_page();
  emit choice_picks_changed();
}

engine::Collection::ChoiceValidation InstallWidget::choice_validation() const {
  return engine::Collection::validate_choice_groups(choice_groups_, picks_);
}

// -- completion ----------------------------------------------------------------------

bool InstallWidget::ready_to_finish() const {
  for (int row = 0; row < InstallStepModel::kStepCount; ++row) {
    const auto status = model_->status(static_cast<InstallStep>(row));
    if (status == StepStatus::Running || status == StepStatus::Error) {
      return false;
    }
  }
  if (has_patches() && !patch_consent_.has_value()) {
    return false;
  }
  const auto validation = choice_validation();
  for (const auto &verdict : validation.verdicts) {
    if (verdict.status != engine::Collection::ChoiceStatus::Valid) {
      return false;
    }
  }
  return true;
}

QString InstallWidget::instructions_markdown() const {
  return instructions_source_;
}

}  // namespace ui
