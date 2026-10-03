#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/core/instance/instance_snapshot.h"
#include "engine/gmmpack/packer.h"

class QCloseEvent;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QLineEdit;
class QTextEdit;
class QTableWidget;
class QLabel;
class QProgressBar;
class QTreeWidget;

namespace ui {

struct PackBuildResult;
class ExportPackThread;

// Modpack export wizard: left sidebar (step list, ~30%) + right
// QStackedWidget content + Cancel/Back/Next navigation.
//
// Steps: Info (pack metadata), Mods (include/category/policy review),
// Executables (include selection), Tree (layout preview), Review
// (summary + output path + Export).
//
// The packer engine (engine::gmmpack) only exports what is in the snapshot it
// is given, so include choices are applied by handing create_gmmpack() a
// filtered snapshot copy. Per-row category, update policy and "bundle this
// manual mod" all travel through PackOptions and end up in the pack.
//
// Building that pack is not a cheap operation: it walks every exported mod's
// folder, parses every INI those mods ship and hashes every bundled byte, so
// it runs on ExportPackThread, never here. The Tree and Review pages are two
// views of ONE build - build_pack() caches it and rebuilds only after an input
// changes (mark_pack_dirty).
class ExportWizard : public QDialog {
  Q_OBJECT
public:
  enum class Step {
    Info,
    Mods,
    Executables,
    Tree,
    Review,
  };

  ExportWizard(const engine::InstanceSnapshot &snapshot, std::filesystem::path mods_dir,
               std::filesystem::path downloads_dir = {},
               std::filesystem::path schema_dir = {}, QWidget *parent = nullptr);
  ~ExportWizard() override;

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  struct StepState {
    Step id;
    QString title;
    bool skipped = false;
    bool visited = false;
  };

  struct ModRow {
    std::string folder;
    QString source;
    bool disabled             = false;
    bool resolvable           = true;
    bool included             = true;
    bool is_vanilla           = false;       // unmanaged game master (Skyrim.esm etc.)
    bool is_manual            = false;       // manual/unknown source - bundled, opt-in
    std::string update_policy = "latest";    // "latest" or "exact"
    std::string category      = "required";  // required | recommended | optional
  };

  struct ExeRow {
    int index = -1;
    QString title;
    QString path;
    QString args;
    bool included = true;
  };

  void build_steps();
  void build_pages();
  void load_foreign_mods();
  void build_mod_rows();
  void build_exe_rows();
  void go_to(int index);
  void on_next();
  void on_back();
  void on_cancel();
  void on_step_clicked(QListWidgetItem *item);
  void refresh_chrome();
  static QString step_title(Step step);

  // Page builders (one per step, in step order).
  QWidget *build_info_page();
  QWidget *build_mods_page();
  QWidget *build_executables_page();
  QWidget *build_tree_page();
  QWidget *build_review_page();

  // Per-page refresh / enter hooks.
  void on_page_entered(int index);
  void refresh_mods_count();
  // Render the Tree and Review pages from the cached build. Safe to call at
  // any time: with no build ready it shows the waiting state instead.
  void refresh_tree();
  void refresh_review();
  [[nodiscard]] engine::gmmpack::PackOptions pack_options() const;
  [[nodiscard]] engine::InstanceSnapshot filtered_snapshot() const;
  [[nodiscard]] int separator_count() const;

  // Pack building. build_pack() queues a run on pack_thread_ unless one is
  // already in flight or the cached build still matches the current choices;
  // on_pack_built() stores it and repaints. mark_pack_dirty() invalidates the
  // cache and is wired to every control that feeds the pack.
  ExportPackThread *ensure_pack_thread();
  void build_pack();
  void set_pack_busy(bool busy);
  void mark_pack_dirty();

private slots:
  void on_exclude_disabled();
  void on_mod_include_toggled();
  void on_policy_changed();
  void on_category_changed();
  void on_exe_include_toggled();
  void on_browse_output();
  void on_export();
  void on_pack_input_changed();
  void on_pack_built(PackBuildResult result);

private:
  engine::InstanceSnapshot snapshot_;
  std::filesystem::path mods_dir_;
  std::filesystem::path downloads_dir_;
  std::filesystem::path schema_dir_;
  std::unordered_set<std::string> foreign_mods_;  // unmanaged vanilla masters
  std::vector<StepState> steps_;
  int current_ = 0;
  std::vector<ModRow> mods_;
  std::vector<ExeRow> exes_;

  // Cached pack build, shared by the Tree and Review pages. pack_dirty_ means
  // the cache no longer matches the choices; pack_building_ means a run is in
  // flight; exporting_ means that run is writing the archive (see set_pack_busy).
  ExportPackThread *pack_thread_ = nullptr;
  engine::gmmpack::Gmmpack pack_;
  QString pack_error_;
  bool pack_dirty_    = true;
  bool pack_ready_    = false;
  bool pack_building_ = false;
  bool exporting_     = false;

  QListWidget *sidebar_     = nullptr;
  QStackedWidget *stack_    = nullptr;
  QPushButton *back_button_ = nullptr;
  QPushButton *next_button_ = nullptr;

  // Step 1: pack metadata.
  QLineEdit *name_edit_         = nullptr;
  QLineEdit *author_edit_       = nullptr;
  QTextEdit *desc_edit_         = nullptr;
  QLineEdit *homepage_edit_     = nullptr;
  QTextEdit *instructions_edit_ = nullptr;

  // Step 2: mod review.
  QTableWidget *mods_table_ = nullptr;
  QLabel *mods_count_       = nullptr;

  // Step 3: executable selection.
  QTableWidget *exes_table_ = nullptr;

  // Step 4: tree preview.
  QTreeWidget *tree_    = nullptr;
  QLabel *tree_summary_ = nullptr;

  // Step 5: review + export.
  QLabel *review_summary_ = nullptr;
  QLineEdit *output_edit_ = nullptr;
  QLabel *status_label_   = nullptr;
  QProgressBar *progress_ = nullptr;
};

}  // namespace ui
