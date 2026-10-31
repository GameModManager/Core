#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QVector>

#include <filesystem>
#include <string>
#include <vector>

#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "ui/game_selection/game_card.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QLineEdit;
class QLabel;
class QComboBox;
class QCheckBox;
#include "engine/plugin_host/plugin_loader.h"

namespace ui {

// Guided instance creation wizard (Workspace-ffei). QDialog + QStackedWidget
// following the ModpackInstallWizard chrome. Pages: Game, Variants, Name,
// Profile, Paths, Confirmation.
class InstanceCreationWizard : public QDialog {
  Q_OBJECT
public:
  explicit InstanceCreationWizard(engine::PluginLoader *loader,
                                  QWidget *parent              = nullptr,
                                  const GameEntry *preselected = nullptr);
  ~InstanceCreationWizard() override = default;

  // Name of the instance created on accept (empty on cancel/failure).
  [[nodiscard]] QString created_instance_name() const { return created_name_; }

  // Collected options (valid after accept()).
  [[nodiscard]] const engine::InstanceCreationOptions &options() const {
    return options_;
  }
  [[nodiscard]] const GameEntry &chosen_game() const { return chosen_game_; }

private:
  struct StepState {
    int id;
    QString title;
    bool skipped = false;
    bool visited = false;
  };

  enum PageId {
    PageGame = 0,
    PageVariants,
    PageName,
    PageProfile,
    PagePaths,
    PageConfirm,
  };

  void build_steps();
  void build_pages();
  void go_to(int index);
  void on_next();
  void on_back();
  void on_cancel();
  void refresh_chrome();
  void refresh_next_enabled();

  QWidget *build_game_page();
  QWidget *build_variants_page();
  QWidget *build_name_page();
  QWidget *build_profile_page();
  QWidget *build_paths_page();
  QWidget *build_confirm_page();

  void on_page_entered(int index);
  bool name_valid() const;
  bool game_dir_valid() const;

  engine::PluginLoader *loader_ = nullptr;
  std::vector<GameEntry> installed_entries_;
  std::vector<GameEntry> available_entries_;

  GameEntry chosen_game_;
  bool chosen_ok_ = false;

  QLineEdit *name_edit_                  = nullptr;
  QLabel *name_error_                    = nullptr;
  QComboBox *variant_combo_              = nullptr;
  QCheckBox *archive_invalidation_check_ = nullptr;
  QCheckBox *local_settings_check_       = nullptr;
  QCheckBox *local_saves_check_          = nullptr;

  // Paths page
  QLineEdit *game_dir_edit_      = nullptr;
  QLabel *game_dir_error_        = nullptr;
  QLineEdit *mods_dir_edit_      = nullptr;
  QLineEdit *downloads_dir_edit_ = nullptr;
  QLineEdit *cache_dir_edit_     = nullptr;
  QLineEdit *profiles_dir_edit_  = nullptr;
  QLineEdit *overwrite_dir_edit_ = nullptr;

  QList<StepState> steps_;
  int current_              = 0;
  QListWidget *sidebar_     = nullptr;
  QStackedWidget *stack_    = nullptr;
  QPushButton *back_button_ = nullptr;
  QPushButton *next_button_ = nullptr;

  engine::InstanceCreationOptions options_;
  QString created_name_;
};

}  // namespace ui