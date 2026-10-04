#pragma once

#include "engine/mod/filetree/staging_layout.h"
#include "engine/pipeline/pipeline.h"

#include <QDialog>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>

class QAction;
class QLabel;
class QMessageBox;
class QPoint;
class QStandardItemModel;
class QTreeView;

namespace ui {

// MO2's InstallDialog (installermanual) port. The LAST resort of an install:
// ExtractStage only asks when the silent peel and the silent repair both found
// nothing that looks like the game's data directory, so an archive that installs
// fine on its own never opens this.
//
// A tree over the extracted content carries the archive's own name as its top
// row (installdialog.cpp:55-62 seeds a pseudo-root there; archivetree.cpp's
// setup() labels it "<" + dataFolderName + ">", which reads as a placeholder
// rather than as the thing being installed). Right-clicking a directory offers
// "Set as <prefix> directory", and once one is set, "Unset <prefix> directory"
// (:167, :174). The label under the tree is the live verdict against the
// subtree currently designated - green valid, red does not look valid, amber
// cannot check (:100, :109, :117). OK on a red verdict asks "Continue?" with
// CANCEL as the DEFAULT button (:196-207): a mis-click backs out, and
// installing a layout the game does not recognise has to be chosen deliberately.
class LayoutDialog : public QDialog {
  Q_OBJECT
public:
  LayoutDialog(const std::filesystem::path &content_root,
               const std::string &data_prefix, const std::string &archive_name,
               std::shared_ptr<const engine::ModDataCheckerFeature> checker,
               QWidget *parent = nullptr);

  // The subtree the user designated, relative to the content root. Empty means
  // the content root itself, i.e. install as-is.
  std::filesystem::path data_root() const { return data_root_; }

  // What the label is currently showing.
  engine::LayoutVerdict verdict() const { return verdict_; }
  QString verdict_text() const;

  // The "Continue?" prompt an OK on a red verdict puts up, already configured.
  // Public so the default button can be read without driving a modal loop:
  // Cancel being the default is the safety property of this dialog, not a
  // cosmetic detail of a widget. Caller owns the returned box.
  static QMessageBox *make_continue_prompt(QWidget *parent);

protected:
  void accept() override;

private:
  void on_set_data_root();
  void on_unset_data_root();
  void show_menu(const QPoint &pos);
  void refresh_verdict();

  std::filesystem::path content_root_;
  std::filesystem::path data_root_;
  std::shared_ptr<const engine::ModDataCheckerFeature> checker_;
  QString prefix_;
  QStandardItemModel *model_     = nullptr;
  QTreeView *tree_               = nullptr;
  QLabel *verdict_label_         = nullptr;
  QAction *set_action_           = nullptr;
  QAction *unset_action_         = nullptr;
  engine::LayoutVerdict verdict_ = engine::LayoutVerdict::Invalid;
};

// Blocking helper that shows the dialog and returns what the user chose.
// Cancel comes back as LayoutDecision::cancel, which aborts the install. Safe
// to call from any thread: off the main thread it marshals the modal dialog
// onto the main thread and waits (same pattern as ask_overwrite).
engine::LayoutDecision
ask_layout(const std::filesystem::path &content_root, const std::string &data_prefix,
           const std::string &archive_name,
           std::shared_ptr<const engine::ModDataCheckerFeature> checker,
           QWidget *parent = nullptr);

}  // namespace ui
