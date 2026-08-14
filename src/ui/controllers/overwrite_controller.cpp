#include "ui/controllers/overwrite_controller.h"
#include "ui/controllers/mod_list_controller.h"

#include <QDesktopServices>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QUrl>

#include "engine/core/log/logger.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/overwrite/overwrite_utils.h"
#include "engine/game/registry/game_knowledge.h"
#include "ui/main_window/main_window.h"
#include "ui/overwrite/move_to_mod_dialog.h"
#include "ui/overwrite/overwrite_info_dialog.h"
#include "ui/overwrite/sync_overwrite_dialog.h"
#include "ui/widgets/mod_list_model.h"

namespace ui {

OverwriteController::OverwriteController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {}

void OverwriteController::clear_overwrite() {
  auto reply = QMessageBox::question(
      w_, tr("Clear Overwrite"),
      tr("Remove all files from the Overwrite folder? Deleted files go to the "
         "system trash."),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply != QMessageBox::Yes)
    return;

  if (w_->current_instance_root_.empty())
    return;
  auto overwrite_dir = w_->overwrite_dir_path();
  auto mods_subpath = w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                           "mods_subpath", "")
                                     : std::string();
  auto cleared = engine::clear_overwrite(overwrite_dir, mods_subpath);
  if (cleared > 0) {
    engine::Logger::instance().debug("Overwrite cleared (" +
                                     std::to_string(cleared) + " file(s))");
    QMessageBox::information(w_, tr("Overwrite"),
                             tr("Overwrite folder cleared."));
  } else {
    QMessageBox::warning(w_, tr("Overwrite"),
                         tr("Failed to clear Overwrite folder."));
  }
}

void OverwriteController::create_mod_from_overwrite() {
  if (w_->current_instance_root_.empty())
    return;
  auto overwrite_dir = w_->overwrite_dir_path();
  auto mods_subpath = w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                           "mods_subpath", "")
                                     : std::string();
  if (mods_subpath.empty())
    return;

  if (engine::overwrite_is_empty(overwrite_dir)) {
    QMessageBox::information(w_, tr("Create Mod"),
                             tr("Overwrite folder is empty."));
    return;
  }

  bool ok;
  auto name =
      QInputDialog::getText(w_, tr("Create Mod from Overwrite"),
                            tr("Mod name:"), QLineEdit::Normal, QString(), &ok);
  if (!ok || name.isEmpty())
    return;

  auto mod_dir = w_->mods_dir_path() / name.toStdString();
  auto moved =
      engine::move_overwrite_to_mod(overwrite_dir, mod_dir, mods_subpath);
  if (moved) {
    // Write the game's metadata file so ModScanner picks the mod up.
    auto metadata_file =
        w_->knowledge_->get(w_->current_game_id_, "metadata_file", "meta.ini");
    engine::ModMeta::write_game_metadata(mod_dir, metadata_file,
                                         name.toStdString(), "1.0", "0");
    auto id = name;
    w_->mod_model_->add_mod(id, name, "");
    engine::Logger::instance().debug("Promote Overwrite to mod: " +
                                     name.toStdString());
    QMessageBox::information(
        w_, tr("Create Mod"),
        tr("Overwrite contents promoted to mod: %1").arg(name));
  } else {
    QMessageBox::warning(w_, tr("Create Mod"),
                         tr("Failed to promote Overwrite files."));
  }
}

void OverwriteController::move_overwrite_content_to_mod() {
  if (w_->current_instance_root_.empty())
    return;
  auto overwrite_dir = w_->overwrite_dir_path();
  auto mods_subpath = w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                           "mods_subpath", "")
                                     : std::string();
  if (mods_subpath.empty())
    return;
  if (engine::overwrite_is_empty(overwrite_dir)) {
    QMessageBox::information(w_, tr("Move content"),
                             tr("Overwrite folder is empty."));
    return;
  }

  // MO2 moveOverwriteContentToExistingMod: picker excludes separators /
  // foreign (game-native) / Overwrite / merged mods.
  std::vector<std::pair<std::string, std::string>> mods;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native)
      continue;
    mods.emplace_back(m.id.toStdString(), m.name.toStdString());
  }
  if (mods.empty()) {
    QMessageBox::information(w_, tr("Move content"), tr("No mods available."));
    return;
  }

  MoveToModDialog dialog(mods, w_);
  if (dialog.exec() != QDialog::Accepted)
    return;
  auto folder = dialog.selected_folder();
  if (folder.empty())
    return;

  auto mod_dir = w_->mods_dir_path() / folder;
  auto moved =
      engine::move_overwrite_to_mod(overwrite_dir, mod_dir, mods_subpath);
  if (moved) {
    engine::Logger::instance().debug("Moved Overwrite contents to mod: " +
                                     folder);
    QMessageBox::information(w_, tr("Move content"),
                             tr("Overwrite contents moved to mod: %1")
                                 .arg(QString::fromStdString(folder)));
  } else {
    QMessageBox::warning(w_, tr("Move content"),
                         tr("Failed to move Overwrite files."));
  }
}

