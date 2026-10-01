#include "ui/modinfo/source_panels/git_source_panel.h"

#include "engine/mod/meta/mod_meta.h"
#include "engine/source/git/git_info.h"
#include "ui/modinfo/git_ops.h"
#include "ui/theme/icon_manager.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

namespace {

  // The "you are about to lose work" notice shown when a mod is managed from
  // git while also having come from a download source. Colours come from the
  // palette, never hardcoded: QPalette has no warning role, so ToolTipBase /
  // ToolTipText are the closest standard roles (they are the platform's own
  // "this needs attention" surface) and the dim yellow the notice asks for
  // is ToolTipBase blended toward the window colour. A theme that makes
  // ToolTipBase a strong colour still gets a readable notice because the
  // text role is applied to it unchanged.
  void apply_banner_palette(QLabel *label) {
    const QPalette pal = QApplication::palette();
    const QColor base  = pal.color(QPalette::ToolTipBase);
    const QColor dim   = base.lighter(102);
    label->setAutoFillBackground(true);
    QPalette own = pal;
    own.setColor(QPalette::Window, dim);
    own.setColor(QPalette::WindowText, pal.color(QPalette::ToolTipText));
    label->setPalette(own);
    // 1px border in the undimmed tooltip colour, so the box reads as a box
    // on any theme.
    label->setStyleSheet(
        QStringLiteral("QLabel { border: 1px solid %1; padding: 6px; }")
            .arg(base.name()));
  }

}  // namespace

GitSourcePanel::GitSourcePanel(const ModInfoData &data, QWidget *parent)
    : SourceInfoPanel(data, parent) {
  auto *layout = new QVBoxLayout(this);

  banner_ = new QLabel(tr("This mod was downloaded from a different source, be "
                          "careful about managing this mod from the upstream "
                          "repo"),
                       this);
  banner_->setWordWrap(true);
  banner_->setVisible(false);
  apply_banner_palette(banner_);
  layout->addWidget(banner_);

  auto *form = new QFormLayout();

  remote_ = new QLineEdit(this);
  remote_->setReadOnly(true);
  remote_->setToolTip(tr("The git remote this mod's files come from. Read from "
                         "the repository, not editable here."));
  form->addRow(tr("Remote:"), remote_);

  branch_ = new QLineEdit(this);
  branch_->setReadOnly(true);
  form->addRow(tr("Branch:"), branch_);

  commit_ = new QLineEdit(this);
  commit_->setReadOnly(true);
  form->addRow(tr("Commit:"), commit_);

  upstream_state_ = new QLabel(this);
  upstream_state_->setWordWrap(true);
  form->addRow(tr("Upstream:"), upstream_state_);

  branches_ = new QComboBox(this);
  branches_->setEditable(false);
  branches_->setToolTip(tr("Upstream branches git knows about locally. "
                           "Choosing one checks it out in this mod folder."));
  form->addRow(tr("Upstream branches:"), branches_);

  layout->addLayout(form);

  auto *buttons = new QHBoxLayout();
  check_        = new QPushButton(tr("Check for updates"), this);
  pull_         = new QPushButton(tr("Pull"), this);
  reset_        = new QPushButton(tr("Reset to upstream..."), this);
  buttons->addWidget(check_);
  buttons->addWidget(pull_);
  buttons->addWidget(reset_);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  git_missing_ = new QLabel(this);
  git_missing_->setWordWrap(true);
  layout->addWidget(git_missing_);

  connect(check_, &QPushButton::clicked, this, &GitSourcePanel::on_check_updates);
  connect(pull_, &QPushButton::clicked, this, &GitSourcePanel::on_pull);
  connect(reset_, &QPushButton::clicked, this, &GitSourcePanel::on_reset_to_upstream);
  connect(branches_, &QComboBox::currentTextChanged, this,
          [this](const QString &branch) {
            if (loading_ || branch.isEmpty())
              return;
            const auto dir = repo_path();
            if (dir.empty())
              return;
            // A branch NAME is a git ref, not a shell string: QProcess passes argv
            // directly, so nothing is expanded or interpreted.
            const gitops::Result r = gitops::run(
                dir, {QStringLiteral("checkout"), QStringLiteral("--quiet"), branch});
            if (!r.ok) {
              QMessageBox::warning(
                  this, tr("Git"),
                  tr("Could not switch to %1.\n\n%2").arg(branch, r.summary()));
              populate();
              return;
            }
            persist_checkout();
            refresh_status();
          });

  populate();
}

QString GitSourcePanel::icon_key_for(const QString &remote_url) {
  return QString::fromStdString(
      engine::git_icon_key(engine::Git::host_of(remote_url.toStdString())));
}

