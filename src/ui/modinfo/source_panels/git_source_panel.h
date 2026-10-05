#pragma once

#include "ui/modinfo/source_panels/source_info_panel.h"

#include <QString>

#include <filesystem>

class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;

namespace ui {

class DescriptionRenderer;

// The Git sub-panel of the mod-info Sources tab. Shown for a mod whose folder
// is a git working copy, IN ADDITION to whatever download source it also has
// (Nexus + Git coexist) - never instead of one.
//
// Everything git-specific about the mod is here: the upstream URL (read-only
// - it comes from the repository, not from a field the user types), the
// current branch and commit, upstream branch listing, the README as the
// mod's description, and the three actions - check for updates, pull, and the
// destructive clean reset. The platform (GitHub / GitLab / ...) never appears:
// the source_type is "git" and the host only chooses the tab's icon.
class GitSourcePanel : public SourceInfoPanel {
  Q_OBJECT
public:
  explicit GitSourcePanel(const ModInfoData &data, QWidget *parent = nullptr);

  void populate() override;
  void save_state() override;
  [[nodiscard]] bool has_data() const override;

  // Icon key for this mod's Git tab: the branded github badge when the remote
  // host is github.com, the generic git badge for every other host (and for
  // a repo whose remote is unknown). Static + argument-taking so the tab
  // builder can ask the same question before the panel exists.
  [[nodiscard]] static QString icon_key_for(const QString &remote_url);

private:
  // Absolute path of the mod folder, or empty when the tab has no usable
  // data.mod_dir. Every git command is scoped to this and nowhere else.
  [[nodiscard]] std::filesystem::path repo_path() const;

  void refresh_status();
  void refresh_branches();
  void persist_checkout();

  // Reads README.md from the mod root and renders it through the same
  // description renderer the download-source panels use. No README, no
  // description - readme_missing_ says so rather than one being invented.
  void render_readme();

  void on_check_updates();
  void on_pull();
  void on_reset_to_upstream();

  // Rebuilds the coexisting-source warning. Shown only when this mod ALSO
  // has a download source, which is exactly the case where the two can
  // disagree about what the mod's files should be.
  void update_coexistence_banner();

  QLabel *banner_         = nullptr;
  QLineEdit *remote_      = nullptr;
  QLineEdit *branch_      = nullptr;
  QLineEdit *commit_      = nullptr;
  QLabel *upstream_state_ = nullptr;
  QComboBox *branches_    = nullptr;
  QPushButton *check_     = nullptr;
  QPushButton *pull_      = nullptr;
  QPushButton *reset_     = nullptr;
  QLabel *git_missing_    = nullptr;
  QLabel *readme_missing_ = nullptr;
  // Built on the first README this panel renders, so a repository without one
  // never pays for a web engine view. Null means no README was seen yet.
  DescriptionRenderer *description_ = nullptr;
};

}  // namespace ui
