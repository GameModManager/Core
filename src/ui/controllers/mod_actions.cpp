#include "ui/controllers/mod_actions.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/core/log/logger.h"
#include "engine/mod/meta/categories.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/list_dialog.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QColorDialog>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "ui/widgets/status_bar.h"

namespace ui {

namespace {

  bool write_separator_color_file(const std::filesystem::path& mod_dir,
                                  const QString& color) {
    auto meta_path = mod_dir / "meta.ini";
    engine::ModMeta meta;
    if (std::filesystem::exists(meta_path)) {
      std::ifstream f(meta_path);
      if (f) {
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        meta.parse(content);
      }
    }

    const bool had_color = !meta.get("General", "color").empty();

    if (color.isEmpty()) {
      if (!had_color)
        return true;  // nothing stored - nothing to clear
      // Rebuild the meta without the color key (ModMeta has no remove).
      engine::ModMeta rebuilt;
      for (const auto& section : meta.sections()) {
        for (const auto& key : meta.keys(section)) {
          if (section == "General" && key == "color")
            continue;
          rebuilt.set(section, key, meta.get(section, key));
        }
      }
      if (rebuilt.sections().empty()) {
        std::error_code ec;
        std::filesystem::remove(meta_path, ec);
        return true;
      }
      std::ofstream out(meta_path);
      if (!out)
        return false;
      out << rebuilt.serialize();
      return out.good();
    }

    meta.set("General", "color", color.toStdString());
    std::ofstream out(meta_path);
    if (!out)
      return false;
    out << meta.serialize();
    return out.good();
  }

  // Remove the mod's in-folder meta.ini (delete cleanup). Logs on failure;
  // never silently swallows a filesystem error.
  void remove_sidecar(const std::filesystem::path& mods_dir, const QString& id) {
    if (mods_dir.empty())
      return;
    auto path = mods_dir / id.toStdString() / "meta.ini";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      std::filesystem::remove(path, ec);
      if (ec)
        engine::Logger::instance().warn("remove_sidecar: failed to remove " +
                                        path.string());
    }
  }

}  // namespace

ModActions::ModActions(MainWindow* w) : w_(w) {}

void ModActions::set_sync_mod_enable_state(
    std::function<void(const QString&, bool)> cb) {
  sync_mod_enable_state_cb_ = std::move(cb);
}

void ModActions::set_refresh_data_tab(std::function<void()> cb) {
  refresh_data_tab_cb_ = std::move(cb);
}

void ModActions::set_apply_mod_filter(std::function<void()> cb) {
  apply_mod_filter_cb_ = std::move(cb);
}

void ModActions::set_load_mods_from_game(std::function<void()> cb) {
  load_mods_from_game_cb_ = std::move(cb);
}

void ModActions::remove_selected_mods() {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  QStringList names;
  for (const auto& idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    if (w_->mod_model_->mods()[r].is_overwrite)
      continue;
    names.append(w_->mod_model_->mods()[r].name);
  }
  if (names.isEmpty())
    return;

  auto reply = QMessageBox::question(
      w_, QObject::tr("Remove Mods"),
      QObject::tr("Move %1 mod(s) to the trash bin?\n\n%2\n\nTheir files stay "
                  "in the system trash and can be restored.")
          .arg(names.size())
          .arg(names.join("\n")),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply != QMessageBox::Yes)
    return;

  auto mods_subpath =
      w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "")
                     : std::string();
  auto mods_dir = w_->mods_dir_path();

  for (const auto& idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto& entry = w_->mod_model_->mods()[r];
    if (entry.is_overwrite)
      continue;

    // The mods dir is INSTANCE-owned (Workspace-tnj): physical removal needs
    // mods_dir_path(), not the game dir.
    if (!mods_subpath.empty() && !w_->mods_dir_path().empty()) {
      auto mod_folder = w_->mods_dir_path() / entry.id.toStdString();
      if (!engine::remove_path(mod_folder)) {
        engine::Logger::instance().error("Failed to move mod folder to trash: " +
                                         mod_folder.string());
      }
    }
    // Delete cleanup: drop the mod's in-folder meta.ini so a removed mod
    // leaves no orphaned metadata. Children's parent_id is cleared by the
    // model detach and rewritten by sync_mod_ui_state() via mod_list_changed.
    remove_sidecar(mods_dir, entry.id);
    w_->mod_model_->remove_mod(entry.id);
  }
}