void OverwriteController::sync_overwrite_to_mods() {
  if (w_->current_instance_root_.empty() || !w_->knowledge_)
    return;
  auto overwrite_dir = w_->overwrite_dir_path();
  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  if (mods_subpath.empty())
    return;
  if (engine::overwrite_is_empty(overwrite_dir)) {
    QMessageBox::information(w_, tr("Sync to Mods"),
                             tr("Overwrite folder is empty."));
    return;
  }

  const bool conflict_reversed =
      w_->knowledge_->get(w_->current_game_id_, "conflict_order_reversed",
                          "") == "true";
  const bool include_mod_id =
      w_->knowledge_->get(w_->current_game_id_, "deploy_include_mod_id", "") ==
      "true";
  const auto metadata_file =
      w_->knowledge_->get(w_->current_game_id_, "metadata_file", "meta.ini");

  // Enabled managed mods only - the conflict engine must see them all
  // (no extension filter, unlike the flags column).
  std::vector<std::pair<std::string, int>> mod_infos;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native)
      continue;
    if (!m.enabled)
      continue;
    mod_infos.emplace_back(m.id.toStdString(), m.priority);
  }

  // Game-origin destination: a mod folder named after the game.
  const auto game_display = w_->current_game_name_.empty()
                                ? w_->current_game_id_
                                : w_->current_game_name_;
  std::string game_folder = game_display;
  for (char &c : game_folder) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|')
      c = '_';
  }

  SyncOverwriteDialog dialog(
      SyncOverwriteDialog::Context{
          .overwrite_dir = overwrite_dir,
          .mods_dir = w_->mods_dir_path(),
          .mod_infos = std::move(mod_infos),
          .mods_subpath = mods_subpath,
          .conflict_reversed = conflict_reversed,
          .include_mod_id = include_mod_id,
          .game_dir = w_->current_game_dir_,
          .game_folder = game_folder,
          .game_label = game_display,
          .metadata_file = metadata_file,
      },
      w_);
  if (dialog.exec() != QDialog::Accepted)
    return;

  auto targets = dialog.targets();
  if (targets.empty())
    return;

  auto moved =
      engine::apply_sync_plan(targets, overwrite_dir, w_->mods_dir_path(),
                              mods_subpath, metadata_file, include_mod_id);
  if (moved > 0) {
    engine::Logger::instance().debug(
        "Sync Overwrite: " + std::to_string(moved) + " file(s) moved");
    QMessageBox::information(
        w_, tr("Sync to Mods"),
        tr("Moved %1 file(s) from Overwrite to mods.").arg(moved));
  } else {
    QMessageBox::warning(w_, tr("Sync to Mods"),
                         tr("Failed to sync Overwrite files."));
  }
}

void OverwriteController::open_overwrite_in_file_manager() {
  if (w_->current_instance_root_.empty())
    return;
  auto overwrite_dir = w_->overwrite_dir_path();
  std::error_code ec;
  if (!std::filesystem::is_directory(overwrite_dir, ec)) {
    std::filesystem::create_directories(overwrite_dir, ec);
  }
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(overwrite_dir.string())));
}

void OverwriteController::show_overwrite_info_dialog() {
  if (w_->current_instance_root_.empty())
    return;
  auto overwrite_dir = w_->overwrite_dir_path();
  std::error_code ec;
  if (!std::filesystem::is_directory(overwrite_dir, ec)) {
    std::filesystem::create_directories(overwrite_dir, ec);
  }
  auto mods_subpath = w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                           "mods_subpath", "")
                                     : std::string();

  // Shared modeless dialog - MO2's findChild("__overwriteDialog") pattern.
  auto *dialog = w_->findChild<QDialog *>("__overwriteDialog");
  if (dialog == nullptr) {
    dialog = new ui::OverwriteInfoDialog(overwrite_dir, mods_subpath, w_);
    dialog->setObjectName("__overwriteDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
  } else {
    qobject_cast<ui::OverwriteInfoDialog *>(dialog)->set_path(overwrite_dir);
  }
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

void OverwriteController::move_dropped_overwrite_files(const QStringList &paths,
                                                       int mod_row) {
  if (w_->current_instance_root_.empty())
    return;
  if (mod_row < 0 || mod_row >= w_->mod_model_->mods().size())
    return;
  const auto &target = w_->mod_model_->mods()[mod_row];
  if (target.is_overwrite || target.is_separator || target.is_merged ||
      target.is_game_native) {
    return;
  }
  if (paths.isEmpty())
    return;

  auto overwrite_dir = w_->overwrite_dir_path();
  auto mods_subpath = w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                           "mods_subpath", "")
                                     : std::string();
  if (mods_subpath.empty())
    return;
  const bool include_mod_id =
      w_->knowledge_ &&
      w_->knowledge_->get(w_->current_game_id_, "deploy_include_mod_id", "") ==
          "true";
  auto mod_dir = w_->mods_dir_path() / target.id.toStdString();
  const auto mod_id = target.id.toStdString();

  bool any = false;
  std::error_code ec;
  const auto ow_canon = std::filesystem::weakly_canonical(overwrite_dir, ec);
  for (const auto &p : paths) {
    const auto canon = std::filesystem::weakly_canonical(p.toStdString(), ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const auto rel = std::filesystem::relative(canon, ow_canon, ec);
    if (ec || rel.empty() || rel == "..") {
      ec.clear();
      continue;
    }
    if (engine::move_overwrite_entry_to_mod(overwrite_dir, canon, mod_dir,
                                            mods_subpath, include_mod_id,
                                            mod_id))
      any = true;
  }

  if (any) {
    engine::Logger::instance().debug(
        "Moved dropped Overwrite entries into mod: " + mod_id);
    w_->mod_list_->recompute_conflicts();
  }
}

} // namespace ui