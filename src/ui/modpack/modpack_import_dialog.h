#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/modpack/collection/nexus/adapter.h"
#include "engine/gmmpack/types.h"
#include "ui/widgets/line_edit_clear.h"

class QLabel;
class QPushButton;

namespace ui {

// Import entry point for modpacks (File > Import Modpack...).
//
// Two stacked cards: pick a .gmmpack file from disk, or paste a collection
// URL. The Import button validates the input, runs pack source detection,
// then either unpacks a .gmmpack archive or fetches a Nexus collection
// through the Nexus adapter (converted to Gmmpack for the install wizard).
// Accepts .gmmpack/.zip drops directly onto the dialog.
class ModpackImportDialog : public QDialog {
  Q_OBJECT
public:
  explicit ModpackImportDialog(QWidget *parent = nullptr);

  [[nodiscard]] bool has_pack() const { return pack_.has_value(); }
  [[nodiscard]] const engine::gmmpack::Gmmpack &pack() const { return *pack_; }

  // Pre-select a file (used by the main-window drop handler). Ignored
  // when empty; clears any URL text so the file wins.
  void set_picked_file(const QString &path);

  // Pre-fill the collection URL (used by the nxm:// collection link path).
  // Ignored when empty; clears any selected file so the URL wins.
  void set_collection_url(const QString &url);

private:
  void on_pick_file();
  void on_url_edited(const QString &text);
  void on_import();

  // Say what the collection did not give us: a revision Nexus has since
  // retracted or discarded, every mod that was skipped with the reason it was
  // skipped, and everything the collection declared that could not be read -
  // including the collection archive itself when it could not be fetched.
  // Silent when there is nothing to say.
  void report_collection_gaps(
      const std::string &revision_status,
      const std::vector<engine::Collection::Nexus::SkipDiagnostic> &skipped,
      const std::vector<engine::Collection::Unresolved> &unresolved);

  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;

  // Locate the gmmpack JSON schemas at runtime (installed share dir, or
  // the Workspace input/ dir for dev runs). Empty when not found.
  static std::filesystem::path resolve_schema_dir();

  QPushButton *pick_button_   = nullptr;
  QLabel *picked_file_label_  = nullptr;
  LineEditClear *url_edit_    = nullptr;
  QPushButton *import_button_ = nullptr;

  QString picked_file_;
  std::optional<engine::gmmpack::Gmmpack> pack_;
};

}  // namespace ui