void ModActions::move_to_separator(const QString& mod_id, const QString& sep_id) {
  // Placement lives in the model: the mod lands in the LAST slot inside the
  // separator's band (lowest priority / last top-down), never first.
  w_->mod_model_->move_to_separator(mod_id, sep_id);
}

void ModActions::send_to_separator(const QString& mod_id) {
  // MO2 sendModsToSeparator (modlistviewactions.cpp:661-701): collect the
  // separators in mod-list order into the shared ListDialog and move the mod
  // to the chosen one. Ids ride item data so duplicate display names can't
  // misresolve.
  QStringList names;
  QList<QVariant> ids;
  for (const auto& m : w_->mod_model_->mods()) {
    if (m.is_separator) {
      names << m.name;
      ids << m.id;
    }
  }
  if (names.isEmpty())
    return;

  ui::ListDialog dlg(w_);
  dlg.setWindowTitle(QObject::tr("Select a separator..."));
  dlg.setChoices(names);
  dlg.setChoiceData(ids);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString sep_id = dlg.getChoiceData().toString();
  if (!sep_id.isEmpty())
    move_to_separator(mod_id, sep_id);
}

void ModActions::send_selected_to_separator(const QStringList& mod_ids) {
  if (mod_ids.isEmpty())
    return;

  // Collect separators for the list dialog (same as send_to_separator).
  QStringList names;
  QList<QVariant> ids;
  for (const auto& m : w_->mod_model_->mods()) {
    if (m.is_separator) {
      names << m.name;
      ids << m.id;
    }
  }
  if (names.isEmpty())
    return;

  ui::ListDialog dlg(w_);
  dlg.setWindowTitle(QObject::tr("Select a separator..."));
  dlg.setChoices(names);
  dlg.setChoiceData(ids);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString sep_id = dlg.getChoiceData().toString();
  if (sep_id.isEmpty())
    return;

  for (const auto& mod_id : mod_ids)
    move_to_separator(mod_id, sep_id);
}

void ModActions::send_to_highest_priority(const QString& id) {
  if (w_->mod_model_->is_conflict_order_reversed()) {
    // Isaac: lowest priority number = highest priority = top of list
    w_->mod_model_->move_mod(id, 0);
  } else {
    // Standard (MO2): highest priority number = highest priority = bottom of
    // list
    int ow_row = w_->mod_model_->overwrite_row();
    int target = ow_row >= 0 ? ow_row - 1 : w_->mod_model_->mods().size() - 1;
    if (target < 0)
      target = 0;
    w_->mod_model_->move_mod(id, target);
  }
}

void ModActions::send_to_lowest_priority(const QString& id) {
  if (w_->mod_model_->is_conflict_order_reversed()) {
    // Isaac: highest priority number = lowest priority = bottom of list
    // (below the pinned Overwrite/MERGED which sit at the top).
    w_->mod_model_->move_mod(id, w_->mod_model_->mods().size() - 1);
  } else {
    // Standard (MO2): lowest priority number = lowest priority = top of list
    w_->mod_model_->move_mod(id, 0);
  }
}

void ModActions::send_to_highest_in_separator(const QString& id) {
  const auto& mods = w_->mod_model_->mods();
  int mod_row      = w_->mod_model_->priority_of(id);
  if (mod_row < 0)
    return;

  QString sep_id = mods[mod_row].separator_id;
  if (sep_id.isEmpty())
    return;

  int sep_row = -1;
  for (int i = mod_row - 1; i >= 0; --i) {
    if (mods[i].is_separator && mods[i].id == sep_id) {
      sep_row = i;
      break;
    }
  }
  if (sep_row < 0)
    return;
  w_->mod_model_->move_mod(id, sep_row + 1);
}

void ModActions::send_to_lowest_in_separator(const QString& id) {
  const auto& mods = w_->mod_model_->mods();
  int mod_row      = w_->mod_model_->priority_of(id);
  if (mod_row < 0)
    return;

  QString sep_id = mods[mod_row].separator_id;
  if (sep_id.isEmpty())
    return;

  int ow_row = w_->mod_model_->overwrite_row();
  int target = w_->mod_model_->is_conflict_order_reversed()
                   ? mods.size()
                   : (ow_row >= 0 ? ow_row : mods.size());

  for (int i = mod_row + 1; i < mods.size(); ++i) {
    if (mods[i].is_separator) {
      target = i;
      break;
    }
  }
  w_->mod_model_->move_mod(id, target - 1);
}