std::filesystem::path GitSourcePanel::repo_path() const {
  // A default-constructed QDir reports "." - never the process's working
  // directory, which is not this mod's folder. git must never be pointed at
  // anything the mod does not own.
  const QString path = data_.mod_dir.path();
  if (path.isEmpty() || path == QLatin1String("."))
    return {};
  return std::filesystem::path(path.toStdString());
}

void GitSourcePanel::populate() {
  loading_       = true;
  const auto dir = repo_path();

  // The sidecar is the manager's record; the repository is the truth. A .git
  // on disk wins over a stale (or absent) sidecar, and a recorded remote is
  // kept when the repo has none configured so the icon does not flip.
  const QString meta_remote = meta_value("Git", "remote_url");
  QString remote            = meta_remote;
  if (!dir.empty() && engine::Git::is_repository(dir)) {
    const std::string live = engine::Git::remote_url(dir);
    if (!live.empty())
      remote = QString::fromStdString(live);
  }
  remote_->setText(remote.isEmpty() ? tr("(no remote configured)") : remote);
  branch_->setText(meta_value("Git", "branch"));
  commit_->setText(meta_value("Git", "commit"));
  update_coexistence_banner();
  loading_ = false;

  const bool have_repo = !dir.empty() && engine::Git::is_repository(dir);
  const bool have_git  = gitops::available();
  git_missing_->setVisible(!have_repo || !have_git);
  if (!have_git) {
    git_missing_->setText(
        tr("git was not found on PATH, so this mod's repository cannot be "
           "checked or updated from here."));
    check_->setEnabled(false);
    pull_->setEnabled(false);
    reset_->setEnabled(false);
    upstream_state_->clear();
    branches_->clear();
    return;
  }
  if (!have_repo) {
    git_missing_->setText(
        tr("This mod's folder holds no .git, so there is no repository to "
           "check or update. A recorded upstream URL is kept for reference."));
    check_->setEnabled(false);
    pull_->setEnabled(false);
    reset_->setEnabled(false);
    upstream_state_->clear();
    branches_->clear();
    return;
  }

  // Only the reset is refused outside the mod's own repository root, and that
  // is stated up front rather than discovered on click.
  const bool scoped = gitops::is_scoped_repository(dir);
  reset_->setEnabled(scoped);
  if (!scoped)
    reset_->setToolTip(tr("Disabled: this mod's folder is not the root of its "
                          "own repository, so a reset could reach files "
                          "outside the mod."));
  refresh_status();
  refresh_branches();
}

void GitSourcePanel::save_state() {
  persist_checkout();
}

bool GitSourcePanel::has_data() const {
  // A git working copy is real source data even with no remote configured, so
  // the Source tab's red-dot follows is_git rather than the URL.
  return data_.is_git;
}

void GitSourcePanel::refresh_status() {
  const auto dir = repo_path();
  if (dir.empty())
    return;
  const gitops::Status s = gitops::status(dir);
  if (!s.ok) {
    upstream_state_->setText(s.error.isEmpty() ? tr("Could not read the "
                                                    "repository state.")
                                               : s.error);
    pull_->setEnabled(false);
    return;
  }
  branch_->setText(s.branch);
  commit_->setText(s.commit);
  if (s.remote.isEmpty() && remote_->text() == tr("(no remote configured)"))
    remote_->setText(tr("(no remote configured)"));

  if (!s.has_upstream) {
    upstream_state_->setText(tr("This branch has no upstream, so there is "
                                "nothing to compare against."));
    pull_->setEnabled(false);
    return;
  }
  if (s.behind == 0 && s.ahead == 0) {
    upstream_state_->setText(tr("Up to date."));
  } else if (s.behind > 0 && s.ahead == 0) {
    upstream_state_->setText(tr("%n commit(s) behind upstream - Pull will "
                                "fast-forward this mod to it.",
                                nullptr, s.behind));
  } else if (s.ahead > 0 && s.behind == 0) {
    upstream_state_->setText(
        tr("%n local commit(s) not in upstream.", nullptr, s.ahead));
  } else {
    upstream_state_->setText(
        tr("Diverged: %1 local commit(s) and %2 upstream commit(s). A pull "
           "will not merge them; use Reset to upstream to match upstream "
           "exactly.")
            .arg(s.ahead)
            .arg(s.behind));
  }
  // Pull only makes sense when upstream has something we do not. Diverged is
  // still enabled so the user SEES git's refusal - --ff-only fails loudly
  // rather than merging behind their back.
  pull_->setEnabled(s.behind > 0);
}

void GitSourcePanel::refresh_branches() {
  const auto dir        = repo_path();
  loading_              = true;
  const QString current = branches_->currentText();
  branches_->clear();
  if (!dir.empty())
    branches_->addItems(gitops::upstream_branches(dir));
  const int idx = branches_->findText(current);
  if (idx >= 0)
    branches_->setCurrentIndex(idx);
  loading_ = false;
}

