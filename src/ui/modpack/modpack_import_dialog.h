#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <optional>

#include "engine/gmmpack/types.h"

class QLabel;
class QLineEdit;
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

private:
  void on_pick_file();
  void on_url_edited(const QString &text);
  void on_import();

  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;

  // Locate the gmmpack JSON schemas at runtime (installed share dir, or
  // the Workspace input/ dir for dev runs). Empty when not found.
  static std::filesystem::path resolve_schema_dir();

  QPushButton *pick_button_   = nullptr;
  QLabel *picked_file_label_  = nullptr;
  QLineEdit *url_edit_        = nullptr;
  QPushButton *import_button_ = nullptr;

  QString picked_file_;
  std::optional<engine::gmmpack::Gmmpack> pack_;
};

}  // namespace ui