void ModActions::priority_move_selected(int step) {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  int r            = sel.first().row();
  const auto& mods = w_->mod_model_->mods();
  if (r < 0 || r >= mods.size())
    return;
  const auto& e = mods[r];
  if (e.is_separator || e.is_overwrite || e.is_merged)
    return;

  int target = r + step;
  if (target < 0 || target >= mods.size())
    return;
  if (mods[target].is_separator || mods[target].is_overwrite || mods[target].is_merged)
    return;

  w_->mod_model_->move_mod(e.id, target);
}

void ModActions::toggle_selected_mods(bool enabled) {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  for (const auto& idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto& entry = w_->mod_model_->mods()[r];
    if (entry.is_separator || entry.is_overwrite || entry.is_game_native)
      continue;

    // Check if state would actually change
    if (entry.enabled == enabled)
      continue;

    w_->mod_model_->setData(w_->mod_model_->index(r, ModList::Name),
                            enabled ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
    if (sync_mod_enable_state_cb_)
      sync_mod_enable_state_cb_(entry.id, enabled);
  }
}

void ModActions::toggle_root_override(const QList<int>& rows, bool on) {
  for (int r : rows) {
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto& entry = w_->mod_model_->mods()[r];
    if (entry.is_separator || entry.is_overwrite || entry.is_merged ||
        entry.is_game_native)
      continue;
    if (entry.root_override == on)
      continue;

    auto mod_dir  = w_->mods_dir_path() / entry.id.toStdString();
    auto meta_ini = mod_dir / "meta.ini";
    std::error_code ec;
    engine::ModMeta meta;
    if (std::filesystem::is_regular_file(meta_ini, ec)) {
      meta = engine::ModMeta::load_file(meta_ini);
    }
    meta.set("General", "rootOverride", on ? "1" : "0");
    if (!meta.save_file(meta_ini)) {
      engine::Logger::instance().warn("toggle_root_override: failed to write " +
                                      meta_ini.string());
      continue;
    }
    w_->mod_model_->set_root_override(entry.id, on);
  }
  if (refresh_data_tab_cb_)
    refresh_data_tab_cb_();
}

QString ModActions::create_separator_named(const QString& name, const QString& color) {
  // Separators are instance-owned (folder under the instance mods dir);
  // no game dir required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return {};

  auto mods_subpath = w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  auto separator_suffix =
      w_->knowledge_->get(w_->current_game_id_, "separator_suffix", "_separator");
  if (mods_subpath.empty())
    return {};

  // Guard against duplicate names
  if (w_->mod_model_->existing_separator_names().contains(name))
    return {};

  auto folder_name = name.toStdString() + separator_suffix;
  auto sep_dir     = w_->mods_dir_path() / folder_name;

  // Create the separator folder
  std::error_code ec;
  std::filesystem::create_directories(sep_dir, ec);
  if (ec)
    return {};

  // MO2 writes separator metadata into the mod folder's meta.ini; the only
  // persistent field GMM uses is the color. The display name derives from
  // the folder name minus the separator suffix (ModList::getDisplayName),
  // so no explicit name key is needed.
  if (!color.isEmpty()) {
    if (!write_separator_color_file(sep_dir, color)) {
      std::filesystem::remove_all(sep_dir, ec);
      return {};
    }
  }

  // Add to model
  auto id = QString::fromStdString(folder_name);
  w_->mod_model_->add_separator(id, name, color);
  engine::Logger::instance().debug("Separator created: " + name.toStdString());
  return id;
}

void ModActions::create_separator() {
  // Instance-owned (Workspace-tnj) - see create_separator_named.
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // MO2 createSeparator (modlistviewactions.cpp:152-204): a name-only prompt
  // filtered through fixDirectoryName; the previously used separator color is
  // inherited automatically - there is no color picker in w_ step.
  QString name;
  while (true) {
    bool ok = false;
    name    = QInputDialog::getText(
        w_, QObject::tr("Create Separator..."),
        QObject::tr("This will create a new separator.\nPlease enter a name:"),
        QLineEdit::Normal, name, &ok);
    if (!ok)
      return;
    name = QString::fromStdString(engine::sanitize_directory_name(name.toStdString()));
    if (!name.isEmpty())
      break;
  }

  // Check for duplicate names
  if (w_->mod_model_->existing_separator_names().contains(name)) {
    QMessageBox::warning(w_, QObject::tr("Create Separator..."),
                         QObject::tr("A separator with w_ name already exists."));
    return;
  }

  auto previous       = Settings::instance().previous_separator_color();
  const QString color = previous ? previous->name(QColor::HexArgb) : QString();
  if (create_separator_named(name, color).isEmpty()) {
    QMessageBox::warning(w_, QObject::tr("Create Separator..."),
                         QObject::tr("Failed to create separator directory."));
  }
}

void ModActions::create_empty_mod() {
  // Instance-owned: the empty mod folder is written into the instance mods
  // dir; no game dir required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  bool ok;
  auto name = QInputDialog::getText(w_, QObject::tr("Create Empty Mod"),
                                    QObject::tr("Mod name:"), QLineEdit::Normal,
                                    QString(), &ok);
  if (!ok || name.trimmed().isEmpty())
    return;

  // Check for duplicate names
  QString trimmed = name.trimmed();
  for (const auto& m : w_->mod_model_->mods()) {
    if (!m.is_separator && !m.is_overwrite && !m.is_merged &&
        (m.name.compare(trimmed, Qt::CaseInsensitive) == 0 ||
         m.id.compare(trimmed, Qt::CaseInsensitive) == 0)) {
      QMessageBox::warning(w_, QObject::tr("Create Empty Mod"),
                           QObject::tr("A mod with w_ name already exists."));
      return;
    }
  }

  // Sanitize the folder name (drop path separators and reserved characters)
  QString folder = trimmed;
  folder.replace(QRegularExpression(R"([/\\:*?"<>|])"), "_");

  auto mods_dir = w_->mods_dir_path();
  std::error_code ec;
  std::filesystem::create_directories(mods_dir, ec);
  if (ec) {
    QMessageBox::warning(w_, QObject::tr("Create Empty Mod"),
                         QObject::tr("Failed to create mods directory."));
    return;
  }

  auto mod_dir = mods_dir / folder.toStdString();
  if (std::filesystem::exists(mod_dir, ec)) {
    QMessageBox::warning(
        w_, QObject::tr("Create Empty Mod"),
        QObject::tr("A folder named %1 already exists in the mods directory.")
            .arg(folder));
    return;
  }
  std::filesystem::create_directories(mod_dir, ec);
  if (ec) {
    QMessageBox::warning(w_, QObject::tr("Create Empty Mod"),
                         QObject::tr("Failed to create mod folder."));
    return;
  }

  // Write the game's metadata file into the mod folder so ModScanner picks
  // the mod up. MO2-style games get a meta.ini (same keys MO2 and
  // InstallStage write); XML games (Isaac) get their metadata.xml.
  // Empty mod: no Nexus provenance, so the meta.ini modid slot must be
  // empty (writes "0", the MO2 "no Nexus id" sentinel). Without this
  // the user's mod would inherit a fake Nexus attribution just because
  // it has a folder (Workspace-rvld).
  auto metadata_file =
      w_->knowledge_->get(w_->current_game_id_, "metadata_file", "meta.ini");
  engine::ModMeta::write_game_metadata(mod_dir, metadata_file, trimmed.toStdString(),
                                       "1.0", "");

  engine::Logger::instance().debug("Empty mod created: " + folder.toStdString());
  if (load_mods_from_game_cb_)
    load_mods_from_game_cb_();
}

void ModActions::create_separator_at_row(int row) {
  // Instance-owned (Workspace-tnj) - see create_separator_named.
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // Same MO2 flow as create_separator(): name-only prompt, previous color.
  QString name;
  while (true) {
    bool ok = false;
    name    = QInputDialog::getText(
        w_, QObject::tr("Create Separator..."),
        QObject::tr("This will create a new separator.\nPlease enter a name:"),
        QLineEdit::Normal, name, &ok);
    if (!ok)
      return;
    name = QString::fromStdString(engine::sanitize_directory_name(name.toStdString()));
    if (!name.isEmpty())
      break;
  }

  // Check for duplicate names
  if (w_->mod_model_->existing_separator_names().contains(name)) {
    QMessageBox::warning(w_, QObject::tr("Create Separator..."),
                         QObject::tr("A separator with w_ name already exists."));
    return;
  }

  auto previous       = Settings::instance().previous_separator_color();
  const QString color = previous ? previous->name(QColor::HexArgb) : QString();
  auto id             = create_separator_named(name, color);
  if (id.isEmpty()) {
    QMessageBox::warning(w_, QObject::tr("Create Separator..."),
                         QObject::tr("Failed to create separator directory."));
    return;
  }

  // Move the new separator to the target row (below the clicked row)
  int insert_row = row + 1;
  w_->mod_model_->move_mod(id, insert_row);
  engine::Logger::instance().debug("Separator created at row " +
                                   std::to_string(insert_row) + ": " +
                                   name.toStdString());
}

void ModActions::rename_mod_inline(int row) {
  if (row < 0 || row >= w_->mod_model_->mods().size())
    return;
  const auto& mod = w_->mod_model_->mods()[row];
  if (mod.is_overwrite || mod.is_merged || mod.is_game_native)
    return;
  w_->mod_view_->edit(w_->mod_model_->index(row, ModList::Name));
}

void ModActions::apply_rename(int row, const QString& name) {
  const auto revert = [this, row]() {
    emit w_->mod_model_->dataChanged(w_->mod_model_->index(row, ModList::Name),
                                     w_->mod_model_->index(row, ModList::Version));
  };

  if (row < 0 || row >= w_->mod_model_->mods().size())
    return;
  const auto& entry = w_->mod_model_->mods()[row];
  if (entry.is_overwrite || entry.is_merged || entry.is_game_native) {
    revert();
    return;
  }

  if (name == entry.name) {
    revert();
    return;
  }  // unchanged

  // Instance-owned: the rename moves the folder under the instance mods dir
  // (+ meta sidecar); no game dir required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty()) {
    revert();
    return;
  }

  auto mods_subpath = w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  auto separator_suffix =
      w_->knowledge_->get(w_->current_game_id_, "separator_suffix", "_separator");
  if (mods_subpath.empty()) {
    revert();
    return;
  }

  // Internal name = folder name on disk (MO2 makeInternalName): separators
  // get the suffix appended, everything else is the raw name.
  auto clean = engine::sanitize_directory_name(name.toStdString());
  if (clean.empty()) {
    QMessageBox::warning(w_, QObject::tr("Rename"), QObject::tr("Invalid name."));
    revert();
    return;
  }
  const QString display_name = QString::fromStdString(clean);
  std::string internal       = clean;
  if (entry.is_separator)
    internal += separator_suffix;
  const QString new_id = QString::fromStdString(internal);

  if (new_id == entry.id) {
    revert();
    return;
  }  // sanitized back to the same folder

  // Duplicate check (case-insensitive, excluding self) - MO2 renameMod.
  for (const auto& m : w_->mod_model_->mods()) {
    if (m.id == entry.id)
      continue;
    if (m.id.compare(new_id, Qt::CaseInsensitive) == 0) {
      QMessageBox::warning(w_, QObject::tr("Rename"),
                           QObject::tr("Name is already in use by another mod."));
      revert();
      return;
    }
  }

  auto mods_dir       = w_->mods_dir_path();
  const auto old_path = mods_dir / entry.id.toStdString();
  const auto new_path = mods_dir / new_id.toStdString();

  std::error_code ec;
  if (std::filesystem::exists(new_path, ec)) {
    QMessageBox::warning(
        w_, QObject::tr("Rename"),
        QObject::tr("A folder named %1 already exists in the mods directory.")
            .arg(new_id));
    revert();
    return;
  }

  if (std::filesystem::exists(old_path, ec)) {
    std::filesystem::rename(old_path, new_path, ec);
    if (ec) {
      QMessageBox::warning(w_, QObject::tr("Rename"),
                           QObject::tr("Failed to rename mod folder."));
      revert();
      return;
    }
  }

  // The folder moved, so the in-folder meta.ini moved with it - just
  // re-stamp its [GameModManager] folder key to the new id.
  if (!mods_dir.empty()) {
    auto meta_path = mods_dir / new_id.toStdString() / "meta.ini";
    if (std::filesystem::exists(meta_path, ec)) {
      auto meta = engine::ModMeta::load(mods_dir, new_id.toStdString());
      meta.set("GameModManager", "folder", new_id.toStdString());
      meta.save(mods_dir, new_id.toStdString());
    }
  }

  w_->mod_model_->rename_mod_in_place(row, new_id, display_name);
  engine::Logger::instance().debug("Renamed mod: " + entry.name.toStdString() + " -> " +
                                   display_name.toStdString());
}