void GitSourcePanel::persist_checkout() {
  if (!data_.load_meta || !data_.save_meta)
    return;
  auto meta            = data_.load_meta();
  const QString commit = commit_->text().trimmed();
  const QString branch = branch_->text().trimmed();
  if (meta.get("Git", "commit") == commit.toStdString() &&
      meta.get("Git", "branch") == branch.toStdString())
    return;
  meta.set_git(meta.git_remote_url(), commit.toStdString(), branch.toStdString());
  data_.save_meta(meta);
}

void GitSourcePanel::on_check_updates() {
  const auto dir = repo_path();
  if (dir.empty())
    return;
  check_->setEnabled(false);
  const gitops::Result r = gitops::fetch(dir);
  check_->setEnabled(true);
  if (!r.ok) {
    // git's own stderr, not a guess about what went wrong.
    QMessageBox::warning(
        this, tr("Git"),
        tr("Could not reach the upstream repository.\n\n%1").arg(r.summary()));
    return;
  }
  refresh_status();
  refresh_branches();
}

void GitSourcePanel::on_pull() {
  const auto dir = repo_path();
  if (dir.empty())
    return;
  const gitops::Status s = gitops::status(dir);
  if (s.ok && s.behind == 0 && s.ahead == 0) {
    QMessageBox::information(this, tr("Git"), tr("Already up to date."));
    return;
  }
  if (QMessageBox::question(
          this, tr("Pull"),
          tr("Fast-forwarding this mod to its upstream branch. Local files "
             "that upstream also changed are replaced.\n\nContinue?"),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    return;

  pull_->setEnabled(false);
  const gitops::Result r = gitops::pull_ff(dir);
  pull_->setEnabled(true);
  refresh_status();
  refresh_branches();
  if (r.ok) {
    QMessageBox::information(this, tr("Git"),
                             tr("Fast-forwarded this mod to upstream."));
    persist_checkout();
    return;
  }
  // A refused fast-forward (diverged branch) is a real outcome, not a bug -
  // say what git said and what the other option is.
  QMessageBox::warning(
      this, tr("Git"),
      tr("Pull did not fast-forward this mod.\n\n%1\n\nNothing was merged. The "
         "mod's files are unchanged.")
          .arg(r.summary()));
}

void GitSourcePanel::on_reset_to_upstream() {
  const auto dir = repo_path();
  if (dir.empty())
    return;

  // Everything the discard takes with it, counted BEFORE anything runs.
  const int changes       = gitops::discardable_change_count(dir);
  const gitops::Status s  = gitops::status(dir);
  const QString ahead_txt = s.ok && s.ahead > 0
                                ? tr("\n\n%1 local commit(s) in this mod will be "
                                     "discarded too.")
                                      .arg(s.ahead)
                                : QString();

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("Reset this mod to upstream"));
  box.setText(tr("This DELETES local changes in this mod's folder."));
  box.setInformativeText(
      changes > 0 ? tr("%1 modified, deleted or untracked file(s) in this mod will be "
                       "discarded permanently.%2\n\nThe mod folder will match upstream "
                       "exactly. This cannot be undone.")
                        .arg(changes)
                        .arg(ahead_txt)
                  : tr("The mod folder will be reset to match upstream exactly, and "
                       "any untracked file in it deleted.%2\n\nThis cannot be "
                       "undone.")
                        .arg(ahead_txt));
  auto *yes    = box.addButton(tr("Reset to upstream"), QMessageBox::AcceptRole);
  auto *cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  // Focus lands on Cancel: the destructive path must be the deliberate one.
  box.setDefaultButton(cancel);
  box.exec();
  if (box.clickedButton() != yes)
    return;

  reset_->setEnabled(false);
  const gitops::Result r = gitops::reset_to_upstream(dir);
  reset_->setEnabled(true);
  refresh_status();
  refresh_branches();
  if (r.ok) {
    QMessageBox::information(this, tr("Git"), tr("This mod now matches upstream."));
    persist_checkout();
    return;
  }
  QMessageBox::warning(this, tr("Git"),
                       tr("The reset did not complete.\n\n%1").arg(r.summary()));
}

void GitSourcePanel::update_coexistence_banner() {
  // Only when the mod ALSO has a download source. A git-only mod has nothing
  // to disagree with, and a warning there would be noise.
  const QString t                = data_.source_type.toLower();
  const bool has_download_source = !t.isEmpty() && t != QLatin1String("git") &&
                                   t != QLatin1String("manual") &&
                                   t != QLatin1String("direct");
  banner_->setVisible(has_download_source);
  if (has_download_source)
    banner_->setToolTip(tr("This mod was installed from %1, and its files are "
                           "now tracked by a git repository. Changes made "
                           "through one are not visible to the other.")
                            .arg(t));
}

}  // namespace ui