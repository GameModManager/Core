#pragma once

// Two-pane modpack install widget (Workspace-iiao).
//
// Left pane: ordered step list (custom QAbstractListModel) with live status
// plus a per-step detail panel below it. Right pane: the pack's
// instructions.md rendered statically for the whole install. Bottom: a
// persistent diagnostics count badge that expands to the full list.
//
// The widget is a view + state machine only: it never downloads, installs,
// or runs anything itself. Engine work is supplied from outside via
// set_step_status(), while pack content comes in through set_pack() and the
// incremental path through set_update_plan(). It reuses the finished engine
// modules instead of reimplementing them:
//   - choice groups: Collection::validate_choice_groups() for the radio step
//     and Collection::reconcile_prior_choices() for carrying picks across
//     updates (engine/collection/choice_groups.h),
//   - INI tweaks: gmmpack::to_edit_file() + modpack::merge_ini_edits() for the
//     per-tweak list and its overridden badges,
//   - incremental updates: modpack::UpdatePlan from diff_update(), which
//     decides which steps run and which are skipped.

#include <QAbstractListModel>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/collection/choice_groups.h"
#include "engine/collection/manifest.h"
#include "engine/gmmpack/types.h"
#include "engine/modpack/incremental_update.h"

class QButtonGroup;
class QCheckBox;
class QGroupBox;
class QLabel;
class QListView;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QVBoxLayout;

namespace ui
{

// Fixed install pipeline, in run order. Choice groups come after LOOT and
// Done is terminal; both match the ticket's step list.
enum class InstallStep
{
  Resolve = 0,
  Download,
  PatchConsent,
  Install,
  IniEdits,
  BuildTree,
  SetupExes,
  Loot,
  ChoiceGroups,
  Done,
};

enum class StepStatus
{
  Pending,
  Running,
  Done,
  Error,
  Skipped,  // incremental update: UpdatePlan says this step has no work
};

struct StepDiagnostic
{
  InstallStep step;
  QString message;
};

// Model behind the step list. DisplayRole renders "glyph title (N)" where N
// is the step's inline diagnostic count; ToolTipRole carries the messages.
class InstallStepModel : public QAbstractListModel
{
  Q_OBJECT
public:
  static constexpr int kStepCount = 10;
  static constexpr int StatusRole = Qt::UserRole + 1;
  static constexpr int StepRole   = Qt::UserRole + 2;

  explicit InstallStepModel(QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index,
                              int role = Qt::DisplayRole) const override;

  void set_status(InstallStep step, StepStatus status);
  [[nodiscard]] StepStatus status(InstallStep step) const;
  void set_diagnostic_count(InstallStep step, int count);
  void set_diagnostic_preview(InstallStep step, const QString& preview);

  [[nodiscard]] static QString title(InstallStep step);

private:
  [[nodiscard]] static int row_of(InstallStep step);
  void notify_row(int row);

  StepStatus statuses_[kStepCount];
  int diag_counts_[kStepCount];
  QString diag_previews_[kStepCount];
};

class InstallWidget : public QWidget
{
  Q_OBJECT
public:
  explicit InstallWidget(QWidget* parent = nullptr);

  // -- pack content -----------------------------------------------------
  // Fills patch-consent mods, INI tweak panels, choice radios, setup-exe
  // and LOOT summaries, and the instructions pane from an unpacked pack.
  // Resets all step statuses to Pending and clears diagnostics.
  void set_pack(const engine::gmmpack::Gmmpack& pack);
  // Fresh install: every step runs.
  void set_fresh_install();
  // Incremental update: only steps with work in the plan run, the rest show
  // Skipped. Plan diagnostics attach to steps by path prefix (mods/ ->
  // Resolve, patches/ -> PatchConsent, ini/ -> IniEdits, tree -> BuildTree,
  // executables/ -> SetupExes, anything else -> Loot).
  void set_update_plan(const engine::modpack::UpdatePlan& plan);

  // -- step state (driven by the installer outside this widget) ---------
  void set_step_status(InstallStep step, StepStatus status);
  [[nodiscard]] StepStatus step_status(InstallStep step) const;
  [[nodiscard]] static QString step_title(InstallStep step);