void ModActions::delete_separator(int row) {
  if (row < 0 || row >= w_->mod_model_->mods().size())
    return;
  const auto& mod = w_->mod_model_->mods()[row];
  if (!mod.is_separator)
    return;

  auto reply = QMessageBox::question(
      w_, QObject::tr("Delete Separator"),
      QObject::tr("Move separator \"%1\" to the trash bin?\n\nIt can be "
                  "restored from the system trash.")
          .arg(mod.name),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply != QMessageBox::Yes)
    return;

  // Separators are pure UI/model constructs (+ an optional folder in the
  // instance mods dir), so deletion must proceed even without a game dir
  // (Workspace-tnj) - only knowledge/game_id gate the disk access below.
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  auto mods_subpath = w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  if (!mods_subpath.empty() && !w_->mods_dir_path().empty()) {
    auto sep_folder = w_->mods_dir_path() / mod.id.toStdString();
    if (!engine::remove_path(sep_folder)) {
      engine::Logger::instance().error("Failed to move separator folder to trash: " +
                                       sep_folder.string());
    }
  }

  // Delete cleanup: drop the mod's in-folder meta.ini. Children of the
  // separator are detached by the model (parent_id cleared) and their
  // in-folder metas rewritten by sync_mod_ui_state() via mod_list_changed.
  remove_sidecar(w_->mods_dir_path(), mod.id);
  w_->mod_model_->remove_mod(mod.id);
  engine::Logger::instance().debug("Separator deleted: " + mod.name.toStdString());
}

void ModActions::select_color_for_selected() {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  const auto& ref = w_->mod_model_->mods()[sel.first().row()];
  QColor current;
  if (ref.is_separator && !ref.separator_color.isEmpty())
    current = QColor(ref.separator_color);

  // MO2 setColor (modlistviewactions.cpp:1195-1224): standalone color dialog
  // with alpha; prefills the current color or the remembered previous one.
  QColorDialog dialog(w_);
  dialog.setOption(QColorDialog::ShowAlphaChannel);
  if (current.isValid()) {
    dialog.setCurrentColor(current);
  } else if (auto prev = Settings::instance().previous_separator_color()) {
    dialog.setCurrentColor(*prev);
  }
  if (dialog.exec() != QDialog::Accepted)
    return;

  const auto color = dialog.currentColor();
  if (!color.isValid())
    return;

  Settings::instance().set_previous_separator_color(color);
  const QString hex = color.name(QColor::HexArgb);

  for (const auto& idx : sel) {
    int row = idx.row();
    if (row < 0 || row >= w_->mod_model_->mods().size())
      continue;
    const auto& mod = w_->mod_model_->mods()[row];
    if (!mod.is_separator)
      continue;
    write_separator_color_file(w_->mods_dir_path() / mod.id.toStdString(), hex);
    w_->mod_model_->set_mod_color(mod.id, color);
  }
}