  // -- diagnostics -------------------------------------------------------
  void add_diagnostic(InstallStep step, const QString& message);
  void clear_diagnostics();
  [[nodiscard]] const std::vector<StepDiagnostic>& diagnostics() const;
  [[nodiscard]] int diagnostic_count() const;

  // -- patch consent -----------------------------------------------------
  [[nodiscard]] bool has_patches() const;
  [[nodiscard]] std::optional<bool> patch_consent() const;
  void set_patch_consent(bool allowed);

  // -- INI tweaks ----------------------------------------------------------
  [[nodiscard]] std::vector<std::string> ini_tweak_ids() const;
  [[nodiscard]] bool ini_tweak_enabled(const std::string& tweak_id) const;
  // Required tweaks are locked: disabling one is ignored.
  void set_ini_tweak_enabled(const std::string& tweak_id, bool enabled);

  // -- choice groups -------------------------------------------------------
  void set_choice_picks(const engine::Collection::ChoicePicks& picks);
  [[nodiscard]] engine::Collection::ChoicePicks choice_picks() const;
  // Carry remembered picks forward across a pack revision; changed/removed
  // groups drop to unpicked so the UI re-prompts instead of installing stale
  // choices. Reuses reconcile_prior_choices().
  void set_prior_choices(const engine::Collection::PriorChoiceState& prior);
  [[nodiscard]] engine::Collection::ChoiceValidation choice_validation() const;

  // -- completion ------------------------------------------------------------
  // True when no enabled step is Running/Error, patch consent is decided
  // (when the pack has patches), and every choice group validates.
  [[nodiscard]] bool ready_to_finish() const;
  [[nodiscard]] QString instructions_markdown() const;

signals:
  void step_status_changed(InstallStep step);
  void diagnostics_changed();
  void patch_consent_changed(bool allowed);
  void ini_tweak_toggled(const QString& tweak_id, bool enabled);
  void choice_picks_changed();

private slots:
  void on_step_selected(const QModelIndex& current);
  void on_allow_patches();
  void on_deny_patches();
  void on_diagnostics_toggled(bool expanded);

private:
  void build_ui();
  void build_step_page(InstallStep step);
  void rebuild_patch_page();
  void rebuild_choice_page();
  void refresh_badge();
  void refresh_model_for(InstallStep step);
  [[nodiscard]] engine::Collection::ChoicePicks current_picks_from_ui() const;
  void sync_picks_from_ui();

  InstallStepModel* model_    = nullptr;
  QListView* step_list_       = nullptr;
  QStackedWidget* detail_     = nullptr;
  QTextBrowser* instructions_ = nullptr;
  QToolButton* diag_badge_    = nullptr;
  QListWidget* diag_list_     = nullptr;

  // Per-step detail pages (indexed by step row).
  QWidget* step_pages_[InstallStepModel::kStepCount]          = {};
  QLabel* step_status_labels_[InstallStepModel::kStepCount]   = {};
  QListWidget* step_diag_lists_[InstallStepModel::kStepCount] = {};

  // Patch consent page.
  QListWidget* patch_list_   = nullptr;
  QPushButton* allow_button_ = nullptr;
  QPushButton* deny_button_  = nullptr;
  std::vector<std::string> patch_mods_;
  std::optional<bool> patch_consent_;

  // INI page: tweak id -> (checkbox, required). Grouped by target file.
  struct IniRow
  {
    QCheckBox* box = nullptr;
    bool required  = false;
  };
  QWidget* ini_body_ = nullptr;
  std::unordered_map<std::string, IniRow> ini_rows_;
  std::vector<std::string> ini_order_;

  // Choice page: group id -> radio group; rebuilt from groups_.
  QWidget* choice_body_ = nullptr;
  std::vector<QButtonGroup*> choice_button_groups_;
  std::vector<engine::Collection::ChoiceGroup> choice_groups_;
  engine::Collection::ChoicePicks picks_;

  QString instructions_source_;
  std::vector<StepDiagnostic> diagnostics_;
};

}  // namespace ui