void ModActions::reset_color_for_selected() {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  for (const auto& idx : sel) {
    int row = idx.row();
    if (row < 0 || row >= w_->mod_model_->mods().size())
      continue;
    const auto& mod = w_->mod_model_->mods()[row];
    if (!mod.is_separator)
      continue;
    write_separator_color_file(w_->mods_dir_path() / mod.id.toStdString(), QString());
    w_->mod_model_->clear_mod_color(mod.id);
  }
  Settings::instance().remove_previous_separator_color();
}

// ---------------------------------------------------------------------------
// Mirror/backup for external mods (Workspace-0pi5)
// ---------------------------------------------------------------------------

namespace {

  // Folder mtime as epoch seconds (the [Mirror] sourceTimestamp). 0 on error.
  int64_t folder_mtime_secs(const std::filesystem::path& dir) {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(dir, ec);
    if (ec)
      return 0;
    // Same __file_clock -> system_clock conversion as the scanner's
    // folder_timestamps: the raw duration is not Unix-epoch based.
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::clock_cast<std::chrono::system_clock>(mtime)
                .time_since_epoch())
            .count());
  }

}  // namespace

// Resolve the live external source folder for a mod row: content_dir when it
// is still a directory, else the game's external mods dir. Empty when
// neither exists (not an external mod, or its source is gone).
std::filesystem::path ModActions::mirror_source_for(const ModEntry& entry) {
  std::error_code ec;
  if (!entry.content_dir.isEmpty()) {
    std::filesystem::path c(entry.content_dir.toStdString());
    if (std::filesystem::is_directory(c, ec))
      return c;
  }
  auto game_mods = w_->current_game_mods_dir();
  if (!game_mods.empty()) {
    auto p = game_mods / entry.id.toStdString();
    if (std::filesystem::is_directory(p, ec))
      return p;
  }
  return {};
}

void ModActions::set_action_status(const QString& text) {
  if (w_->status_bar_)
    w_->status_bar_->set_status(text);
}

// Copy the external source folder into instance/mods/ and stamp [Mirror]
// (sourcePath + source mtime) in BOTH the source meta and the backup meta:
// the source copy drives the mirrored badge while the source is present,
// the backup copy drives source-missing detection and the prune skip after
// the source is gone. Re-mirroring compares the live source mtime against
// the stored timestamp and refreshes the backup on mismatch.
ModActions::MirrorResult ModActions::mirror_single(const QString& mod_id) {
  const ModEntry* found = nullptr;
  for (const auto& m : w_->mod_model_->mods()) {
    if (m.id == mod_id) {
      found = &m;
      break;
    }
  }
  if (!found)
    return MirrorResult::Failed;
  const ModEntry entry = *found;
  if (entry.is_separator || entry.is_overwrite || entry.is_merged ||
      entry.is_game_native)
    return MirrorResult::Failed;

  const auto mods_dir = w_->mods_dir_path();
  if (mods_dir.empty())
    return MirrorResult::Failed;

  const auto source = mirror_source_for(entry);
  if (source.empty()) {
    engine::Logger::instance().warn("mirror_mod: no live source for '" +
                                    mod_id.toStdString() + "'");
    return MirrorResult::Failed;
  }

  const int64_t source_mtime = folder_mtime_secs(source);
  const auto backup          = mods_dir / entry.id.toStdString();

  // Already mirrored: compare the live source mtime to the stored
  // timestamp (source meta first, backup meta as fallback for XML games
  // whose source meta may be absent). Match + backup present = no-op.
  if (entry.is_mirrored && source_mtime != 0) {
    int64_t stored      = 0;
    const auto src_meta = engine::ModMeta::load_file(source / "meta.ini");
    if (src_meta.is_mirrored())
      stored = src_meta.mirror_source_timestamp();
    if (stored == 0) {
      const auto bak_meta = engine::ModMeta::load(mods_dir, entry.id.toStdString());
      if (bak_meta.is_mirrored())
        stored = bak_meta.mirror_source_timestamp();
    }
    std::error_code ec;
    if (stored == source_mtime && std::filesystem::is_directory(backup, ec))
      return MirrorResult::AlreadyMirrored;
  }

  // Snapshot copy: drop any stale backup first so files deleted from the
  // source vanish from the backup too.
  std::error_code ec;
  std::filesystem::create_directories(mods_dir, ec);
  if (ec) {
    engine::Logger::instance().warn("mirror_mod: cannot create mods dir: " +
                                    ec.message());
    return MirrorResult::Failed;
  }
  std::filesystem::remove_all(backup, ec);
  ec.clear();
  std::filesystem::copy(source, backup,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        ec);
  if (ec) {
    engine::Logger::instance().warn("mirror_mod: copy failed for '" +
                                    mod_id.toStdString() + "': " + ec.message());
    return MirrorResult::Failed;
  }

  // Stamp the marker in the source meta (the marker belongs to the source
  // mod) and in the backup meta (drives source-missing UI + prune skip).
  const std::string source_str = source.string();
  {
    auto src_meta = engine::ModMeta::load_file(source / "meta.ini");
    src_meta.set_mirror(source_str, source_mtime);
    if (!src_meta.save_file(source / "meta.ini"))
      engine::Logger::instance().warn("mirror_mod: failed to stamp source meta for '" +
                                      mod_id.toStdString() + "'");
  }
  {
    auto bak_meta = engine::ModMeta::load(mods_dir, entry.id.toStdString());
    bak_meta.set_mirror(source_str, source_mtime);
    if (!bak_meta.save(mods_dir, entry.id.toStdString()))
      engine::Logger::instance().warn("mirror_mod: failed to stamp backup meta for '" +
                                      mod_id.toStdString() + "'");
  }

  w_->mod_model_->set_mirror_info(mod_id, true, false,
                                  QString::fromStdString(source_str));
  engine::Logger::instance().info("mirror_mod: '" + mod_id.toStdString() +
                                  "' mirrored from " + source_str);
  return MirrorResult::Mirrored;
}

// Delete the instance backup and clear the source marker (when the source
// still exists). The model row keeps pointing at the source.
ModActions::MirrorResult ModActions::unmirror_single(const QString& mod_id) {
  const ModEntry* found = nullptr;
  for (const auto& m : w_->mod_model_->mods()) {
    if (m.id == mod_id) {
      found = &m;
      break;
    }
  }
  if (!found || !found->is_mirrored)
    return MirrorResult::Failed;
  const ModEntry entry = *found;

  const auto mods_dir = w_->mods_dir_path();
  if (mods_dir.empty())
    return MirrorResult::Failed;

  // The recorded source path (backup meta first, source meta as fallback),
  // then the row's content_dir, for the marker cleanup below.
  std::string stored_path;
  {
    const auto bak_meta = engine::ModMeta::load(mods_dir, entry.id.toStdString());
    if (bak_meta.is_mirrored())
      stored_path = bak_meta.mirror_source_path();
  }
  if (stored_path.empty() && !entry.content_dir.isEmpty()) {
    std::error_code ec;
    std::filesystem::path c(entry.content_dir.toStdString());
    if (std::filesystem::exists(c, ec))
      stored_path = c.string();
  }

  // Delete the backup copy (to the system trash, like mod removal).
  const auto backup = mods_dir / entry.id.toStdString();
  {
    std::error_code ec;
    if (std::filesystem::is_directory(backup, ec) && !engine::remove_path(backup)) {
      engine::Logger::instance().warn("unmirror_mod: failed to remove backup for '" +
                                      mod_id.toStdString() + "'");
      return MirrorResult::Failed;
    }
  }

  // Remove the [Mirror] section from the source meta when it still exists.
  if (!stored_path.empty()) {
    std::error_code ec;
    std::filesystem::path src(stored_path);
    if (std::filesystem::is_directory(src, ec)) {
      auto src_meta = engine::ModMeta::load_file(src / "meta.ini");
      if (src_meta.is_mirrored()) {
        src_meta.clear_mirror();
        if (!src_meta.save_file(src / "meta.ini"))
          engine::Logger::instance().warn(
              "unmirror_mod: failed to clear source marker for '" +
              mod_id.toStdString() + "'");
      }
    }
  }

  w_->mod_model_->set_mirror_info(mod_id, false, false, {});
  engine::Logger::instance().info("unmirror_mod: backup removed for '" +
                                  mod_id.toStdString() + "'");
  return MirrorResult::Mirrored;
}

void ModActions::mirror_mod(const QString& mod_id) {
  switch (mirror_single(mod_id)) {
  case MirrorResult::Mirrored:
    set_action_status(QObject::tr("Mod mirrored to instance"));
    break;
  case MirrorResult::AlreadyMirrored:
    set_action_status(QObject::tr("Mod already mirrored to instance"));
    break;
  case MirrorResult::Failed:
    set_action_status(QObject::tr("Failed to mirror mod to instance"));
    break;
  }
}

void ModActions::unmirror_mod(const QString& mod_id) {
  switch (unmirror_single(mod_id)) {
  case MirrorResult::Mirrored:
    set_action_status(QObject::tr("Mirror backup removed"));
    break;
  case MirrorResult::AlreadyMirrored:
    set_action_status(QObject::tr("Mod is not mirrored"));
    break;
  case MirrorResult::Failed:
    set_action_status(QObject::tr("Failed to remove mirror backup"));
    break;
  }
}

void ModActions::mirror_selected_mods() {
  auto sel  = w_->mod_view_->selectionModel()->selectedRows();
  int total = 0;
  int done  = 0;
  for (const auto& idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto& entry = w_->mod_model_->mods()[r];
    if (entry.is_separator || entry.is_overwrite || entry.is_merged ||
        entry.is_game_native || entry.is_mirrored)
      continue;
    ++total;
    if (mirror_single(entry.id) != MirrorResult::Failed)
      ++done;
  }
  if (total == 0)
    return;
  if (done == total)
    set_action_status(QObject::tr("%1 mod(s) mirrored to instance").arg(done));
  else
    set_action_status(QObject::tr("Mirrored %1 of %2 selected mods to instance")
                          .arg(done)
                          .arg(total));
}

void ModActions::unmirror_selected_mods() {
  auto sel  = w_->mod_view_->selectionModel()->selectedRows();
  int total = 0;
  int done  = 0;
  for (const auto& idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto& entry = w_->mod_model_->mods()[r];
    if (!entry.is_mirrored)
      continue;
    ++total;
    if (unmirror_single(entry.id) != MirrorResult::Failed)
      ++done;
  }
  if (total == 0)
    return;
  if (done == total)
    set_action_status(QObject::tr("%1 mirror backup(s) removed").arg(done));
  else
    set_action_status(
        QObject::tr("Removed %1 of %2 mirror backups").arg(done).arg(total));
}

}  // namespace ui
