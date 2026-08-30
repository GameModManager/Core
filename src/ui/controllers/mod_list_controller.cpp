#include "ui/controllers/mod_list_controller.h"
#include "engine/profile/profile_creation.h"
#include "ui/controllers/launch_controller.h"
#include "ui/controllers/overwrite_controller.h"
#include "ui/controllers/queue_controller.h"
#include "ui/controllers/settings_controller.h"
#include "ui/settings/categories_dialog.h"
#include <QSplitter>
#include <filesystem>

#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLCDNumber>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>

#include "engine/core/events/event_bus.h"
#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/toml_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/trace/trace_recorder.h"
#include "engine/core/util/fs_utils.h"
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/index/conflict_engine.h"
#include "engine/mod/meta/categories.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/overwrite/overwrite_utils.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/platform/theme/theme_manager.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"
#include "engine/profile/profile_switching.h"
#include "engine/sort/sort_provider.h"
#include "engine/sort/sort_registry.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/nxm/managed_games.h"
#include "engine/source/source_provider.h"
#include "ui/main_window/conflict_scan_worker.h"
#include "ui/main_window/loot_sort_worker.h"
#include "ui/main_window/mod_scan_worker.h"
#include "ui/main_window/plugin_db_load_worker.h"
#include "ui/modinfo/mod_info_data.h"
#include "ui/modinfo/mod_info_dialog.h"
#include "ui/panels/tab_panels.h"
#include "ui/preview/preview_window.h"
#include "ui/profile/profile_manager_dialog.h"
#include "ui/settings/settings.h"
#include "ui/theme/icon_manager.h"
#include "ui/widgets/category_filter_panel.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/status_bar.h"
#include "ui/widgets/list_dialog.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/right_panel.h"

namespace ui {

namespace {

QString mod_column_name(int column) {
  switch (column) {
  case ModListModel::Name:
    return "Name";
  case ModListModel::Conflicts:
    return "Conflicts";
  case ModListModel::Flags:
    return "Flags";
  case ModListModel::Category:
    return "Category";
  case ModListModel::Source:
    return "Source";
  case ModListModel::SourceId:
    return "Source ID";
  case ModListModel::Version:
    return "Version";
  case ModListModel::Installation:
    return "Installation";
  case ModListModel::Changed:
    return "Changed";
  case ModListModel::Priority:
    return "Priority";
  }
  return {};
}

bool write_separator_color_file(const std::filesystem::path &mod_dir,
                                const QString &color) {
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
      return true; // nothing stored - nothing to clear
    // Rebuild the meta without the color key (ModMeta has no remove).
    engine::ModMeta rebuilt;
    for (const auto &section : meta.sections()) {
      for (const auto &key : meta.keys(section)) {
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

// Per-row UI state persisted in the manager sidecar
// ({instance_root}/meta/{folder_name}.ini, [GameModManager] section):
// folded (tree-view collapse) and parent_id (visual-nesting link; absent =
// top-level). The sidecar is the single source of truth for these fields;
// instance.toml's legacy folded_separators/folded_mods/mod_parents are only
// a one-release read-compat fallback (migrated into the sidecar on load).
struct SidecarUiState {
  engine::ModMeta meta; // loaded sidecar (empty when no file exists)
  bool has_folded = false;
  bool folded = false;
  bool has_parent = false;
  QString parent_id;
};

SidecarUiState load_sidecar_ui_state(const std::filesystem::path &meta_dir,
                                     const QString &id) {
  SidecarUiState out;
  if (meta_dir.empty())
    return out;
  out.meta = engine::ModMeta::load(meta_dir, id.toStdString());
  for (const auto &key : out.meta.keys("GameModManager")) {
    if (key == "folded") {
      out.has_folded = true;
      out.folded = out.meta.get("GameModManager", "folded") == "true";
    } else if (key == "parent_id") {
      auto v = out.meta.get("GameModManager", "parent_id");
      if (!v.empty()) {
        out.has_parent = true;
        out.parent_id = QString::fromStdString(v);
      }
    }
  }
  return out;
}

// Remove the manager sidecar for a mod id (delete cleanup). Logs on failure;
// never silently swallows a filesystem error.
void remove_sidecar(const std::filesystem::path &meta_dir, const QString &id) {
  if (meta_dir.empty())
    return;
  auto sidecar = meta_dir / (id.toStdString() + ".ini");
  std::error_code ec;
  if (std::filesystem::exists(sidecar, ec) &&
      !std::filesystem::remove(sidecar, ec)) {
    engine::Logger::instance().error("Failed to remove sidecar: " +
                                     sidecar.string());
  }
}

std::vector<QStringList> parse_csv(const QByteArray &data) {
  std::vector<QStringList> rows;
  const QString text = QString::fromUtf8(data);
  QStringList current;
  QString field;
  bool in_quotes = false;
  for (int i = 0; i < text.size(); ++i) {
    const QChar c = text[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field += c;
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      current << field;
      field.clear();
    } else if (c == '\n') {
      current << field;
      rows.push_back(current);
      current.clear();
      field.clear();
    } else if (c != '\r') {
      field += c;
    }
  }
  if (!field.isEmpty() || !current.isEmpty()) {
    current << field;
    rows.push_back(current);
  }
  return rows;
}

QString csv_escape(const QString &field) {
  if (!field.contains(',') && !field.contains('"') && !field.contains('\n'))
    return field;
  QString quoted = field;
  quoted.replace("\"", "\"\"");
  return "\"" + quoted + "\"";
}

// Converts a legacy absolute toolbar-shortcut pin to a game-relative path
// (Issue #34 schema migration). Returns the input unchanged when it is not
// absolute, cannot be resolved relative to the game dir, or escapes the game
// dir (e.g. /usr/bin/dolphin) - such pins keep the absolute form and launch
// path-only.
QString to_game_relative_path(const std::filesystem::path &game_dir,
                              const QString &path) {
  if (path.isEmpty() || game_dir.empty() || !QFileInfo(path).isAbsolute())
    return path;
  std::error_code ec;
  auto canon_game = std::filesystem::weakly_canonical(game_dir, ec);
  auto base = (ec || canon_game.empty()) ? game_dir : canon_game;
  auto canon_full = std::filesystem::weakly_canonical(path.toStdString(), ec);
  if (ec || canon_full.empty())
    return path;
  auto rel = std::filesystem::relative(canon_full, base, ec);
  if (ec || rel.empty() || rel.begin()->string() == "..")
    return path;
  return QString::fromStdString(rel.generic_string());
}

} // anonymous namespace

ModListController::ModListController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {}

void ModListController::setup_mod_list(QVBoxLayout *left_layout) {
  w_->profile_bar_ = new ProfileBar(w_);
  left_layout->addWidget(w_->profile_bar_);

  connect(w_->profile_bar_, &ProfileBar::create_separator_clicked, this,
          [this]() { create_separator(); });
  connect(w_->profile_bar_, &ProfileBar::create_empty_mod_clicked, this,
          [this]() { create_empty_mod(); });

  connect(w_->profile_bar_, &ProfileBar::profile_changed, this,
          [this](const QString &profile) { switch_profile(profile); });
  connect(w_->profile_bar_, &ProfileBar::manage_profiles_requested, this,
          [this]() { open_profile_manager(); });

  connect(w_->profile_bar_, &ProfileBar::open_folder_requested, this,
          &ModListController::open_folder);
  connect(w_->profile_bar_, &ProfileBar::export_modlist_clicked, this,
          [this]() { export_modlist(); });
  connect(w_->profile_bar_, &ProfileBar::import_modlist_clicked, this,
          [this]() { import_modlist(); });

  w_->mod_model_ = new ModListModel(w_);
  w_->mod_view_ = new ModTableView(w_);
  w_->mod_model_->set_view(w_->mod_view_);
  w_->mod_view_->setModel(w_->mod_model_);

  // Highlight conflicting mods on selection + populate ConflictsTab
  connect(w_->mod_view_->selectionModel(), &QItemSelectionModel::currentChanged,
          this,
          [this](const QModelIndex &current, const QModelIndex & /*previous*/) {
            if (!current.isValid()) {
              w_->mod_model_->set_selected_mods({});
              auto *ct = w_->right_panel_->conflicts_tab();
              if (ct)
                ct->clear_content();
              return;
            }
            const auto &mods = w_->mod_model_->mods();
            if (current.row() >= 0 && current.row() < mods.size() &&
                !mods[current.row()].is_separator &&
                !mods[current.row()].is_overwrite) {
              auto &selected = mods[current.row()];
              w_->mod_model_->set_selected_mods({selected.id});

              // Push conflict data to the ConflictsTab
              auto *ct = w_->right_panel_->conflicts_tab();
              if (ct) {
                ct->show_conflicts(
                    selected.id, mods, w_->last_conflict_registry_,
                    w_->mod_model_->conflict_pairs(),
                    w_->mod_model_->is_conflict_order_reversed());
              }
            } else {
              w_->mod_model_->set_selected_mods({});
              auto *ct = w_->right_panel_->conflicts_tab();
              if (ct)
                ct->clear_content();
            }
          });

  // Mod selection -> highlight the mod's plugins in the plugins list
  // (union across multi-selection, MO2's highlightPlugins parity).
  connect(w_->mod_view_->selectionModel(),
          &QItemSelectionModel::selectionChanged, this,
          &ModListController::on_mod_selection_changed);

  // Alternating row colors follow the system palette's AlternateBase (no
  // custom bake: a setPalette() here would freeze the view and its children
  // against system Light<->Dark switches).

  // Sync checkbox toggles to filesystem (disable.it)
  connect(w_->mod_model_, &QAbstractItemModel::dataChanged, this,
          [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                 const QVector<int> &roles) {
            (void)bottomRight;
            if (roles.contains(Qt::CheckStateRole) &&
                topLeft.column() == ModListModel::Name) {
              auto id =
                  w_->mod_model_
                      ->data(topLeft.sibling(topLeft.row(), ModListModel::Name),
                             Qt::EditRole)
                      .toString();
              bool enabled =
                  w_->mod_model_->data(topLeft, Qt::CheckStateRole).toInt() ==
                  Qt::Checked;
              sync_mod_enable_state(id, enabled);

              // Update the mod-list counter (enabled / total)
              update_mod_count_label();
            }
          });

  // Counter follows add/remove/move/load (mod_list_changed fires once per
  // structural change; toggles are covered by the dataChanged handler above).
  connect(w_->mod_model_, &ModListModel::mod_list_changed, this,
          [this]() { update_mod_count_label(); });

  // Sync priority rewrites to metadata files after reorder; plugin discovery
  // for the Plugins tab follows any mod-list change (install/remove/toggle).
  connect(w_->mod_model_, &ModListModel::mod_list_changed, this, [this]() {
    sync_priorities();
    refresh_plugins_tab();
  });

  // Save order on every model change
  connect(w_->mod_model_, &ModListModel::mod_list_changed, this, [this]() {
    if (w_->loading_)
      return;
    save_order();
    sync_separator_ids();
    // Persist per-mod UI state (folded + parent_id) to the manager sidecar.
    // Covers fold toggles, nesting drops, parent renames (the model cascades
    // children's parent_id, this rewrites their sidecars) and deletes (the
    // model detaches children, this clears their sidecar parent_id).
    sync_mod_ui_state();
    apply_mod_filter();
  });

  // Inline rename (MO2 renameMod): the handler renames the folder on disk
  // and updates the row in place; on failure it reverts the editor.
  connect(w_->mod_model_, &ModListModel::rename_requested, this,
          [this](int row, const QString &name) { apply_rename(row, name); });

  // Fold/unfold on the Fold column ONLY (the dedicated arrow cell, left of
  // Name). Clicks anywhere else on the separator row - including the whole
  // Name cell - must not fold. A separator with no content to hide shows an
  // empty Fold cell and its click is a dead no-op too. With nesting enabled
  // a mod with children folds the same way (hides its subtree).
  connect(w_->mod_view_, &QTreeView::clicked, this,
          [this](const QModelIndex &idx) {
            if (!idx.isValid() || idx.column() != ModListModel::Fold)
              return;
            int row = idx.row();
            if (row < 0 || row >= w_->mod_model_->mods().size())
              return;
            if (!w_->mod_model_->has_content(row))
              return;

            const bool folded = w_->mod_model_->mods()[row].folded;
            w_->mod_model_->set_folded(row, !folded);
          });

  // Double-clicking the Overwrite row opens the shared info dialog (MO2's
  // default-action behavior for the Overwrite entry). Mod rows map the
  // clicked column to a Mod Info tab (MO2's modlistview.cpp double-click):
  // Version → Nexus, Flags → Conflicts, anything else → last-used tab.
  // Shift+Double-Click (MO2 parity) opens the mod's source page in the
  // default browser instead of the Mod Info dialog; rows without a
  // resolvable source (Overwrite, separators, game-native, unknown source)
  // do nothing.
  connect(w_->mod_view_, &QTreeView::doubleClicked, this,
          [this](const QModelIndex &idx) {
            if (!idx.isValid())
              return;
            int row = idx.row();
            if (row < 0 || row >= w_->mod_model_->mods().size())
              return;
            const auto &entry = w_->mod_model_->mods()[row];

            if (QApplication::keyboardModifiers() & Qt::ShiftModifier) {
              if (!entry.is_overwrite && !entry.is_separator &&
                  !entry.is_game_native) {
                auto src = source_visit_info(entry.source_type, entry.source_id,
                                             entry.source_page_url);
                if (!src.url.isEmpty())
                  QDesktopServices::openUrl(QUrl(src.url));
              }
              return;
            }

            if (entry.is_overwrite) {
              w_->overwrite_->show_overwrite_info_dialog();
              return;
            }
            if (entry.is_separator || entry.is_game_native)
              return;

            int tab = -1;
            switch (idx.column()) {
            case ModListModel::Conflicts:
              tab = static_cast<int>(ui::ModInfoTabId::Conflicts);
              break;
            case ModListModel::Flags:
              tab = static_cast<int>(ui::ModInfoTabId::Conflicts);
              break;
            case ModListModel::Category:
              tab = static_cast<int>(ui::ModInfoTabId::Categories);
              break;
            case ModListModel::Source:
            case ModListModel::SourceId:
            case ModListModel::Version:
              tab = static_cast<int>(ui::ModInfoTabId::Source);
              break;
            default:
              break; // last-used tab
            }
            on_data_mod_info(entry.id, tab);
          });

  // MO2 parity: Ctrl+Double-Click opens the OS file explorer at the mod's
  // folder (ModTableView::ctrl_double_clicked). Regular mods open their
  // folder in the instance mods dir; the Overwrite row opens the Overwrite
  // dir. Foreign/unmanaged (game-native) rows, separators and MERGED do
  // nothing.
  connect(w_->mod_view_, &ModTableView::ctrl_double_clicked, this,
          [this](const QModelIndex &idx) {
            if (!idx.isValid())
              return;
            int row = idx.row();
            if (row < 0 || row >= w_->mod_model_->mods().size())
              return;
            const auto &entry = w_->mod_model_->mods()[row];
            if (entry.is_separator || entry.is_merged || entry.is_game_native)
              return;

            std::filesystem::path folder;
            if (entry.is_overwrite) {
              folder = w_->overwrite_dir_path();
            } else {
              const auto mods_subpath =
                  w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                       "mods_subpath", "")
                                 : "";
              folder =
                  w_->resolve_mod_folder(entry.id.toStdString(), mods_subpath);
            }
            if (folder.empty())
              return;
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QString::fromStdString(folder.string())));
          });

  // Drag-and-drop archives onto the mod list to install manually
  connect(w_->mod_view_, &ModTableView::files_dropped, this,
          [this](const QStringList &paths) { import_archives(paths); });

  // Drag-and-drop files/folders out of the Overwrite info dialog onto a mod
  // row moves them into that mod (MO2's drop-to-mod).
  connect(w_->mod_view_, &ModTableView::overwrite_files_dropped, this,
          [this](const QStringList &paths, int mod_row) {
            w_->overwrite_->move_dropped_overwrite_files(paths, mod_row);
          });

  auto *mod_header = new ColumnToggleHeaderView(Qt::Horizontal, w_->mod_view_);
  mod_header->set_column_labels({"", "Name", "Conflicts", "Flags", "Category",
                                 "Source", "Source ID", "Version",
                                 "Installation", "Changed", "Priority"});
  mod_header->set_section_tooltips({
      tr("Fold or unfold w_ separator (hides or shows its contents)"),
      tr("Name of the mod"),
      tr("Win/loss state of file conflicts with other mods"),
      tr("Badges: hidden files, FOMOD saved, root override, invalid data"),
      tr("Primary category of the mod"),
      tr("Site the mod was downloaded from"),
      tr("Mod/file ID on the source site"),
      tr("Version of the mod (if available)"),
      tr("When the mod folder was created (install/replace time)"),
      tr("Last time the mod folder was modified"),
      tr("Install priority: the higher, the more it overwrites"),
  });
  w_->mod_view_->setHeader(mod_header);
  w_->mod_header_ = mod_header;

  mod_header->setStretchLastSection(false);
  mod_header->setSectionsMovable(true);
  // The Fold arrow column is pinned to the left edge: fixed 24px cell,
  // always visible, and re-snapped to visual index 0 if the user tries to
  // drag another column past it (the arrow must stay aligned to the edge).
  mod_header->setSectionResizeMode(ModListModel::Fold, QHeaderView::Fixed);
  mod_header->resizeSection(ModListModel::Fold, 24);
  mod_header->setSectionResizeMode(ModListModel::Name, QHeaderView::Stretch);
  for (int c = ModListModel::Conflicts; c < ModListModel::ColumnCount; ++c)
    mod_header->setSectionResizeMode(c, QHeaderView::Interactive);
  mod_header->resizeSection(ModListModel::Conflicts, 80);
  mod_header->resizeSection(ModListModel::Flags, 80);
  mod_header->resizeSection(ModListModel::Category, 120);
  mod_header->resizeSection(ModListModel::Source, 40);
  mod_header->resizeSection(ModListModel::SourceId, 70);
  mod_header->resizeSection(ModListModel::Version, 80);
  mod_header->resizeSection(ModListModel::Installation, 90);
  mod_header->resizeSection(ModListModel::Changed, 90);
  mod_header->resizeSection(ModListModel::Priority, 60);

  // The Name and Fold columns can never be hidden (the context menu shows
  // them checked + disabled). Persist user visibility toggles per instance;
  // the restore happens on scan finish when the instance name is known.
  mod_header->set_locked_section(ModListModel::Name);
  mod_header->set_locked_section(ModListModel::Fold);
  connect(
      mod_header, &ColumnToggleHeaderView::section_toggled, this,
      [this](int logical, bool hidden) {
        if (logical == ModListModel::Name || logical == ModListModel::Fold ||
            w_->current_instance_root_.empty())
          return;
        const auto key = QString::fromStdString(
            w_->current_instance_root_.filename().string());
        const auto stored = Settings::instance().modlist_hidden_columns(key);
        auto hidden_set = QSet<QString>(stored.cbegin(), stored.cend());
        const QString name = mod_column_name(logical);
        if (hidden)
          hidden_set.insert(name);
        else
          hidden_set.remove(name);
        Settings::instance().set_modlist_hidden_columns(key,
                                                        hidden_set.values());
      });

  // Non-negotiable: the Fold arrow column stays at the left edge. Other
  // columns stay draggable, but any drag that displaces Fold from visual
  // index 0 is reverted. The recursive sectionMoved (from moveSection) sees
  // Fold already at 0 and returns, so w_ cannot loop.
  connect(mod_header, &QHeaderView::sectionMoved, this,
          [mod_header](int, int, int) {
            const int foldLogical = ModListModel::Fold;
            if (mod_header->visualIndex(foldLogical) == 0)
              return;
            mod_header->moveSection(foldLogical, 0);
          });

  // Category filter panel (MO2 parity): hidden by default; the << / >> toggle
  // in the filter bar shows/hides it. Placed on the LEFT side of the mod list
  // using a horizontal splitter (standard mod-manager UX pattern).
  w_->category_filter_panel_ = new CategoryFilterPanel(w_);
  w_->category_filter_panel_->hide();
  w_->category_filter_panel_->setMinimumWidth(160);

  // MO2-style digital counter above the mod list showing the enabled mod
  // count only, right-aligned. QLCDNumber gives the seven-segment "lcd"
  // look (same as the plugins-tab counter); colors come from the palette.
  w_->mod_count_enabled_ = new QLCDNumber(w_);
  w_->mod_count_enabled_->setObjectName("mo2CounterLabel");
  w_->mod_count_enabled_->setDigitCount(4);
  w_->mod_count_enabled_->display(0);

  auto *count_row = new QHBoxLayout;
  count_row->setContentsMargins(4, 2, 4, 2);
  count_row->addStretch(1); // push the counter to the right edge
  count_row->addWidget(w_->mod_count_enabled_);

  auto *mod_list_pane = new QWidget(w_);
  auto *pane_layout = new QVBoxLayout(mod_list_pane);
  pane_layout->setContentsMargins(0, 0, 0, 0);
  pane_layout->setSpacing(2);
  pane_layout->addLayout(count_row);
  pane_layout->addWidget(w_->mod_view_, 1);

  auto *mod_splitter = new QSplitter(Qt::Horizontal, w_);
  mod_splitter->addWidget(w_->category_filter_panel_);
  mod_splitter->addWidget(mod_list_pane);
  mod_splitter->setStretchFactor(0, 0);
  mod_splitter->setStretchFactor(1, 1);
  mod_splitter->setSizes({220, 800});
  left_layout->addWidget(mod_splitter, 1);

  w_->filter_bar_ = new ModFilterBar(w_);
  left_layout->addWidget(w_->filter_bar_);

  connect(w_->filter_bar_, &ModFilterBar::filter_changed, this,
          [this]() { apply_mod_filter(); });
  connect(w_->filter_bar_, &ModFilterBar::group_changed, this,
          [this]() { apply_mod_filter(); });
  connect(w_->filter_bar_, &ModFilterBar::category_panel_toggled,
          w_->category_filter_panel_, &QWidget::setVisible);
  connect(w_->category_filter_panel_,
          &CategoryFilterPanel::category_filter_changed, this,
          [this]() { apply_mod_filter(); });
  connect(w_->category_filter_panel_,
          &CategoryFilterPanel::edit_categories_clicked, this, [this]() {
            // MO2 parity: the Categories dialog edits the global category
            // registry (engine::CategoryFactory) and persists it to the
            // instance's categories.dat. On accept the filter tree is rebuilt
            // (the checked set is reset — removed categories can no longer be
            // checked) and the mod filter is re-applied.
            ui::CategoriesDialog dlg(w_->current_instance_root_, w_);
            if (dlg.exec() == QDialog::Accepted) {
              w_->category_filter_panel_->rebuild();
              apply_mod_filter();
            }
          });
}

void ModListController::refresh_profiles() {
  const auto profiles_dir = w_->profiles_dir_path();
  if (profiles_dir.empty())
    return;

  // Ensure the Default profile exists and is initialized. A missing Default
  // directory (or one that is not a directory at all) is bootstrapped with
  // defaults; a directory that exists but is missing required files is
  // repaired in place below.
  {
    const auto default_dir = profiles_dir / "Default";
    if (!std::filesystem::is_directory(default_dir)) {
      if (std::filesystem::exists(default_dir))
        std::filesystem::remove_all(default_dir);
      engine::profile::create_fresh_profile(profiles_dir, "Default", nullptr);
    }
  }

  // Auto-repair every profile directory: create any missing required files
  // (settings.ini, modlist.txt, archives.txt) with sensible defaults instead
  // of failing silently later. repair() never touches existing files, so a
  // partially-populated profile keeps its mod list and settings.
  for (const auto &name : engine::profile::list_profiles(profiles_dir)) {
    engine::profile::Profile profile(profiles_dir / name);
    const auto generated = profile.repair();
    if (!generated.empty()) {
      engine::Logger::instance().info(
          "Repaired profile \"" + name + "\": created " +
          std::to_string(generated.size()) + " missing file(s)");
    }
  }

  QStringList names;
  for (const auto &name : engine::profile::list_profiles(profiles_dir))
    names << QString::fromStdString(name);

  // Resolve the profile to select: the current profile when it still exists,
  // else the saved default profile, else the first profile. The fallback
  // updates current_profile_name_ so the rest of the app (title, plugin DB,
  // mod list) agrees with the selector.
  QString current = QString::fromStdString(w_->current_profile_name_);
  if (!names.contains(current)) {
    const QString def = Settings::instance().default_profile();
    if (!def.isEmpty() && names.contains(def)) {
      current = def;
    } else if (!names.isEmpty()) {
      current = names.first();
    } else {
      current.clear();
    }
    if (!current.isEmpty()) {
      w_->current_profile_name_ = current.toStdString();
      w_->update_title();
    }
  }
  w_->profile_bar_->set_profiles(names, current);
}

void ModListController::open_profile_manager() {
  const auto profiles_dir = w_->profiles_dir_path();
  if (profiles_dir.empty())
    return;

  ui::ProfileManagerDialog dlg(
      profiles_dir, QString::fromStdString(w_->current_profile_name_),
      Settings::instance().default_profile(), w_);
  connect(&dlg, &ProfileManagerDialog::profiles_changed, this,
          [this]() { refresh_profiles(); });
  connect(&dlg, &ProfileManagerDialog::default_profile_changed, this,
          [](const QString &name) {
            Settings::instance().set_default_profile(name);
          });

  if (dlg.exec() == QDialog::Accepted) {
    const QString selected = dlg.selected_profile();
    if (!selected.isEmpty() &&
        selected != QString::fromStdString(w_->current_profile_name_)) {
      switch_profile(selected);
    }
  }
  refresh_profiles();
}

void ModListController::switch_profile(const QString &profile) {
  if (profile.isEmpty() ||
      profile == QString::fromStdString(w_->current_profile_name_))
    return;

  const auto profiles_dir = w_->profiles_dir_path();
  if (profiles_dir.empty())
    return;

  // Live state snapshot for save_current_profile (MO2's saveCurrentProfile):
  // the in-memory mod list is the source of truth for what's installed.
  engine::profile::ProfileSaveState state;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.is_separator || m.is_overwrite || m.is_merged)
      continue;
    state.known_mods.push_back(m.id.toStdString());
    if (m.is_game_native)
      state.foreign_mods.push_back(m.id.toStdString());
  }

  // The active profile's engine model is the source of truth for the current
  // profile's modlist state (toggles persist through it via
  // sync_mod_enable_state). Ensure it exists and is loaded before the
  // switcher saves it — a fresh Profile has an empty in-memory list and
  // would flush an empty modlist.txt over the real per-profile state.
  if (!w_->active_profile_ ||
      w_->active_profile_->name() != w_->current_profile_name_) {
    w_->active_profile_ = std::make_unique<engine::profile::Profile>(
        profiles_dir / w_->current_profile_name_);
    w_->active_profile_->refresh_mod_status(state.known_mods,
                                            state.foreign_mods);
  }

  engine::profile::ProfileSwitchCallbacks callbacks;
  // Re-scan the mods directory and rebuild the mod list (MO2's
  // refreshDirectoryStructure). The scan reads the mods dir; the new
  // profile's modlist.txt state is restored by the switcher into the
  // engine Profile before this callback runs, and on_mod_scan_finished
  // applies it to the UI model via apply_profile_mod_states().
  callbacks.refresh_directory_structure = [this]() { load_mods_from_game(); };
  // Reload the Plugins tab from the new profile's plugin files (MO2's
  // refreshLists). refresh_plugins_tab() applies load_profile itself.
  callbacks.refresh_plugin_list = [this]() { refresh_plugins_tab(); };
  // The Archives tab is not yet wired to profile data; the switcher skips
  // empty callbacks.
  callbacks.refresh_bsa_list = {};
  // Archive invalidation is persisted per profile (settings.ini); applying
  // it to the game INI is a deploy concern outside this ticket.
  callbacks.set_archive_invalidation = {};

  auto result = engine::profile::switch_profile(
      profiles_dir, profile.toStdString(), w_->active_profile_.get(), state,
      &w_->plugins_db_, callbacks);
  if (!result.success) {
    QMessageBox::warning(
        w_, tr("Switch Profile"),
        tr("Could not switch to profile \"%1\": %2")
            .arg(profile, QString::fromStdString(result.error)));
    return;
  }
  if (!result.changed)
    return;

  // Adopt the new profile's engine model (modlist.txt state already restored
  // by the switcher). The scan launched by the refresh callback lands after
  // this, so on_mod_scan_finished applies the new profile's state.
  w_->active_profile_ = std::move(result.profile);

  // Delayed disable capability: queue the FULL desired state of the new
  // profile so the next Run reconciles the on-disk sentinels with the profile.
  // Idempotent (writing an existing sentinel / removing an absent one is a
  // no-op) and self-healing — it also satisfies the "multiple profile swaps
  // accumulate the final state" criterion without a delta against the old
  // on-disk state. The full state supersedes any earlier queued toggles.
  if (w_->knowledge_ && !w_->current_game_id_.empty() &&
      engine::delayed_disable_for(*w_->knowledge_, w_->current_game_id_)) {
    w_->deferred_disable_queue_.clear();
    for (const auto &pm : w_->active_profile_->mods()) {
      w_->deferred_disable_queue_.push_back({pm.mod_id, pm.enabled});
    }
    engine::Logger::instance().debug(
        "Delayed disable: queued full profile state for '" +
        w_->active_profile_->name() + "' (" +
        std::to_string(w_->deferred_disable_queue_.size()) + " mods)");
  }

  // Use the canonical profile name (the actual directory name the switcher
  // resolved case-insensitively) so the selector and the active-profile
  // guard agree.
  w_->current_profile_name_ = w_->active_profile_->name();
  w_->update_title();
  refresh_profiles();
}

void ModListController::update_status_bar_for_game() {
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // Download sources: comma-separated list (e.g. "Nexus,Steam")
  auto sources_csv =
      w_->knowledge_->get(w_->current_game_id_, "download_sources", "");
  QStringList sources;
  if (!sources_csv.empty()) {
    for (const auto &part :
         QString::fromStdString(sources_csv).split(',', Qt::SkipEmptyParts)) {
      sources.append(part.trimmed());
    }
  }
  w_->status_bar_->set_sources(sources);
}

void ModListController::update_mod_count_label() {
  if (!w_->mod_count_enabled_)
    return;
  // Same row filter as the old status-bar counter: separators and the
  // Overwrite pseudo-row are not mods.
  int enabled = 0;
  int total = 0;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.is_separator || m.is_overwrite)
      continue;
    ++total;
    if (m.enabled)
      ++enabled;
  }
  w_->mod_count_enabled_->display(enabled);

  // MO2-style breakdown tooltip: enabled / total mods.
  w_->mod_count_enabled_->setToolTip(
      tr("<table cellspacing=\"6\">"
         "<tr><th>%1</th><th>%2</th><th>%3</th></tr>"
         "<tr><td>All mods:</td><td align=\"right\">%4</td>"
         "<td align=\"right\">%5</td></tr>"
         "<tr><td>Active:</td><td align=\"right\">%6</td>"
         "<td align=\"right\">%7</td></tr>"
         "</table>")
          .arg(tr("Type"), tr("Active"), tr("Total"))
          .arg(enabled)
          .arg(total)
          .arg(enabled)
          .arg(total));
}

void ModListController::sync_mod_enable_state(const QString &mod_id,
                                              bool enabled) {
  if (w_->loading_)
    return;
  if (!w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty())
    return;

  // Separators don't have enable/disable on disk
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.id == mod_id && m.is_separator)
      return;
  }

  // Game is running - queue the change instead of writing to disk
  if (w_->running_process_pid_ > 0) {
    // Remove any existing pending toggle for w_ mod (latest wins)
    auto it = std::remove_if(
        w_->pending_changes_.begin(), w_->pending_changes_.end(),
        [&](const PendingToggle &pt) { return pt.mod_id == mod_id; });
    w_->pending_changes_.erase(it, w_->pending_changes_.end());
    w_->pending_changes_.push_back({mod_id, enabled});
    w_->queue_->update_queue_label();
    engine::Logger::instance().debug(
        "Queued toggle for " + mod_id.toStdString() + " -> " +
        (enabled ? "enabled" : "disabled") + " (" +
        std::to_string(w_->pending_changes_.size()) + " pending)");
    return;
  }

  // Delayed disable capability (plugin-declared hook, e.g. Isaac's Direct
  // deploy mode): skip the immediate on-disk sentinel write. The toggle is
  // recorded in the deferred queue (latest-wins per mod_id) and applied at the
  // next Run, when launch_with_executable flushes the queue before the deploy
  // worker starts. The profile modlist.txt is still updated now — it is the
  // per-profile source of truth; the on-disk sentinel is reconciled at launch.
  if (engine::delayed_disable_for(*w_->knowledge_, w_->current_game_id_)) {
    auto it = std::remove_if(w_->deferred_disable_queue_.begin(),
                             w_->deferred_disable_queue_.end(),
                             [&](const DeferredDisable &dd) {
                               return dd.mod_id == mod_id.toStdString();
                             });
    w_->deferred_disable_queue_.erase(it, w_->deferred_disable_queue_.end());
    w_->deferred_disable_queue_.push_back({mod_id.toStdString(), enabled});

    // Persist the toggle to the active profile's modlist.txt (the per-profile
    // source of truth for enabled state).
    if (w_->active_profile_) {
      w_->active_profile_->set_mod_enabled(mod_id.toStdString(), enabled);
    }

    engine::Logger::instance().debug(
        "Delayed disable: queued toggle for " + mod_id.toStdString() + " -> " +
        (enabled ? "enabled" : "disabled") + " (" +
        std::to_string(w_->deferred_disable_queue_.size()) + " deferred)");

    // P1.3 event bus: mirror MO2 onModStateChanged. Fires on the UI thread
    // after the profile state change; a plugin handler must not block.
    engine::EventBus::instance().dispatch(engine::events::kModStateChanged,
                                          engine::json_obj({
                                              {"mod", mod_id.toStdString()},
                                              {"enabled", enabled ? "1" : "0"},
                                          }));
    return;
  }

  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  if (mods_subpath.empty())
    return;

  auto mod_folder = w_->resolve_mod_folder(mod_id.toStdString(), mods_subpath);

  if (enabled) {
    (void)engine::ModScanner::enable_mod(*w_->knowledge_, w_->current_game_id_,
                                         mod_folder);
  } else {
    (void)engine::ModScanner::disable_mod(*w_->knowledge_, w_->current_game_id_,
                                          mod_folder);
  }

  // Persist the toggle to the active profile's modlist.txt (the per-profile
  // source of truth for enabled state; the disable.it write above is the
  // on-disk deploy mechanism). Without this, modlist.txt never records the
  // toggle and every profile shares the same global state.
  if (w_->active_profile_) {
    w_->active_profile_->set_mod_enabled(mod_id.toStdString(), enabled);
  }

  // P1.3 event bus: mirror MO2 onModStateChanged. Fires on the UI thread
  // after the on-disk state change; a plugin handler must not block.
  engine::EventBus::instance().dispatch(engine::events::kModStateChanged,
                                        engine::json_obj({
                                            {"mod", mod_id.toStdString()},
                                            {"enabled", enabled ? "1" : "0"},
                                        }));
}

void ModListController::sync_priorities() {
  if (w_->loading_)
    return;
  // Instance-owned persistence: priorities go to the meta sidecars (and the
  // game-native metadata write below self-guards on mods_subpath), so no
  // game dir is required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // Game is running - skip disk write; full order saved at flush
  if (w_->running_process_pid_ > 0) {
    return;
  }

  auto meta_dir = w_->meta_dir_path();
  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");

  auto &mods = w_->mod_model_->mods();
  for (int i = 0; i < mods.size(); ++i) {
    // Persist priority to meta.ini for every row (Overwrite, separators, mods)
    if (!meta_dir.empty()) {
      auto meta = engine::ModMeta::load(meta_dir, mods[i].id.toStdString());
      int old_priority = meta.priority();
      if (old_priority != i) {
        meta.set_priority(i);
        meta.save(meta_dir, mods[i].id.toStdString());
        // P1.3 event bus: mirror MO2 onModMoved — fired only for real
        // moves, on the UI thread, after the priority persisted.
        if (old_priority >= 0 && !mods[i].is_overwrite &&
            !mods[i].is_separator) {
          engine::EventBus::instance().dispatch(
              engine::events::kModMoved,
              engine::json_obj({
                  {"mod", mods[i].id.toStdString()},
                  {"from", std::to_string(old_priority)},
                  {"to", std::to_string(i)},
              }));
        }
      }
    }
    // Write game-native priority - resolve actual mod folder location.
    // Only games that encode priority into mod-folder metadata (Isaac's
    // NNN prefix in metadata.xml, read by the game itself) get a folder
    // write; MO2-style games persist priority in the meta dir sidecar
    // above and read load order from their plugins.txt / order encoding.
    if (!mods[i].is_overwrite && !mods[i].is_separator &&
        !mods_subpath.empty()) {
      auto metadata_file = w_->knowledge_->get(w_->current_game_id_,
                                               "metadata_file", "meta.ini");
      if (!metadata_file.empty() && metadata_file != "meta.ini") {
        auto mod_folder =
            w_->resolve_mod_folder(mods[i].id.toStdString(), mods_subpath);
        (void)engine::ModScanner::set_priority(
            *w_->knowledge_, w_->current_game_id_, mod_folder, i);
      }
    }
  }
}

void ModListController::sort_mods() {
  auto &trace = engine::TraceRecorder::instance();
  trace.begin_flow("sort");

  auto *provider =
      engine::SortRegistry::instance().get_provider(w_->current_game_id_);
  if (!provider) {
    engine::Logger::instance().warn("No sort provider registered for game: " +
                                    w_->current_game_id_);
    trace.end_flow("sort", false,
                   "No sort provider for " + w_->current_game_id_);
    return;
  }

  // Build mod info list from current model
  trace.begin_stage("sort", "Gather mod info");
  std::vector<engine::SortModInfo> mod_infos;
  for (const auto &mod : w_->mod_model_->mods()) {
    if (mod.is_separator || mod.is_overwrite || mod.id == kOverwriteModId ||
        mod.is_game_native)
      continue;

    engine::SortModInfo info;
    info.folder_name = mod.id.toStdString();
    info.display_name = mod.name.toStdString();

    // Extract workshop ID from folder name
    auto workshop_pattern =
        w_->knowledge_->get(w_->current_game_id_, "workshop_id_pattern", "");
    if (!workshop_pattern.empty()) {
      try {
        std::regex re(workshop_pattern);
        std::smatch m;
        if (std::regex_search(info.folder_name, m, re)) {
          info.workshop_id = std::stoll(m[1].str());
        }
      } catch (...) {
      }
    }

    mod_infos.push_back(info);
  }
  trace.end_stage("sort", true,
                  std::to_string(mod_infos.size()) + " mod(s) collected");

  // Call the sort provider
  trace.begin_stage("sort", "Run sort provider");
  auto result = provider->sort(mod_infos);
  trace.end_stage("sort", true, std::string("Provider: ") + provider->name());

  // Apply the sorted order to the model
  trace.begin_stage("sort", "Apply order");
  w_->loading_ = true;

  // Build a map of folder_name -> ModEntry
  QMap<QString, ui::ModEntry> mod_map;
  for (const auto &mod : w_->mod_model_->mods()) {
    mod_map[mod.id] = mod;
  }

  // Create new ordered list: game-native (unmanaged) mods first (fixed top
  // band, in declared order), then the provider's sorted user mods.
  QVector<ui::ModEntry> new_order;
  for (const auto &mod : w_->mod_model_->mods())
    if (mod.is_game_native)
      new_order.append(mod);

  // Add mods in sorted order
  for (const auto &folder : result.sorted_folders) {
    auto qfolder = QString::fromStdString(folder);
    if (mod_map.contains(qfolder)) {
      new_order.append(mod_map[qfolder]);
    }
  }

  // Add any mods not in the sorted result (shouldn't happen, but be safe)
  for (const auto &mod : w_->mod_model_->mods()) {
    if (!mod.is_overwrite && !mod.is_game_native &&
        std::find(result.sorted_folders.begin(), result.sorted_folders.end(),
                  mod.id.toStdString()) == result.sorted_folders.end()) {
      new_order.append(mod);
    }
  }

  // Overwrite always at bottom
  for (const auto &mod : w_->mod_model_->mods()) {
    if (mod.is_overwrite) {
      new_order.append(mod);
      break;
    }
  }

  // Apply the new order
  w_->mod_model_->reset_with_order(new_order);

  // Apply tags from sort result
  for (const auto &tag_info : result.tags) {
    QVector<ui::ModTag> tags;
    tags.append({QString::fromStdString(tag_info.type),
                 QString::fromStdString(tag_info.message)});
    w_->mod_model_->set_tags(QString::fromStdString(tag_info.folder_name),
                             tags);
  }

  w_->loading_ = false;
  trace.end_stage("sort", true, "New order applied");

  trace.begin_stage("sort", "Save order");
  save_order();
  trace.end_stage("sort", true, "Order persisted");

  engine::Logger::instance().debug("Mods sorted by " +
                                   std::string(provider->name()));
  trace.end_flow("sort", true);
}

void ModListController::load_mods_from_game() {
  // No game_dir requirement (Workspace-wk8): with an empty game dir the
  // scan runs against the instance mods dir instead (ModScanWorker handles
  // the swap). An instance root is still mandatory - there is nothing to
  // scan without one.
  if (!w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_instance_root_.empty())
    return;

  // Ensure the active profile's engine model exists and points at the
  // current profile (first load / instance switch). The scan result
  // converges it with the mods dir; toggles persist through it. On a
  // profile switch the switcher replaces active_profile_ with the new
  // profile before the scan lands, so this guard only fires on first load
  // / instance switch (the name still matches during the switch's refresh
  // callback, which runs before current_profile_name_ is updated).
  if (!w_->current_profile_name_.empty() &&
      (!w_->active_profile_ ||
       w_->active_profile_->name() != w_->current_profile_name_)) {
    w_->active_profile_ = std::make_unique<engine::profile::Profile>(
        w_->profiles_dir_path() / w_->current_profile_name_);
    w_->active_profile_->refresh_mod_status({}, {});
  }

  w_->loading_ = true;

  // Configure conflict order from plugin hook (before adding mods)
  auto conflict_reversed =
      w_->knowledge_->get(w_->current_game_id_, "conflict_order_reversed", "");
  w_->mod_model_->set_conflict_order_reversed(conflict_reversed == "true");

  // Any in-flight scan belongs to an older state (a refresh supersedes the
  // previous refresh; set_game_info bumps on instance switches too): bump
  // the generation so its result is dropped when it lands. The scan itself
  // runs on ModScanThread — the main thread does no directory walking here
  // (P8.2, THREADING.md §3.5/§3.6).
  w_->mod_scan_generation_ = w_->mod_scan_generation_ + 1;

  ui::ModScanRequest request = build_mod_scan_request();
  if (!w_->mod_scan_thread_) {
    w_->mod_scan_thread_ = new ui::ModScanThread(w_);
    connect(w_->mod_scan_thread_->worker(), &ui::ModScanWorker::finished, this,
            &ModListController::on_mod_scan_finished, Qt::UniqueConnection);
  }
  w_->mod_scan_thread_->start(std::move(request), w_->mod_scan_generation_);
}

ui::ModScanRequest ModListController::build_mod_scan_request() {
  ui::ModScanRequest request;
  request.knowledge =
      *w_->knowledge_; // snapshot — read-only after plugin registration
  request.game_id = w_->current_game_id_;
  request.game_dir = w_->current_game_dir_;
  // Game-native mods dir override (Workspace-6up): instance.toml
  // "game_mods_dir" when set, else the worker derives game_dir/mods_subpath.
  request.game_mods_dir = w_->current_game_mods_dir();
  request.instance_root = w_->current_instance_root_;
  request.mods_dir = w_->mods_dir_path();
  request.meta_dir = w_->meta_dir_path();
  // Direct-symlink deploys persist their ledger at the instance root; the
  // stray-plugin scan consults it so deployed .esp files are not synthesized
  // as unmanaged rows. Empty in portable mode (no instance -> no deploy).
  if (!w_->current_instance_root_.empty()) {
    request.ledger_file = engine::deploy_config_for(
                              w_->current_instance_root_, w_->current_game_dir_,
                              *w_->knowledge_, w_->current_game_id_)
                              .ledger_file;
  }
  return request;
}

void ModListController::launch_plugin_db_preload() {
  // Discard any preload state left over from a previous instance and bump
  // the generation so a still-running load's result is dropped when it lands.
  w_->plugin_db_generation_ = w_->plugin_db_generation_ + 1;
  w_->preload_pending_ = false;
  w_->preloaded_plugin_db_.reset();

  if (!w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty())
    return;
  const auto game_native =
      engine::native_plugins_csv(*w_->knowledge_, w_->current_game_id_);
  if (game_native.empty())
    return; // game declares no plugin hooks — nothing to preload

  ui::PluginDbLoadRequest request;
  request.game_dir = w_->current_game_dir_;
  request.mods_dir = w_->mods_dir_path();
  request.meta_dir = w_->meta_dir_path();
  request.disable_mechanism =
      engine::disable_mechanism_for(*w_->knowledge_, w_->current_game_id_);
  request.game_native = game_native;

  w_->preload_pending_ = true;
  w_->preloaded_plugin_db_game_dir_ = w_->current_game_dir_;
  if (!w_->plugin_db_load_thread_) {
    w_->plugin_db_load_thread_ = new ui::PluginDbLoadThread(w_);
    connect(w_->plugin_db_load_thread_->worker(),
            &ui::PluginDbLoadWorker::finished, this,
            &ModListController::on_plugin_db_preloaded, Qt::UniqueConnection);
  }
  w_->plugin_db_load_thread_->start(std::move(request),
                                    w_->plugin_db_generation_);
}

void ModListController::on_plugin_db_preloaded(engine::PluginDatabase db,
                                               quint64 generation) {
  if (generation != w_->plugin_db_generation_ || !w_->preload_pending_) {
    // Superseded by an instance switch or already consumed/superseded by a
    // synchronous fallback read — never adopt stale disk state.
    return;
  }
  w_->preloaded_plugin_db_ = std::move(db);
}

bool ModListController::adopt_preloaded_plugin_db() {
  if (!w_->preload_pending_ || !w_->preloaded_plugin_db_)
    return false;
  // The preload belongs to a different instance's game dir (paranoia; the
  // generation check above already covers switches) — refuse it.
  if (w_->preloaded_plugin_db_game_dir_ != w_->current_game_dir_)
    return false;
  w_->plugins_db_ = std::move(*w_->preloaded_plugin_db_);
  w_->preloaded_plugin_db_.reset();
  w_->preload_pending_ = false;
  return true;
}

void ModListController::on_mod_scan_finished(ui::ModScanResult result,
                                             quint64 generation) {
  if (generation != w_->mod_scan_generation_) {
    // Superseded (a newer refresh or instance switch launched another
    // scan): never apply a stale mod list. w_->loading_ stays true — the newer
    // scan's result clears it when it lands.
    return;
  }

  auto &scanned = result.scanned;

  // Apply the per-instance column visibility (defaults on first run; Name is
  // always forced visible). The instance root is known now, so per-instance
  // settings resolve correctly across instance switches.
  restore_mod_column_visibility();

  // Clear all existing mods (including game-native) - needed for instance
  // switching
  w_->mod_model_->remove_all_mods();

  // MERGED pseudo-mod is game-dependent (Isaac only); turn the flag on/off
  // before anything adds rows, so switching Isaac <-> Skyrim adds/removes it.
  auto uses_merged =
      w_->knowledge_->get(w_->current_game_id_, "uses_merged", "");
  w_->mod_model_->set_uses_merged(uses_merged == "true");

  // Filter out MERGED pseudo-mod folder from scan results
  scanned.erase(std::remove_if(scanned.begin(), scanned.end(),
                               [](const engine::ScannedMod &m) {
                                 return m.folder_name == "MERGED";
                               }),
                scanned.end());

  // Add scanned mods before Overwrite (Overwrite stays last)
  for (const auto &mod : scanned) {
    auto id = QString::fromStdString(mod.folder_name);
    auto name = QString::fromStdString(mod.display_name);
    auto ver = QString::fromStdString(mod.version);
    if (mod.is_separator) {
      auto color = QString::fromStdString(mod.separator_color);
      w_->mod_model_->add_separator(id, name, color);
    } else {
      w_->mod_model_->add_mod(id, name, ver, mod.priority, mod.is_game_native,
                              mod.install_time, mod.changed_time);
      if (mod.is_fomod) {
        w_->mod_model_->set_fomod(id, true);
      }
      if (mod.root_override) {
        w_->mod_model_->set_root_override(id, true);
      }
      if (mod.invalid_data) {
        w_->mod_model_->set_invalid_data(id, true);
      }
      if (mod.no_metadata) {
        w_->mod_model_->set_no_metadata(id, true);
      }
      if (!mod.enabled) {
        w_->mod_model_->toggle_mod(id);
      }
    }
  }

  // Apply the active profile's modlist.txt enabled state (the per-profile
  // source of truth) on top of the scan result (which reflects the global
  // on-disk disable.it marker). Without this, every profile would show the
  // same enabled/disabled state regardless of which profile is active.
  apply_profile_mod_states();

  // Load/create meta for each mod. (The one-time MO2 meta.ini import ran on
  // the worker thread as part of the scan; w_ only reads sidecars.)
  load_meta_for_mods();

  // Read persisted priority from meta.ini for ALL entries (including
  // separators, Overwrite). Mods without a persisted priority (e.g. freshly
  // installed) get the bottom of the user band - MO2's rule: a new mod gets the
  // highest regular priority, just above the pinned Overwrite/MERGED rows.
  // set_priority() only writes the field; load_order() applies it.
  {
    auto meta_dir = w_->meta_dir_path();
    if (!meta_dir.empty()) {
      // Game-native (unmanaged) mods own the top band, but a separator
      // may sit ABOVE it (its fold hides the native mods): that
      // separator keeps its persisted priority and the band shifts down
      // past it. Natives fill the remaining top slots in declared order.
      int native_priority = 0;
      std::set<int> sep_priorities;
      for (const auto &m : w_->mod_model_->mods()) {
        if (!m.is_separator)
          continue;
        auto sep_meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
        int sp = sep_meta.priority();
        if (sp >= 0)
          sep_priorities.insert(sp);
      }
      for (const auto &m : w_->mod_model_->mods()) {
        if (!m.is_game_native)
          continue;
        while (sep_priorities.count(native_priority))
          ++native_priority;
        w_->mod_model_->set_priority(m.id, native_priority++);
      }

      // Non-pinned rows (natives + user mods) span priorities 0..regular-1;
      // a user mod without a persisted priority gets the highest one - just
      // above the pinned Overwrite/MERGED block (MO2's new-mod rule).
      int regular_rows = 0;
      for (const auto &m : w_->mod_model_->mods()) {
        if (!m.is_overwrite && !m.is_merged)
          ++regular_rows;
      }
      int bottom_priority = std::max(0, regular_rows - 1);
      for (const auto &m : w_->mod_model_->mods()) {
        if (m.is_game_native)
          continue;
        auto meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
        int p = meta.priority();
        if (p < 0)
          p = bottom_priority;
        w_->mod_model_->set_priority(m.id, p);
      }
    }
  }

  // Ensure MERGED pseudo-mod is present (after loading scanned mods, before
  // sorting)
  w_->mod_model_->ensure_merged_present();

  w_->loading_ = false;

  // Apply the per-instance nesting gate before restoring order/folds/links,
  // so load_order's fold restore and render decisions see the right mode.
  w_->settings_->apply_nesting_setting();

  // Sort by priority to restore saved order
  load_order();

  // Persist priorities to {modname}.ini - including the ones just assigned to
  // freshly installed mods above - so the order survives restarts. This also
  // fires when load_order() produced no reorder (already-correct order).
  sync_priorities();

  // Sync separator IDs for new mods or first-load (no instance.toml yet)
  sync_separator_ids();

  // Tell the model where Overwrite lives so it can colour the entry
  if (!w_->current_instance_root_.empty()) {
    auto overwrite_dir = w_->overwrite_dir_path();
    w_->mod_model_->set_overwrite_path(
        QString::fromStdString(overwrite_dir.string()));
  }

  engine::Logger::instance().debug("Loaded " + std::to_string(scanned.size()) +
                                   " mods for " + w_->current_game_name_);

  // Update the mod-list counter after the game switch / refresh
  update_mod_count_label();

  // Compute conflict stats for all mods (debounced entry; the scan runs off
  // the main thread per P8.1).
  recompute_conflicts();

  // Populate the Plugins tab from the (now loaded) mod list.
  refresh_plugins_tab();
}

void ModListController::apply_profile_mod_states() {
  if (!w_->active_profile_)
    return;

  // Flush any pending toggle first so the re-read below sees the in-memory
  // state (the delayed writer may still hold an unflushed change — e.g. the
  // user toggled a mod and hit Refresh within the ~5s debounce window).
  w_->active_profile_->write_modlist_now();

  // Converge the profile with the scanned mods dir: mods not yet in the
  // profile (freshly installed) are appended enabled by default and
  // persisted (delayed) — MO2's refreshModStatus behavior.
  std::vector<std::string> known_mods;
  std::vector<std::string> foreign_mods;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.is_separator || m.is_overwrite || m.is_merged)
      continue;
    known_mods.push_back(m.id.toStdString());
    if (m.is_game_native)
      foreign_mods.push_back(m.id.toStdString());
  }
  w_->active_profile_->refresh_mod_status(known_mods, foreign_mods);

  // Apply the profile's enabled state (the per-profile source of truth) on
  // top of the scan result (which reflects the global on-disk disable.it
  // marker). Runs while w_->loading_ is true, so the model-change handlers
  // (save_order, sync_mod_enable_state) early-return and no disk write is
  // triggered from here.
  for (const auto &pm : w_->active_profile_->mods()) {
    w_->mod_model_->set_mod_enabled(QString::fromStdString(pm.mod_id),
                                    pm.enabled);
  }

  // Re-apply the on-disk disable.it marker: the profile is the per-profile
  // source of truth, but the global disable sentinel always wins. Without
  // this, a new instance (empty profile) would show all mods enabled even
  // if they have a disable.it file.  Check both the instance mods dir and
  // the game's native mods dir (e.g. Isaac workshop mods live in
  // game_dir/mods/, not <instance>/mods/).
  const std::string disable_file =
      engine::disable_mechanism_for(*w_->knowledge_, w_->current_game_id_);
  if (!disable_file.empty()) {
    const auto inst_mods = w_->mods_dir_path();
    const auto game_mods = w_->current_game_mods_dir();
    for (const auto &pm : w_->active_profile_->mods()) {
      std::error_code ec;
      const bool disabled =
          std::filesystem::exists(inst_mods / pm.mod_id / disable_file, ec) ||
          std::filesystem::exists(game_mods / pm.mod_id / disable_file, ec);
      if (disabled) {
        w_->mod_model_->set_mod_enabled(QString::fromStdString(pm.mod_id),
                                        false);
      }
    }
  }
}

void ModListController::add_installed_mod(const std::string &folder_name) {
  if (folder_name.empty())
    return;
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // Scan just the one folder the install produced - not the whole mods dir.
  auto scanned = engine::ModScanner::scan_folder(
      *w_->knowledge_, w_->current_game_id_, w_->mods_dir_path(), folder_name,
      w_->current_instance_root_.empty()
          ? std::vector<std::filesystem::path>{}
          : std::vector<std::filesystem::path>{w_->current_instance_root_});
  if (scanned.empty())
    return;

  const auto &mod = scanned.front();

  // If the row already exists (Merge/Replace into an existing folder, or a
  // reinstall), don't add a duplicate - the files changed, so the conflict
  // and Data refreshes below still run.
  const auto id = QString::fromStdString(mod.folder_name);
  bool exists = false;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.id == id) {
      exists = true;
      break;
    }
  }
  if (!exists) {
    auto name = QString::fromStdString(mod.display_name);
    auto ver = QString::fromStdString(mod.version);
    if (mod.is_separator) {
      w_->mod_model_->add_separator(
          id, name, QString::fromStdString(mod.separator_color));
    } else {
      w_->mod_model_->add_mod(id, name, ver, mod.priority, mod.is_game_native,
                              mod.install_time, mod.changed_time);
      if (mod.is_fomod)
        w_->mod_model_->set_fomod(id, true);
      if (mod.root_override)
        w_->mod_model_->set_root_override(id, true);
      if (mod.invalid_data)
        w_->mod_model_->set_invalid_data(id, true);
      if (mod.no_metadata)
        w_->mod_model_->set_no_metadata(id, true);
      if (!mod.enabled)
        w_->mod_model_->toggle_mod(id);
    }
    // Persist the freshly assigned priority (MO2 bottom-of-band) and
    // separator ids, mirroring the full-load tail.
    sync_priorities();
    sync_separator_ids();
  } else {
    // Replace/merge into an existing folder changed its birth/write time,
    // so the Installation/Changed cells must follow (single-row refresh).
    w_->mod_model_->set_timestamps(id, mod.install_time, mod.changed_time);
  }

  // Files changed regardless of whether the row is new: conflicts, Data tab
  // and Plugins tab all reflect the installed content. Unlike the full
  // recompute, the Data tab is refreshed incrementally: the engine's token
  // cache already limits the registry rescan to the new mod's files, and
  // apply_mod() merges only that mod's rows into the existing tree. The scan
  // runs off the main thread (P8.1); the incremental apply runs once the
  // freshly computed registry includes w_ mod.
  request_conflict_scan([this, folder_name]() {
    if (auto *dt = w_->right_panel_->data_tab()) {
      std::string mods_subpath;
      std::string deploy_prefix;
      bool deploy_include_mod_id = false;
      if (w_->knowledge_) {
        mods_subpath =
            w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
        deploy_prefix =
            w_->knowledge_->get(w_->current_game_id_, "deploy_prefix", "Data");
        deploy_include_mod_id =
            w_->knowledge_->get(w_->current_game_id_, "deploy_include_mod_id",
                                "false") == "true";
      }
      dt->apply_mod(
          w_->last_conflict_registry_, folder_name, w_->mod_model_->mods(),
          w_->mod_model_->is_conflict_order_reversed(), w_->mods_dir_path(),
          w_->current_game_mods_dir(), w_->current_game_dir_, mods_subpath,
          deploy_prefix, deploy_include_mod_id);
    }
  });
  refresh_plugins_tab();

  // Update the mod-list counter after the install
  update_mod_count_label();

  engine::Logger::instance().debug("Added installed mod row: " + folder_name);
}

void ModListController::load_meta_for_mods() {
  auto meta_dir = w_->meta_dir_path();
  if (meta_dir.empty())
    return;

  // Workshop ID pattern - used to detect Steam Workshop mods from folder names
  auto workshop_pattern =
      w_->knowledge_->get(w_->current_game_id_, "workshop_id_pattern", "");

  // Per-instance category DB (MO2's categories.dat/nexuscatmap.dat), loaded
  // once per call for the Category column. Same resolution the Categories tab
  // uses: [General] category CSV primary first, else the Nexus mapping.
  engine::Categories cats;
  if (!w_->current_instance_root_.empty())
    cats = engine::Categories::load(w_->current_instance_root_);

  auto mods = w_->mod_model_->mods();
  for (int i = 0; i < mods.size(); ++i) {
    const auto &mod = mods[i];
    if (mod.is_separator || mod.is_overwrite)
      continue;

    auto folder_name = mod.id.toStdString();
    if (folder_name.empty())
      continue;

    // Load existing meta (or empty if no file yet)
    auto meta = engine::ModMeta::load(meta_dir, folder_name);

    if (!meta.has_section("General") && !meta.has_section("GameModManager")) {
      // No meta file exists - create a default one (already at
      // CURRENT_META_VERSION). Detect Steam Workshop mods from the folder
      // name pattern so the source column shows Steam, not "manual".
      std::string source_type = "manual";
      std::string source_id;
      if (!workshop_pattern.empty()) {
        try {
          std::regex pattern(workshop_pattern);
          std::smatch m;
          if (std::regex_search(folder_name, m, pattern) && m.size() > 1) {
            source_type = "steam";
            source_id = m[1].str();
          }
        } catch (...) {
        }
      }
      meta = engine::ModMeta::from_default(folder_name, source_type, source_id);
      meta.save(meta_dir, folder_name);

    } else {
      // Existing meta - check if upgrade is needed
      bool upgraded = false;
      int mv = meta.meta_version();

      if (mv < engine::ModMeta::CURRENT_META_VERSION) {
        // v0 → v1: detect Steam Workshop mods and write
        // [SteamWorkshop]workshop_id
        if (mv < 1 && !workshop_pattern.empty()) {
          try {
            std::regex pattern(workshop_pattern);
            std::smatch m;
            if (std::regex_search(folder_name, m, pattern) && m.size() > 1) {
              if (meta.get("SteamWorkshop", "workshop_id").empty()) {
                meta.set("SteamWorkshop", "workshop_id", m[1].str());
              }
              // Register Steam source so the source column shows Steam
              if (meta.source_type().empty() ||
                  meta.source_type() == "manual") {
                meta.set("GameModManager", "source_type", "steam");
                meta.set("GameModManager", "source_id", m[1].str());
              }
            }
          } catch (...) {
          }
        }

        // Future v1 → v2 upgrades go here

        meta.set_meta_version(engine::ModMeta::CURRENT_META_VERSION);
        upgraded = true;
      }

      if (upgraded) {
        meta.save(meta_dir, folder_name);
      }
    }

    // Update ModEntry with source info
    auto st = meta.source_type();
    auto sid = meta.source_id();
    if (!st.empty()) {
      w_->mod_model_->set_source_info(
          mod.id, QString::fromStdString(st), QString::fromStdString(sid),
          QString::fromStdString(meta.source_page_url()));
    }

    // Category column: same resolution as the Categories tab — [General]
    // "category" CSV primary first, else the Nexus category mapping. Both
    // names come from the per-instance category DB.
    QString category_name;
    const auto csv = QString::fromStdString(meta.get("General", "category"));
    int primary = 0;
    const auto parts = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
      primary = parts.first().toInt();
    if (primary <= 0) {
      const int nexus_id =
          QString::fromStdString(meta.get("Nexusmods", "nexuscategory"))
              .toInt();
      if (nexus_id > 0) {
        if (const auto *cat = cats.category_for_nexus(nexus_id))
          primary = cat->id;
      }
    }
    if (primary > 0) {
      if (const auto *cat = cats.find(primary))
        category_name = QString::fromStdString(cat->name);
    }
    if (!category_name.isEmpty())
      w_->mod_model_->set_category(mod.id, category_name);

    // Category ids for the filter panel: the full [General] "category" CSV
    // (primary first). The Nexus fallback (no CSV) contributes the mapped
    // internal id so the panel can filter those mods too.
    QVector<int> category_ids;
    for (const auto &p : parts) {
      bool ok = false;
      const int cid = p.toInt(&ok);
      if (ok && cid > 0)
        category_ids.append(cid);
    }
    if (category_ids.isEmpty() && primary > 0)
      category_ids.append(primary);

    // Workshop tag → category fallback: when [General] category is empty,
    // check [SteamWorkshop] tags and map them via the workshop_tag_categories
    // hook. This auto-assigns categories to mods downloaded from Steam Workshop
    // based on their declared tags.
    if (category_ids.isEmpty()) {
      auto tags_csv = QString::fromStdString(meta.get("SteamWorkshop", "tags"));
      if (!tags_csv.isEmpty()) {
        auto tag_mapping = w_->knowledge_->get(w_->current_game_id_,
                                               "workshop_tag_categories", "");
        if (!tag_mapping.empty()) {
          auto tags = tags_csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
          try {
            auto mapping = nlohmann::json::parse(tag_mapping);
            if (mapping.is_object()) {
              for (const auto &tag : tags) {
                auto lower_tag = tag.toLower().toStdString();
                auto it = mapping.find(lower_tag);
                if (it != mapping.end() && it->is_number_integer()) {
                  int cat_id = it->get<int>();
                  if (!category_ids.contains(cat_id))
                    category_ids.append(cat_id);
                }
              }
              // Persist mapped categories to meta.ini so future scans
              // don't need to re-map.
              if (!category_ids.isEmpty()) {
                QStringList id_strs;
                for (int cid : category_ids)
                  id_strs << QString::number(cid);
                meta.set("General", "category",
                         id_strs.join(QLatin1Char(',')).toStdString());
                meta.save(meta_dir, folder_name);
                if (primary <= 0 && !category_ids.isEmpty())
                  primary = category_ids.first();
              }
            }
          } catch (...) {
          }
        }
      }
    }

    if (!category_ids.isEmpty())
      w_->mod_model_->set_category_ids(mod.id, category_ids);

    // Update ModEntry with separator info from meta.ini
    auto sep_id = meta.separator_id();
    if (!sep_id.empty()) {
      w_->mod_model_->set_separator_id(mod.id, QString::fromStdString(sep_id));
    }
  }
}

void ModListController::restore_mod_column_visibility() {
  if (!w_->mod_header_ || w_->current_instance_root_.empty())
    return;

  const auto key =
      QString::fromStdString(w_->current_instance_root_.filename().string());
  const auto stored = Settings::instance().modlist_hidden_columns(key);
  const auto hidden_set = QSet<QString>(stored.cbegin(), stored.cend());

  for (int c = ModListModel::Name; c < ModListModel::ColumnCount; ++c) {
    const QString name = mod_column_name(c);
    if (name.isEmpty())
      continue;
    // Name is hard-locked visible; everything else follows the stored set.
    const bool hidden =
        !w_->mod_header_->is_locked(c) && hidden_set.contains(name);
    w_->mod_header_->setSectionHidden(c, hidden);
  }
}

void ModListController::recompute_conflicts() {
  if (!w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty())
    return;
  // Debounce: coalesce rapid toggle/reorder/refresh requests into one scan.
  if (w_->conflict_debounce_timer_->isActive())
    w_->conflict_debounce_timer_->stop();
  w_->conflict_debounce_timer_->start();
}

void ModListController::start_conflict_scan() {
  request_conflict_scan([this]() { refresh_data_tab(); });
}

void ModListController::request_conflict_scan(std::function<void()> follow_up) {
  std::vector<std::function<void()>> batch;
  if (follow_up)
    batch.push_back(std::move(follow_up));
  launch_conflict_scan_batch(std::move(batch));
}

void ModListController::launch_conflict_scan_batch(
    std::vector<std::function<void()>> follow_ups) {
  if (w_->conflict_scan_running_) {
    // One scan is already in flight. Queue a fresh one (snapshot is rebuilt
    // when it launches, so it reflects the newest state); its follow-ups
    // run after that newer scan lands.
    w_->conflict_scan_pending_ = true;
    for (auto &f : follow_ups)
      if (f)
        w_->conflict_scan_pending_follow_ups_.push_back(std::move(f));
    return;
  }

  // An immediate scan supersedes any pending debounce.
  w_->conflict_debounce_timer_->stop();

  ui::ConflictScanRequest request = build_conflict_scan_request();
  if (request.mod_infos.empty()) {
    // Nothing enabled to scan: mirror the old compute_conflict_state()
    // early-return — the registry is cleared, follow-ups still run so the
    // Data tab (and any incremental install apply) empties.
    w_->last_conflict_registry_.clear();
    for (auto &f : follow_ups)
      if (f)
        f();
    return;
  }

  w_->conflict_scan_running_ = true;
  w_->conflict_scan_active_follow_ups_ = std::move(follow_ups);
  w_->conflict_scan_generation_ = w_->conflict_scan_generation_ + 1;
  if (!w_->conflict_scan_thread_) {
    w_->conflict_scan_thread_ = new ui::ConflictScanThread(w_);
    connect(w_->conflict_scan_thread_->worker(),
            &ui::ConflictScanWorker::finished, this,
            &ModListController::on_conflict_scan_finished,
            Qt::UniqueConnection);
  }
  w_->conflict_scan_thread_->start(std::move(request),
                                   w_->conflict_scan_generation_);
}

ui::ConflictScanRequest ModListController::build_conflict_scan_request() {
  ui::ConflictScanRequest request;
  request.mods_dir = w_->mods_dir_path();
  request.extra_mods_dir = w_->current_game_mods_dir();
  request.cache_path = w_->conflict_cache_path_;

  // Read per-game config from knowledge hooks (needed before mod_infos for
  // overwrite priority)
  request.extensions_csv =
      w_->knowledge_->get(w_->current_game_id_, "conflict_extensions", "");
  request.ignored_csv =
      w_->knowledge_->get(w_->current_game_id_, "ignored_files", "");
  // Mod folders carry per-mod metadata files the manager itself writes
  // (meta.ini) or that the game reads (metadata.xml / disable marker).
  // Every mod folder has them, so exclude them from conflict counting.
  auto metadata_file =
      w_->knowledge_->get(w_->current_game_id_, "metadata_file", "meta.ini");
  auto disable_file =
      engine::disable_mechanism_for(*w_->knowledge_, w_->current_game_id_);
  for (const auto *f : {&metadata_file, &disable_file}) {
    if (f->empty())
      continue;
    if (request.ignored_csv.find(*f) != std::string::npos)
      continue;
    if (!request.ignored_csv.empty())
      request.ignored_csv += ",";
    request.ignored_csv += *f;
  }
  request.conflict_reversed =
      w_->knowledge_->get(w_->current_game_id_, "conflict_order_reversed",
                          "") == "true";
  request.scan_dirs_csv =
      w_->knowledge_->get(w_->current_game_id_, "conflict_scan_dirs", "");

  // Collect mod info - only enabled mods affect the game
  for (const auto &mod : w_->mod_model_->mods()) {
    if (mod.is_separator)
      continue;
    if (!mod.enabled && !mod.is_overwrite && !mod.is_merged)
      continue;
    if (mod.is_overwrite) {
      request.mod_infos.emplace_back(mod.id.toStdString(),
                                     request.conflict_reversed ? -1 : 999999);
      continue;
    }
    if (mod.is_merged) {
      request.mod_infos.emplace_back(mod.id.toStdString(),
                                     request.conflict_reversed ? 0 : 999998);
      continue;
    }
    request.mod_infos.emplace_back(mod.id.toStdString(), mod.priority);
  }

  request.invalidate = std::move(w_->conflict_invalidate_pending_);
  w_->conflict_invalidate_pending_.clear();
  return request;
}

void ModListController::on_conflict_scan_finished(ui::ConflictScanResult result,
                                                  quint64 generation) {
  if (generation != w_->conflict_scan_generation_) {
    // Superseded (e.g. an instance switch bumped the generation while w_
    // scan was in flight): never apply a stale result, but the worker is
    // idle now, so any queued requests for the newer state must still run.
    w_->conflict_scan_running_ = false;
    if (w_->conflict_scan_pending_) {
      w_->conflict_scan_pending_ = false;
      auto batch = std::move(w_->conflict_scan_pending_follow_ups_);
      w_->conflict_scan_pending_follow_ups_.clear();
      launch_conflict_scan_batch(std::move(batch));
    }
    return;
  }
  w_->conflict_scan_running_ = false;

  apply_conflict_results(result);

  auto follow_ups = std::move(w_->conflict_scan_active_follow_ups_);
  w_->conflict_scan_active_follow_ups_.clear();
  for (auto &f : follow_ups)
    if (f)
      f();
  reload_open_modinfo_dialog();

  // A request arrived mid-scan: launch the queued fresh scan now.
  if (w_->conflict_scan_pending_) {
    w_->conflict_scan_pending_ = false;
    auto batch = std::move(w_->conflict_scan_pending_follow_ups_);
    w_->conflict_scan_pending_follow_ups_.clear();
    launch_conflict_scan_batch(std::move(batch));
  }
}

void ModListController::apply_conflict_results(
    const ui::ConflictScanResult &result) {
  // Push per-mod stats into the model
  for (const auto &[folder_name, cs] : result.stats) {
    w_->mod_model_->set_conflict_stats(QString::fromStdString(folder_name),
                                       cs.wins, cs.losses);
  }
  // Zero out any stale stats for disabled mods (not fed to the engine)
  for (const auto &mod : w_->mod_model_->mods()) {
    if (!mod.enabled && !mod.is_overwrite && !mod.is_merged &&
        !mod.is_separator)
      w_->mod_model_->set_conflict_stats(mod.id, 0, 0);
  }

  // "Redundant" mods: every file they provide is won by a higher-priority
  // owner, so nothing the mod provides actually takes effect.
  const auto &registry = result.registry;
  const bool conflict_reversed = result.conflict_reversed;
  std::unordered_set<std::string> owns_files;
  std::unordered_set<std::string> wins_a_file;
  for (const auto &[path, owners] : registry) {
    if (owners.empty())
      continue;
    for (const auto &[owner, _] : owners)
      owns_files.insert(owner);
    const auto &winner =
        conflict_reversed ? *std::min_element(owners.begin(), owners.end(),
                                              [](const auto &a, const auto &b) {
                                                return a.second < b.second;
                                              })
                          : *std::max_element(owners.begin(), owners.end(),
                                              [](const auto &a, const auto &b) {
                                                return a.second < b.second;
                                              });
    wins_a_file.insert(winner.first);
  }
  for (const auto &mod : w_->mod_model_->mods()) {
    bool redundant = owns_files.count(mod.id.toStdString()) > 0 &&
                     wins_a_file.count(mod.id.toStdString()) == 0;
    w_->mod_model_->set_conflict_redundant(mod.id, redundant);
  }

  // Build pairwise data from the file registry
  QMap<QString, ui::ConflictPairs> pairs;
  auto add_win = [&](const QString &winner, const QString &loser) {
    auto &w = pairs[winner];
    if (!w.wins_against.contains(loser))
      w.wins_against.append(loser);
  };
  auto add_loss = [&](const QString &loser, const QString &winner) {
    auto &l = pairs[loser];
    if (!l.loses_to.contains(winner))
      l.loses_to.append(winner);
  };
  for (const auto &[path, owners] : registry) {
    if (owners.size() <= 1)
      continue;
    auto winner_it = conflict_reversed
                         ? std::min_element(owners.begin(), owners.end(),
                                            [](const auto &a, const auto &b) {
                                              return a.second < b.second;
                                            })
                         : std::max_element(owners.begin(), owners.end(),
                                            [](const auto &a, const auto &b) {
                                              return a.second < b.second;
                                            });
    const auto &winner = *winner_it;
    auto wq = QString::fromStdString(winner.first);
    for (const auto &[loser_name, _] : owners) {
      if (loser_name == winner.first)
        continue;
      auto lq = QString::fromStdString(loser_name);
      add_win(wq, lq);
      add_loss(lq, wq);
    }
  }
  w_->mod_model_->set_conflict_pairs(pairs);
  w_->last_conflict_registry_ = result.registry;
}

void ModListController::reload_open_modinfo_dialog() {
  if (!w_->modinfo_dialog_)
    return;
  const QString id = w_->modinfo_dialog_->current_mod_id();
  if (id.isEmpty())
    return;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.id == id) {
      w_->modinfo_dialog_->reload_current(build_mod_info_data(m));
      return;
    }
  }
}

void ModListController::refresh_data_tab() {
  auto *dt = w_->right_panel_->data_tab();
  if (!dt)
    return;

  if (w_->last_conflict_registry_.empty() || w_->current_game_id_.empty()) {
    dt->clear_content();
    return;
  }

  auto mods_dir = w_->mods_dir_path();
  auto game_mods_dir = w_->current_game_mods_dir();

  std::string mods_subpath;
  std::string deploy_prefix;
  bool deploy_include_mod_id = false;
  if (w_->knowledge_) {
    mods_subpath =
        w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
    deploy_prefix =
        w_->knowledge_->get(w_->current_game_id_, "deploy_prefix", "Data");
    deploy_include_mod_id =
        w_->knowledge_->get(w_->current_game_id_, "deploy_include_mod_id",
                            "false") == "true";
  }

  dt->show_data(w_->last_conflict_registry_, w_->mod_model_->mods(),
                w_->mod_model_->is_conflict_order_reversed(), mods_dir,
                game_mods_dir, w_->current_game_dir_, mods_subpath,
                deploy_prefix, deploy_include_mod_id);
}

void ModListController::wire_data_tab() {
  auto *dt = w_->right_panel_->data_tab();
  if (!dt || dt == w_->data_tab_widget_)
    return;
  connect(dt, &ui::DataTab::open_requested, this,
          &ModListController::on_data_open);
  connect(dt, &ui::DataTab::execute_requested, this,
          &ModListController::on_data_execute);
  connect(dt, &ui::DataTab::preview_requested, this,
          &ModListController::on_data_preview);
  connect(dt, &ui::DataTab::add_executable_requested, this,
          &ModListController::on_data_add_executable);
  connect(dt, &ui::DataTab::open_mod_info_requested, this,
          [this](const QString &mod_id) { on_data_mod_info(mod_id); });
  connect(dt, &ui::DataTab::hide_requested, this,
          &ModListController::on_data_hide);
  connect(dt, &ui::DataTab::refresh_requested, this,
          &ModListController::recompute_conflicts);
  w_->data_tab_widget_ = dt;
}

void ModListController::on_data_open(const QString &file_path) {
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file_path))) {
    QMessageBox::warning(w_, tr("Open"),
                         tr("Failed to open:\n%1").arg(file_path));
  }
}

void ModListController::on_data_execute(const QString &file_path,
                                        bool is_windows_exe,
                                        const QString &vfs_path) {
  // Every execute goes through the standard overlay-launch chain (the same
  // one the game and toolbar shortcuts use): it deploys enabled mods into
  // .gmm_staging and launches inside the overlay, so the tool sees the
  // merged view of every installed mod (MO2's plain Execute). The launchable
  // target is the merged Data-relative path; legacy absolute entries fall
  // back to their physical path, which the overlay also resolves.
  QString target = file_path;
  if (!vfs_path.isEmpty() && !w_->current_game_dir_.empty()) {
    target = QString::fromStdString(
        (std::filesystem::weakly_canonical(w_->current_game_dir_) /
         vfs_path.toStdString())
            .string());
  }
  (void)is_windows_exe; // launch_with_executable derives it from the extension
  w_->launch_->launch_with_executable(target, {});
}

void ModListController::on_data_preview(const QString &file_path,
                                        const QStringList &provider_paths,
                                        const QStringList &provider_names) {
  if (!w_->preview_window_)
    w_->preview_window_ = new ui::preview::PreviewWindow(w_);
  w_->preview_window_->set_game_id(w_->current_game_id_);
  w_->preview_window_->show_file(file_path, provider_paths, provider_names);
  w_->preview_window_->show();
  w_->preview_window_->raise();
  w_->preview_window_->activateWindow();
}

void ModListController::on_data_add_executable(const QString &file_path,
                                               const QString &default_name,
                                               const QString &physical_path) {
  auto *ec = w_->right_panel_->exec_controls();
  if (!ec)
    return;

  // file_path is the merged-view (deploy-relative) path emitted by the Data
  // tab - stored verbatim. populate_executables / launch resolve it against
  // w_->current_game_dir_, where the overlay mount makes it reachable.

  bool ok = false;
  const QString name =
      QInputDialog::getText(w_, tr("Add as Executable"), tr("Name:"),
                            QLineEdit::Normal, default_name, &ok);
  if (!ok || name.trimmed().isEmpty())
    return;

  // Icon comes from the physical winning copy (DataRealPathRole) - the merged
  // path may not exist on disk until the first deploy w_ session. The
  // extraction is cached by basename so the entry keeps its icon across
  // restarts even with an empty staging dir.
  QIcon icon;
  if (!physical_path.isEmpty())
    icon = ui::extractExeIcon(physical_path, w_->cache_thumbnails_dir_path());
  ec->add_executable(name, file_path, icon);
  w_->launch_->save_executables();
}

ui::ModInfoData ModListController::build_mod_info_data(const ModEntry &mod) {
  ui::ModInfoData data;
  data.id = mod.id;
  data.name = mod.name;
  data.version = mod.version;
  data.color = mod.separator_color;
  data.enabled = mod.enabled;
  data.is_separator = mod.is_separator;
  data.is_overwrite = mod.is_overwrite;
  data.is_game_native = mod.is_game_native;
  data.is_merged = mod.is_merged;
  data.priority = mod.priority;
  data.conflict_wins = mod.conflict_wins;
  data.conflict_losses = mod.conflict_losses;
  data.conflict_reversed = w_->mod_model_->is_conflict_order_reversed();

  data.source_type = mod.source_type;
  data.source_id = mod.source_id;
  // From the plugin identity, not a knowledge hook (there is none named
  // "nexus_domain") - drives the Source-tab Visit-on-Nexus URL AND the
  // Nexus Refresh API call (games/{domain}/mods/{id}.json).
  data.nexus_domain = current_nexus_domain();

  // Sources the current game supports (download_sources knowledge, display
  // names like "Nexus") — gates which sub-tabs the Source tab shows.
  const auto sources_csv =
      w_->knowledge_
          ? w_->knowledge_->get(w_->current_game_id_, "download_sources", "")
          : "";
  for (const auto &part :
       QString::fromStdString(sources_csv).split(',', Qt::SkipEmptyParts))
    data.supported_sources.append(part.trimmed());

  const auto mods_subpath =
      w_->knowledge_
          ? w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "")
          : "";
  data.data_subpath = QString::fromStdString(mods_subpath);

  std::filesystem::path mod_folder;
  if (mod.is_overwrite)
    mod_folder = w_->overwrite_dir_path();
  else
    mod_folder = w_->resolve_mod_folder(mod.id.toStdString(), mods_subpath);
  data.mod_dir = QDir(QString::fromStdString(mod_folder.string()));
  data.instance_root =
      QString::fromStdString(w_->current_instance_root_.string());

  // Conflicts touching w_ mod (registry paths are mod-dir-relative, i.e.
  // what ConflictEngine walked with w_ mod folder as the root).
  for (const auto &[path, owners] : w_->last_conflict_registry_) {
    bool is_owner = false;
    for (const auto &[owner, _] : owners)
      if (owner == mod.id.toStdString()) {
        is_owner = true;
        break;
      }
    if (!is_owner)
      continue;
    ui::ModInfoData::Owners owner_list;
    owner_list.reserve(owners.size());
    for (const auto &[owner, prio] : owners)
      owner_list.emplace_back(QString::fromStdString(owner), prio);
    data.conflicts.emplace_back(QString::fromStdString(path),
                                std::move(owner_list));
  }

  // Persistence: GMM's canonical sidecar meta file (the same one the rest of
  // MainWindow reads/writes). Not the MO2-visible mods/<id>/meta.ini - tabs
  // use GMM-canonical keys ([Nexusmods] etc.) and rewriting an MO2-format
  // file with them would corrupt MO2 compatibility for imported mods.
  const auto meta_dir = w_->meta_dir_path();
  data.load_meta = [meta_dir, mod_id = mod.id]() {
    return engine::ModMeta::load(meta_dir, mod_id.toStdString());
  };
  data.save_meta = [meta_dir, mod_id = mod.id](const engine::ModMeta &meta) {
    return meta.save(meta_dir, mod_id.toStdString());
  };

  // Actions wired to MainWindow.
  const QString mod_dir_str = QString::fromStdString(mod_folder.string());
  data.open_explorer = [mod_dir_str]() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(mod_dir_str));
  };
  data.open_file = [](const QString &path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  };
  data.open_url = [](const QString &url) {
    QDesktopServices::openUrl(QUrl(url));
  };
  data.hide_file = [](const QString &abs, bool hide) {
    const std::filesystem::path p(abs.toStdString());
    return hide ? engine::hide_file(p) : engine::unhide_file(p);
  };
  data.set_mod_color = [this, mod_id = mod.id](const QColor &c) {
    if (c.isValid())
      w_->mod_model_->set_mod_color(mod_id, c);
    else
      w_->mod_model_->clear_mod_color(mod_id);
  };
  // Recompute is debounced + async now (P8.1); the dialog is reloaded with
  // the fresh conflict data from reload_open_modinfo_dialog() once the scan
  // lands, so no eager reload with stale data here.
  data.refresh_conflicts = [this]() { recompute_conflicts(); };
  data.delete_mod = [this, mod_id = mod.id, mods_subpath]() -> bool {
    // The mods dir is INSTANCE-owned (Workspace-tnj): physical removal needs
    // mods_dir_path(), not the game dir.
    if (!mods_subpath.empty() && !w_->mods_dir_path().empty()) {
      auto mod_folder = w_->mods_dir_path() / mod_id.toStdString();
      if (!engine::remove_path(mod_folder)) {
        engine::Logger::instance().error(
            "Failed to move mod folder to trash: " + mod_folder.string());
      }
    }
    w_->mod_model_->remove_mod(mod_id);
    // P1.3 event bus: mirror MO2 onModRemoved.
    engine::EventBus::instance().dispatch(
        engine::events::kModRemoved,
        engine::json_obj({{"mod", mod_id.toStdString()}}));
    return true;
  };

  // Live Nexus lookup for the Nexus tab's Refresh button.
  const QString domain = data.nexus_domain;
  const QString src_id = mod.source_id;
  data.fetch_nexus_info = [domain, src_id]() {
    auto *provider = dynamic_cast<engine::NexusProvider *>(
        engine::SourceRegistry::instance().provider_for("nexus"));
    if (!provider || domain.isEmpty() || src_id.isEmpty())
      return engine::ModInfoResult{};
    return provider->fetch_mod_info(domain.toStdString(), src_id.toStdString());
  };

  return data;
}

void ModListController::on_data_mod_info(const QString &mod_id,
                                         int initial_tab) {
  ui::ModInfoData mod_data;
  bool found = false;
  for (const auto &mod : w_->mod_model_->mods()) {
    if (mod.id == mod_id) {
      mod_data = build_mod_info_data(mod);
      found = true;
      break;
    }
  }
  if (!found)
    return;

  std::vector<std::pair<QString, bool>> nav_list;
  nav_list.reserve(w_->mod_model_->mods().size());
  for (const auto &mod : w_->mod_model_->mods()) {
    nav_list.emplace_back(mod.id, mod.is_separator);
  }

  ui::ModInfoDialog dlg(std::move(mod_data), std::move(nav_list),
                        static_cast<ui::ModInfoTabId>(initial_tab), w_);
  dlg.set_data_builder([this](const QString &id) -> ui::ModInfoData {
    for (const auto &mod : w_->mod_model_->mods()) {
      if (mod.id == id)
        return build_mod_info_data(mod);
    }
    return {};
  });
  w_->modinfo_dialog_ = &dlg;
  dlg.exec();
  w_->modinfo_dialog_.clear();
}

void ModListController::on_data_hide(const QString &file_path,
                                     const QString &mod_id, bool hide) {
  const auto p = std::filesystem::path(file_path.toStdString());
  const bool ok = hide ? engine::hide_file(p) : engine::unhide_file(p);
  if (!ok) {
    QMessageBox::warning(
        w_, tr("Hide File"),
        tr("Failed to %1 the file.").arg(hide ? tr("hide") : tr("un-hide")));
    return;
  }
  // The rename happens inside a subdirectory (e.g. Data/...), which does NOT
  // change the mod root's quick token - the conflict cache would keep serving
  // the pre-rename file list and the tab would show the old name as a normal
  // file (with no real path, so no file menu). Drop the owning mod's cached
  // entry so the next scan re-scans it and surfaces the hidden/un-hidden
  // state. The invalidation is applied by the scan worker (before it reads
  // the cache), so it is only ever touched on the worker thread.
  w_->conflict_invalidate_pending_.insert(mod_id.toStdString());
  recompute_conflicts();
}

void ModListController::refresh_plugins_tab() {
  if (w_->loading_)
    return;
  auto *pt = w_->right_panel_->plugins_tab();
  if (!pt || !w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty()) {
    w_->plugins_tab_widget_ = nullptr;
    w_->plugin_owner_index_.clear();
    w_->plugin_row_by_name_.clear();
    return; // game without plugin support (or no tab yet)
  }
  if (pt != w_->plugins_tab_widget_) { // tab was recreated on game switch
    connect(pt, &ui::PluginsTab::toggle_requested, this,
            &ModListController::on_plugin_toggle);
    connect(pt, &ui::PluginsTab::reorder_requested, this,
            &ModListController::on_plugin_reorder);
    connect(pt, &ui::PluginsTab::lock_requested, this,
            &ModListController::on_plugin_lock);
    connect(pt, &ui::PluginsTab::refresh_requested, this, [this]() {
      refresh_plugins_tab();
      // set_plugins() rebuilds the rows and clears row-hidden states;
      // re-apply the active text filter so rows and counter stay
      // consistent with the filter box.
      if (w_->right_panel_)
        w_->right_panel_->reapply_current_filter();
    });
    connect(pt->table(), &QTableWidget::itemSelectionChanged, this,
            &ModListController::on_plugin_selection_changed);
    w_->plugins_tab_widget_ = pt;
  }

  const auto game_native =
      engine::native_plugins_csv(*w_->knowledge_, w_->current_game_id_);
  if (game_native
          .empty()) { // tab exists but the module declares no plugin hooks
    w_->plugins_db_ = engine::PluginDatabase{};
    w_->plugin_owner_index_.clear();
    w_->plugin_row_by_name_.clear();
    pt->set_plugins({});
    return;
  }

  // T6: when the concurrently-preloaded DB (launch_plugin_db_preload) is
  // ready, adopt it and skip the synchronous disk read entirely. Otherwise
  // fall back to it — and drop the pending preload so a late-landing result
  // can't clobber the fresher synchronous read.
  bool adopted = adopt_preloaded_plugin_db();
  if (!adopted && w_->preload_pending_) {
    w_->preload_pending_ = false;
    w_->preloaded_plugin_db_.reset();
  }
  if (!adopted) {
    const auto disable_mechanism =
        engine::disable_mechanism_for(*w_->knowledge_, w_->current_game_id_);
    w_->plugins_db_.refresh(w_->current_game_dir_, w_->mods_dir_path(),
                            w_->meta_dir_path(), disable_mechanism,
                            game_native);
    w_->plugins_db_.load_creation_club(
        w_->current_game_dir_,
        engine::creation_club_file_for(*w_->knowledge_, w_->current_game_id_));
    w_->plugins_db_.sort_load_order();
  }

  // A persisted profile is the source of truth once it exists; only a first
  // run (no profile yet) enables everything and writes it.
  const auto profiles_dir = w_->profiles_dir_path();
  bool applied = false;
  if (!profiles_dir.empty()) {
    bool repaired = false;
    applied = w_->plugins_db_.load_profile(
        profiles_dir, w_->current_profile_name_, &repaired);
    if (repaired) // core plugins were found below user ones - persist the heal
      w_->plugins_db_.save_profile(profiles_dir, w_->current_profile_name_);
  }
  if (!applied) {
    w_->plugins_db_.set_all_enabled();
    w_->plugins_db_.set_missing_masters();
    if (!profiles_dir.empty())
      w_->plugins_db_.save_profile(profiles_dir, w_->current_profile_name_);
  }
  w_->plugins_db_.generate_mod_indexes();
  if (w_->plugin_loader_) // plugin-supplied diagnostics land in the tooltip
    w_->plugin_loader_->collect_diagnostics(w_->current_game_id_,
                                            w_->plugins_db_);
  pt->set_plugins(w_->plugins_db_.plugins());
  rebuild_plugin_highlight_index();
  // Rows and the selection indexes were rebuilt; re-apply any highlights the
  // user still has active.
  on_mod_selection_changed();
  on_plugin_selection_changed();

  // P1.3 event bus: mirror MO2 onRefreshed (plugin list rebuilt).
  engine::EventBus::instance().dispatch(engine::events::kPluginListRefreshed,
                                        "{}");
}

void ModListController::run_loot_sort() {
  if (w_->loading_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty() || !w_->knowledge_)
    return;

  const std::string loot_game_id =
      w_->knowledge_->get(w_->current_game_id_, "loot_game_id", "");
  if (loot_game_id.empty()) {
    if (w_->status_bar_)
      w_->status_bar_->set_status(tr("This game has no LOOT support"));
    return;
  }

  // A fresh plugin DB: mods may have changed since the last render, and the
  // request must carry each plugin's current winning path.
  refresh_plugins_tab();

  engine::LootRequest request;
  request.game_id = w_->current_game_id_;
  request.loot_game_id = loot_game_id;
  request.masterlist_repo = w_->knowledge_->get(
      w_->current_game_id_, "loot_masterlist_repo", loot_game_id);
  request.game_dir = w_->current_game_dir_;
  request.profile_dir = w_->profiles_dir_path() / w_->current_profile_name_;
  request.platform = w_->platform_;
  for (const auto &p : w_->plugins_db_.plugins()) {
    request.plugins.push_back({p.name, p.full_path});
  }
  if (request.plugins.empty()) {
    if (w_->status_bar_)
      w_->status_bar_->set_status(tr("No plugins to sort"));
    return;
  }

  // gmm_lootcli ships next to the manager binary (build tree and install
  // tree alike); fall back to a PATH search.
  request.cli_path =
      QCoreApplication::applicationDirPath().toStdString() + "/gmm_lootcli";
  if (!std::filesystem::is_regular_file(request.cli_path)) {
    if (const char *path_env = std::getenv("PATH")) {
      std::istringstream ss(path_env);
      std::string token;
      while (std::getline(ss, token, ':')) {
        auto candidate = std::filesystem::path(token) / "gmm_lootcli";
        if (std::filesystem::is_regular_file(candidate)) {
          request.cli_path = candidate;
          break;
        }
      }
    }
  }

  if (!w_->loot_sort_thread_) {
    w_->loot_sort_thread_ = new ui::LootSortThread(w_);
    connect(w_->loot_sort_thread_->worker(), &ui::LootSortWorker::progress,
            this, &ModListController::on_loot_progress, Qt::UniqueConnection);
    connect(w_->loot_sort_thread_->worker(), &ui::LootSortWorker::finished,
            this, &ModListController::on_loot_finished, Qt::UniqueConnection);
  }
  if (w_->status_bar_)
    w_->status_bar_->set_status(tr("Sorting load order with LOOT…"));
  w_->loot_sort_thread_->start(std::move(request));
}

void ModListController::on_loot_progress(int stage, const QString &) {
  static const QStringList kStageNames = {
      QString(),                    // 0 - none
      tr("Checking masterlist…"),   // 1
      tr("Updating masterlist…"),   // 2
      tr("Loading masterlists…"),   // 3
      tr("Reading plugins…"),       // 4
      tr("Sorting plugins…"),       // 5
      tr("Writing load order…"),    // 6
      tr("Parsing LOOT messages…"), // 7
      tr("Load order sorted"),      // 8
  };
  if (!w_->status_bar_)
    return;
  if (stage >= 0 && stage < kStageNames.size() &&
      !kStageNames.at(stage).isEmpty())
    w_->status_bar_->set_status(kStageNames.at(stage));
}

void ModListController::on_loot_finished(engine::LootResult result) {
  if (!result.ok) {
    engine::Logger::instance().warn("LOOT sort failed: " + result.error);
    if (w_->status_bar_)
      w_->status_bar_->set_status(
          tr("LOOT sort failed: %1").arg(QString::fromStdString(result.error)));
    return;
  }

  std::string err;
  if (!w_->plugins_db_.apply_load_order(result.sorted_names, &err)) {
    if (w_->status_bar_)
      w_->status_bar_->set_status(tr("LOOT sort could not be applied: %1")
                                      .arg(QString::fromStdString(err)));
    return;
  }
  w_->plugins_db_.save_profile(w_->profiles_dir_path(),
                               w_->current_profile_name_);
  refresh_plugins_tab();
  if (w_->status_bar_)
    w_->status_bar_->set_status(tr("Load order sorted by LOOT (%1 plugins)")
                                    .arg(result.sorted_names.size()));
}

void ModListController::on_plugin_toggle(const std::string &name,
                                         bool enabled) {
  std::string err;
  if (!w_->plugins_db_.set_enabled(name, enabled, &err)) {
    auto *pt = w_->right_panel_->plugins_tab();
    if (pt)
      pt->sync_enabled(w_->plugins_db_.plugins());
    if (!err.empty())
      QMessageBox::warning(w_, tr("Plugins"), QString::fromStdString(err));
    return;
  }
  w_->plugins_db_.save_profile(w_->profiles_dir_path(),
                               w_->current_profile_name_);
  auto *pt = w_->right_panel_->plugins_tab();
  if (pt)
    pt->sync_enabled(w_->plugins_db_.plugins());
  // P1.3 event bus: mirror MO2 onPluginStateChanged.
  engine::EventBus::instance().dispatch(engine::events::kPluginStateChanged,
                                        engine::json_obj({
                                            {"plugin", name},
                                            {"enabled", enabled ? "1" : "0"},
                                        }));
}

void ModListController::on_plugin_reorder(int from_row, int to_row) {
  // Capture the moved plugin's name before the reorder so the event carries
  // it (refresh_plugins_tab() rebuilds rows right after the move).
  std::string moved_name;
  const auto &plugins_before = w_->plugins_db_.plugins();
  if (from_row >= 0 && from_row < static_cast<int>(plugins_before.size()))
    moved_name = plugins_before[static_cast<size_t>(from_row)].name;

  std::string err;
  if (!w_->plugins_db_.move_plugin(from_row, to_row, &err)) {
    if (!err.empty())
      QMessageBox::warning(w_, tr("Plugins"), QString::fromStdString(err));
    return;
  }
  w_->plugins_db_.save_profile(w_->profiles_dir_path(),
                               w_->current_profile_name_);
  refresh_plugins_tab(); // repopulate: new order + recomputed
                         // priorities/indexes
  // P1.3 event bus: mirror MO2 onPluginMoved.
  engine::EventBus::instance().dispatch(engine::events::kPluginMoved,
                                        engine::json_obj({
                                            {"plugin", moved_name},
                                            {"from", std::to_string(from_row)},
                                            {"to", std::to_string(to_row)},
                                        }));
}

void ModListController::on_plugin_lock(const std::string &name, bool locked) {
  std::string err;
  if (!w_->plugins_db_.set_locked(name, locked, &err)) {
    if (!err.empty())
      QMessageBox::warning(w_, tr("Plugins"), QString::fromStdString(err));
    return;
  }
  w_->plugins_db_.save_profile(w_->profiles_dir_path(),
                               w_->current_profile_name_);
  refresh_plugins_tab(); // repopulate: lock emblem + drag flags re-applied
}

void ModListController::rebuild_plugin_highlight_index() {
  w_->plugin_owner_index_.clear();
  w_->plugin_row_by_name_.clear();
  const auto &plugins = w_->plugins_db_.plugins();
  w_->plugin_row_by_name_.reserve(static_cast<int>(plugins.size()));
  for (size_t i = 0; i < plugins.size(); ++i) {
    const auto &p = plugins[i];
    w_->plugin_row_by_name_.insert(QString::fromStdString(p.name),
                                   static_cast<int>(i));
    if (!p.owner_mod.empty())
      w_->plugin_owner_index_[QString::fromStdString(p.owner_mod)].append(
          QString::fromStdString(p.name));
  }
}

void ModListController::on_mod_selection_changed() {
  auto *pt = w_->right_panel_ ? w_->right_panel_->plugins_tab() : nullptr;
  if (!pt || w_->plugin_row_by_name_.isEmpty())
    return;

  const auto &mods = w_->mod_model_->mods();
  QVector<QString> contained;
  QSet<QString> seen_contained;
  contained.reserve(w_->plugin_row_by_name_.size());

  const auto rows = w_->mod_view_->selectionModel()->selectedRows();
  for (const auto &idx : rows) {
    const int r = idx.row();
    if (r < 0 || r >= mods.size())
      continue;
    const auto &m = mods[r];
    if (m.is_separator || m.is_overwrite || m.is_merged)
      continue;

    // Mods own the plugins whose owner_mod matches their id (MO2's
    // highlightPlugins, via DirectoryEntry origin - GMM's owner_mod is the
    // winning origin already).
    const auto it = w_->plugin_owner_index_.constFind(m.id);
    if (it != w_->plugin_owner_index_.constEnd()) {
      for (const auto &name : it.value()) {
        if (seen_contained.contains(name))
          continue;
        seen_contained.insert(name);
        contained.append(name);
      }
    }
    // Unmanaged (game-native / stray) mod: its plugin is the game file with
    // the same name, as long as no mod wins that file.
    if (m.is_game_native) {
      const auto nit = w_->plugin_row_by_name_.constFind(m.id);
      if (nit != w_->plugin_row_by_name_.constEnd()) {
        const auto &p = w_->plugins_db_.plugins()[nit.value()];
        if (p.owner_mod.empty() && !seen_contained.contains(m.id)) {
          seen_contained.insert(m.id);
          contained.append(m.id);
        }
      }
    }
  }
  pt->set_contained_plugins(contained);
}

void ModListController::on_plugin_selection_changed() {
  auto *pt = w_->right_panel_ ? w_->right_panel_->plugins_tab() : nullptr;
  if (!pt)
    return;

  const QStringList selected = pt->selected_plugin_names();
  QSet<QString> highlighted_mods;
  QVector<QString> masters;
  QSet<QString> seen_masters;

  for (const auto &name : selected) {
    const auto it = w_->plugin_row_by_name_.constFind(name);
    if (it == w_->plugin_row_by_name_.constEnd())
      continue;
    const auto &p = w_->plugins_db_.plugins()[it.value()];
    // Owning mod: owner_mod, or the unmanaged mod row for game-owned files
    // (MO2's plugin-list selection -> setHighlightedMods).
    const QString owner =
        p.owner_mod.empty() ? name : QString::fromStdString(p.owner_mod);
    highlighted_mods.insert(owner);
    // Masters of the selected plugin render plugin_list_master.
    for (const auto &master : p.masters) {
      const QString m = QString::fromStdString(master);
      if (!w_->plugin_row_by_name_.contains(m) || seen_masters.contains(m))
        continue;
      seen_masters.insert(m);
      masters.append(m);
    }
  }
  w_->mod_model_->set_highlighted_mods(highlighted_mods);
  pt->set_master_plugins(masters);
}

void ModListController::on_image_diff_requested(const QString &relative_path) {
  if (!w_->plugin_loader_ || !w_->plugin_loader_->has_image_diff())
    return;

  // Collect all mods that own w_ file from the conflict registry
  auto it = w_->last_conflict_registry_.find(relative_path.toStdString());
  if (it == w_->last_conflict_registry_.end() || it->second.size() < 2)
    return;

  const auto &owners = it->second;
  std::vector<std::string> source_paths;
  source_paths.reserve(owners.size());

  auto mods_dir = w_->mods_dir_path();
  const std::filesystem::path game_mods_dir =
      w_->knowledge_ ? engine::resolve_game_mods_dir(w_->current_game_id_,
                                                     w_->current_game_dir_,
                                                     *w_->knowledge_)
                     : std::filesystem::path{};

  for (const auto &[mod_name, _] : owners) {
    // Check instance mods dir first, then game native mods dir
    std::filesystem::path abs_path =
        mods_dir / mod_name / relative_path.toStdString();
    if (std::filesystem::exists(abs_path)) {
      source_paths.push_back(abs_path.string());
      continue;
    }
    if (!game_mods_dir.empty()) {
      abs_path = game_mods_dir / mod_name / relative_path.toStdString();
      if (std::filesystem::exists(abs_path)) {
        source_paths.push_back(abs_path.string());
        continue;
      }
    }
  }

  if (source_paths.size() < 2)
    return;

  // Compute output path - write to MERGED pseudo-mod folder
  std::filesystem::path output_path =
      w_->mods_dir_path() / "MERGED" / relative_path.toStdString();
  std::error_code ec;
  std::filesystem::create_directories(output_path.parent_path(), ec);

  // Invoke the image diff provider
  const auto &provider = w_->plugin_loader_->image_diff_provider();
  if (provider.fn) {
    std::vector<const char *> c_paths;
    c_paths.reserve(source_paths.size());
    for (const auto &p : source_paths)
      c_paths.push_back(p.c_str());

    std::string out_str = output_path.string();
    provider.fn(c_paths.data(), c_paths.size(), out_str.c_str(),
                provider.user_data);
  }
}

void ModListController::setup_mod_list_context_menu() {
  w_->mod_view_->setContextMenuPolicy(Qt::CustomContextMenu);

  connect(
      w_->mod_view_, &QWidget::customContextMenuRequested, this,
      [this](const QPoint &pos) {
        auto idx = w_->mod_view_->indexAt(pos);
        if (!idx.isValid())
          return;

        int row = idx.row();
        if (row < 0 || row >= w_->mod_model_->mods().size())
          return;
        const auto &entry = w_->mod_model_->mods()[row];

        QMenu menu;

        if (entry.is_overwrite) {
          // MO2 ModListContextMenu::addOverwriteActions. The move/sync/clear
          // actions only make sense when Overwrite has content; Open in
          // Explorer and Information always apply. Gating mirrors MO2's
          // `QDir(...).count() > 2` via overwrite_is_empty().
          auto ow_subpath = w_->knowledge_
                                ? w_->knowledge_->get(w_->current_game_id_,
                                                      "mods_subpath", "")
                                : std::string();
          const bool has_content =
              !engine::overwrite_is_empty(w_->overwrite_dir_path(), ow_subpath);

          auto *sync_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("merge"),
              tr("Sync to Mods..."), w_,
              [this]() { w_->overwrite_->sync_overwrite_to_mods(); });
          auto *create_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("document-new"),
              tr("Create Mod..."), w_,
              [this]() { w_->overwrite_->create_mod_from_overwrite(); });
          auto *move_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("go-down"),
              tr("Move content to Mod..."), w_,
              [this]() { w_->overwrite_->move_overwrite_content_to_mod(); });
          auto *clear_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("edit-clear"),
              tr("Clear Overwrite..."), w_,
              [this]() { w_->overwrite_->clear_overwrite(); });
          for (auto *act : {sync_act, create_act, move_act, clear_act})
            act->setEnabled(has_content);

          menu.addAction(engine::IconManager::instance().resolve_icon("folder"),
                         tr("Open in File Manager"), w_, [this]() {
                           w_->overwrite_->open_overwrite_in_file_manager();
                         });

          menu.addSeparator();
          menu.addAction(engine::IconManager::instance().resolve_icon(
                             "dialog-information"),
                         tr("Information..."), w_, [this]() {
                           w_->overwrite_->show_overwrite_info_dialog();
                         });

          menu.exec(w_->mod_view_->viewport()->mapToGlobal(pos));
          return;
        }

        if (entry.is_separator) {
          // MO2's separator context menu (modlistcontextmenu.cpp:381-409):
          // Rename (inline edit) / Remove / Select Color / Reset Color.
          menu.addAction(
              engine::IconManager::instance().resolve_icon("document-edit"),
              tr("Rename Separator..."), w_,
              [this, row]() { rename_mod_inline(row); });
          menu.addAction(
              engine::IconManager::instance().resolve_icon("edit-delete"),
              tr("Remove Separator..."), w_,
              [this, row]() { delete_separator(row); });
          menu.addSeparator();
          menu.addAction(
              engine::IconManager::instance().resolve_icon("color-picker"),
              tr("Select Color..."), w_,
              [this]() { select_color_for_selected(); });
          if (!entry.separator_color.isEmpty()) {
            menu.addAction(
                engine::IconManager::instance().resolve_icon("edit-clear"),
                tr("Reset Color"), w_,
                [this]() { reset_color_for_selected(); });
          }
          menu.exec(w_->mod_view_->viewport()->mapToGlobal(pos));
          return;
        }

        // --- Mod rows below ---
        auto sel = w_->mod_view_->selectionModel()->selectedRows();
        bool multi = sel.size() > 1;

        if (multi) {
          menu.addAction(
              engine::IconManager::instance().resolve_icon("dialog-ok"),
              tr("Enable Selected"), w_,
              [this]() { toggle_selected_mods(true); });
          menu.addAction(
              engine::IconManager::instance().resolve_icon("dialog-cancel"),
              tr("Disable Selected"), w_,
              [this]() { toggle_selected_mods(false); });
          // Tweaks submenu: applies to every selected mod. Checked only when
          // ALL of them share the state; clicking applies the inverse.
          {
            QList<int> rows;
            bool any_on = false;
            bool any_off = false;
            for (const auto &si : sel) {
              if (si.row() < 0 || si.row() >= w_->mod_model_->mods().size())
                continue;
              const auto &m = w_->mod_model_->mods()[si.row()];
              if (m.is_separator || m.is_overwrite || m.is_merged ||
                  m.is_game_native)
                continue;
              rows << si.row();
              if (m.root_override)
                any_on = true;
              else
                any_off = true;
            }
            auto *tweaks =
                menu.addMenu(engine::IconManager::instance().resolve_icon(
                                 "preferences-other"),
                             tr("Tweaks"));
            auto *root_act = tweaks->addAction(tr("Treat mod as root dir"));
            root_act->setCheckable(true);
            const bool all_on = any_on && !any_off;
            root_act->setChecked(all_on);
            root_act->setEnabled(!rows.isEmpty());
            connect(root_act, &QAction::triggered, this,
                    [this, rows, all_on]() {
                      toggle_root_override(rows, !all_on);
                    });
          }
          menu.addSeparator();
          menu.addAction(
              engine::IconManager::instance().resolve_icon("edit-delete"),
              tr("Remove"), w_, [this]() { remove_selected_mods(); });
          menu.exec(w_->mod_view_->viewport()->mapToGlobal(pos));
          return;
        }

        // Single mod - full menu
        auto mod_id = entry.id;

        // Send to... submenu (MO2 modlistcontextmenu.cpp:285-338): priority
        // moves + the separator picker. The separator picker opens the shared
        // ListDialog (MO2 sendModsToSeparator, listdialog.ui) instead of an
        // inline submenu entry per separator — a submenu with many separators
        // (or long names) grew to cover the whole screen.
        auto *send_to = menu.addMenu(
            engine::IconManager::instance().resolve_icon("view-sort"),
            tr("Send to..."));
        send_to->addAction(
            engine::IconManager::instance().resolve_icon("go-top"),
            tr("Send to Highest Priority"), w_,
            [this, mod_id]() { send_to_highest_priority(mod_id); });
        send_to->addAction(
            engine::IconManager::instance().resolve_icon("go-bottom"),
            tr("Send to Lowest Priority"), w_,
            [this, mod_id]() { send_to_lowest_priority(mod_id); });
        bool any_seps = false;
        for (const auto &m : w_->mod_model_->mods())
          if (m.is_separator) {
            any_seps = true;
            break;
          }
        auto *sep_act = send_to->addAction(
            engine::IconManager::instance().resolve_icon("view-sort"),
            tr("Separator..."), w_,
            [this, mod_id]() { send_to_separator(mod_id); });
        sep_act->setEnabled(any_seps);
        if (!entry.separator_id.isEmpty() &&
            w_->mod_model_->has_conflicts_within_separator(mod_id)) {
          send_to->addAction(
              engine::IconManager::instance().resolve_icon("go-up"),
              tr("Send to Highest in Separator"), w_,
              [this, mod_id]() { send_to_highest_in_separator(mod_id); });
          send_to->addAction(
              engine::IconManager::instance().resolve_icon("go-down"),
              tr("Send to Lowest in Separator"), w_,
              [this, mod_id]() { send_to_lowest_in_separator(mod_id); });
        }

        menu.addAction(engine::IconManager::instance().resolve_icon("list-add"),
                       tr("Create Separator"), w_,
                       [this, row]() { create_separator_at_row(row); });

        // MO2's "Change Categories" (checkboxes) + "Primary Category" (radio
        // buttons) submenus (modlistcontextmenu.cpp:341-379). Both edit the
        // mod's [General] "category" CSV (primary first) in the manager
        // sidecar meta and refresh the mod list filter on change.
        add_category_menus(menu, mod_id);

        // MO2's "Ignore missing data" (modlistcontextmenu + modlistviewactions
        // ignoreMissingData): offered only on flagged rows (no valid game data
        // and/or no manager metadata). Persists [General] validated=true in the
        // mod's own meta.ini so the flags stay cleared on rescan.
        if (entry.invalid_data || entry.no_metadata) {
          menu.addSeparator();
          menu.addAction(
              engine::IconManager::instance().resolve_icon("dialog-ok"),
              tr("Ignore missing data"), w_, [this, mod_id]() {
                auto folder = w_->mods_dir_path() / mod_id.toStdString();
                if (engine::ModScanner::mark_validated(folder)) {
                  w_->mod_model_->set_invalid_data(mod_id, false);
                  w_->mod_model_->set_no_metadata(mod_id, false);
                }
              });
        }

        menu.addSeparator();
        menu.addAction(
            engine::IconManager::instance().resolve_icon("dialog-ok"),
            tr("Enable Selected"), w_,
            [this]() { toggle_selected_mods(true); });
        menu.addAction(
            engine::IconManager::instance().resolve_icon("dialog-cancel"),
            tr("Disable Selected"), w_,
            [this]() { toggle_selected_mods(false); });

        menu.addSeparator();
        menu.addAction(
            engine::IconManager::instance().resolve_icon("document-edit"),
            tr("Rename Mod..."), w_, [this, row]() { rename_mod_inline(row); });

        // Tweaks submenu - per-mod deploy options (MO2's per-mod tweaks).
        {
          auto *tweaks = menu.addMenu(
              engine::IconManager::instance().resolve_icon("preferences-other"),
              tr("Tweaks"));
          auto *root_act = tweaks->addAction(tr("Treat mod as root dir"));
          root_act->setCheckable(true);
          root_act->setChecked(entry.root_override);
          root_act->setEnabled(!entry.is_separator && !entry.is_overwrite &&
                               !entry.is_merged && !entry.is_game_native);
          root_act->setStatusTip(tr("Deploy w_ mod's files to the game root "
                                    "instead of the data dir"));
          connect(root_act, &QAction::triggered, this, [this, row]() {
            toggle_root_override({row},
                                 !w_->mod_model_->mods()[row].root_override);
          });
        }

        menu.addSeparator();
        if (!entry.source_type.isEmpty()) {
          auto src = source_visit_info(entry.source_type, entry.source_id,
                                       entry.source_page_url);
          if (!src.label.isEmpty()) {
            auto *visit_act = menu.addAction(
                engine::IconManager::instance().resolve_icon("text-html"),
                src.label, w_, [this, src]() {
                  if (!src.url.isEmpty())
                    QDesktopServices::openUrl(QUrl(src.url));
                });
            visit_act->setEnabled(!src.url.isEmpty());
          }
        }
        menu.addAction(
            engine::IconManager::instance().resolve_icon("folder"),
            tr("Open in File Manager"), w_, [this, mod_id]() {
              auto folder = w_->mods_dir_path() / mod_id.toStdString();
              std::error_code ec;
              if (!std::filesystem::exists(folder, ec)) {
                // Fall back to game's native mods directory
                auto game_mods_subpath =
                    w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                         "mods_subpath", "")
                                   : "";
                auto fallback = w_->current_game_dir_;
                if (!game_mods_subpath.empty())
                  fallback /= game_mods_subpath;
                folder = fallback / mod_id.toStdString();
              }
              QDesktopServices::openUrl(
                  QUrl::fromLocalFile(QString::fromStdString(folder.string())));
            });

        menu.addSeparator();
        menu.addAction(
            engine::IconManager::instance().resolve_icon("edit-delete"),
            tr("Remove"), w_, [this]() { remove_selected_mods(); });

        // MO2 puts Information last (modlistcontextmenu.cpp:267-273), the
        // default action after all per-type actions.
        menu.addSeparator();
        menu.addAction(
            engine::IconManager::instance().resolve_icon("dialog-information"),
            tr("Information..."), w_,
            [this, mod_id]() { on_data_mod_info(mod_id); });

        menu.exec(w_->mod_view_->viewport()->mapToGlobal(pos));
      });
}

void ModListController::add_category_menus(QMenu &menu, const QString &mod_id) {
  const auto meta_dir = w_->meta_dir_path();
  if (meta_dir.empty())
    return;

  // Current [General] "category" CSV (primary first) from the manager sidecar.
  // Re-read on every toggle so sequential checkbox changes accumulate instead
  // of each overwriting the menu-open snapshot (MO2 mutates the mod info
  // object directly; the sidecar is our equivalent source of truth).
  auto load_current = [meta_dir, mod_id]() {
    auto meta = engine::ModMeta::load(meta_dir, mod_id.toStdString());
    QVector<int> ids;
    const QString csv = QString::fromStdString(meta.get("General", "category"));
    for (const auto &part : csv.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
      bool ok = false;
      const int id = part.toInt(&ok);
      if (ok && id > 0 && !ids.contains(id))
        ids.append(id);
    }
    return ids;
  };

  // Persist a new CSV (primary first), update the Category column + filter
  // ids, and re-apply the mod filter so a category-filtered list reacts
  // immediately (MO2 refreshFilter parity).
  auto apply = [this, meta_dir, mod_id](const QVector<int> &ids) {
    QStringList parts;
    for (int id : ids)
      parts << QString::number(id);
    auto meta = engine::ModMeta::load(meta_dir, mod_id.toStdString());
    meta.set("General", "category", parts.join(QLatin1Char(',')).toStdString());
    meta.save(meta_dir, mod_id.toStdString());

    QString primary_name;
    if (!ids.isEmpty()) {
      if (const auto *cat =
              engine::CategoryFactory::instance().categoryById(ids.first()))
        primary_name = QString::fromStdString(cat->name);
    }
    w_->mod_model_->set_category(mod_id, primary_name);
    w_->mod_model_->set_category_ids(mod_id, ids);
    apply_mod_filter();
  };

  const QVector<int> current = load_current();

  // "Change Categories": one checkable action per category, alphabetized like
  // the filter panel (MO2's flat category list). Checking appends the id
  // (first checked becomes primary); unchecking removes it and the first
  // remaining id becomes primary.
  auto *change_menu = menu.addMenu(
      engine::IconManager::instance().resolve_icon("preferences-other"),
      tr("Change Categories"));
  std::vector<const engine::CategoryFactory::Category *> cats;
  for (const auto &[id, cat] : engine::CategoryFactory::instance().categories())
    if (id != 0)
      cats.push_back(&cat);
  std::sort(cats.begin(), cats.end(), [](const auto *a, const auto *b) {
    return QString::fromStdString(a->name).compare(
               QString::fromStdString(b->name), Qt::CaseInsensitive) < 0;
  });
  for (const auto *cat : cats) {
    auto *act = change_menu->addAction(QString::fromStdString(cat->name));
    act->setCheckable(true);
    act->setChecked(current.contains(cat->id));
    connect(act, &QAction::triggered, this,
            [this, mod_id, cat, load_current, apply](bool checked) {
              QVector<int> ids = load_current();
              if (checked) {
                if (!ids.contains(cat->id))
                  ids.append(cat->id);
              } else {
                ids.removeAll(cat->id);
              }
              apply(ids);
            });
  }

  // "Primary Category": radio buttons for the checked categories only (MO2
  // parity). Selecting one moves it to the front of the CSV.
  auto *primary_menu =
      menu.addMenu(engine::IconManager::instance().resolve_icon("view-sort"),
                   tr("Primary Category"));
  if (current.isEmpty()) {
    primary_menu->setEnabled(false);
  } else {
    auto *group = new QActionGroup(primary_menu);
    for (int id : current) {
      const auto *cat = engine::CategoryFactory::instance().categoryById(id);
      auto *act = primary_menu->addAction(
          cat ? QString::fromStdString(cat->name) : QString::number(id));
      act->setCheckable(true);
      act->setChecked(id == current.first());
      group->addAction(act);
      connect(act, &QAction::triggered, this,
              [this, mod_id, id, load_current, apply]() {
                QVector<int> ids = load_current();
                ids.removeAll(id);
                ids.prepend(id);
                apply(ids);
              });
    }
  }
}

void ModListController::remove_selected_mods() {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  QStringList names;
  for (const auto &idx : sel) {
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
      w_, tr("Remove Mods"),
      tr("Move %1 mod(s) to the trash bin?\n\n%2\n\nTheir files stay in the "
         "system trash and can be restored.")
          .arg(names.size())
          .arg(names.join("\n")),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply != QMessageBox::Yes)
    return;

  auto mods_subpath = w_->knowledge_ ? w_->knowledge_->get(w_->current_game_id_,
                                                           "mods_subpath", "")
                                     : std::string();
  auto meta_dir = w_->meta_dir_path();

  for (const auto &idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto &entry = w_->mod_model_->mods()[r];
    if (entry.is_overwrite)
      continue;

    // The mods dir is INSTANCE-owned (Workspace-tnj): physical removal needs
    // mods_dir_path(), not the game dir.
    if (!mods_subpath.empty() && !w_->mods_dir_path().empty()) {
      auto mod_folder = w_->mods_dir_path() / entry.id.toStdString();
      if (!engine::remove_path(mod_folder)) {
        engine::Logger::instance().error(
            "Failed to move mod folder to trash: " + mod_folder.string());
      }
    }
    // Delete cleanup: drop the manager sidecar so a removed mod leaves no
    // orphaned metadata. Children's parent_id is cleared by the model detach
    // and rewritten by sync_mod_ui_state() via mod_list_changed.
    remove_sidecar(meta_dir, entry.id);
    w_->mod_model_->remove_mod(entry.id);
  }
}

void ModListController::move_to_separator(const QString &mod_id,
                                          const QString &sep_id) {
  w_->mod_model_->set_separator_id(mod_id, sep_id);

  // Move mod row to right after the separator row
  const auto &mods = w_->mod_model_->mods();
  int sep_row = -1;
  for (int i = 0; i < mods.size(); ++i) {
    if (mods[i].is_separator && mods[i].id == sep_id) {
      sep_row = i;
      break;
    }
  }
  if (sep_row >= 0)
    w_->mod_model_->move_mod(mod_id, sep_row + 1);
}

void ModListController::send_to_separator(const QString &mod_id) {
  // MO2 sendModsToSeparator (modlistviewactions.cpp:661-701): collect the
  // separators in mod-list order into the shared ListDialog and move the mod
  // to the chosen one. Ids ride item data so duplicate display names can't
  // misresolve.
  QStringList names;
  QList<QVariant> ids;
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.is_separator) {
      names << m.name;
      ids << m.id;
    }
  }
  if (names.isEmpty())
    return;

  ui::ListDialog dlg(w_);
  dlg.setWindowTitle(tr("Select a separator..."));
  dlg.setChoices(names);
  dlg.setChoiceData(ids);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString sep_id = dlg.getChoiceData().toString();
  if (!sep_id.isEmpty())
    move_to_separator(mod_id, sep_id);
}

void ModListController::send_to_highest_priority(const QString &id) {
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

void ModListController::send_to_lowest_priority(const QString &id) {
  if (w_->mod_model_->is_conflict_order_reversed()) {
    // Isaac: highest priority number = lowest priority = bottom of list
    // (below the pinned Overwrite/MERGED which sit at the top).
    w_->mod_model_->move_mod(id, w_->mod_model_->mods().size() - 1);
  } else {
    // Standard (MO2): lowest priority number = lowest priority = top of list
    w_->mod_model_->move_mod(id, 0);
  }
}

void ModListController::send_to_highest_in_separator(const QString &id) {
  const auto &mods = w_->mod_model_->mods();
  int mod_row = w_->mod_model_->priority_of(id);
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

void ModListController::send_to_lowest_in_separator(const QString &id) {
  const auto &mods = w_->mod_model_->mods();
  int mod_row = w_->mod_model_->priority_of(id);
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

void ModListController::priority_move_selected(int step) {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  int r = sel.first().row();
  const auto &mods = w_->mod_model_->mods();
  if (r < 0 || r >= mods.size())
    return;
  const auto &e = mods[r];
  if (e.is_separator || e.is_overwrite || e.is_merged)
    return;

  int target = r + step;
  if (target < 0 || target >= mods.size())
    return;
  if (mods[target].is_separator || mods[target].is_overwrite ||
      mods[target].is_merged)
    return;

  w_->mod_model_->move_mod(e.id, target);
}

void ModListController::toggle_selected_mods(bool enabled) {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  for (const auto &idx : sel) {
    int r = idx.row();
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto &entry = w_->mod_model_->mods()[r];
    if (entry.is_separator || entry.is_overwrite || entry.is_game_native)
      continue;

    // Check if state would actually change
    if (entry.enabled == enabled)
      continue;

    w_->mod_model_->setData(w_->mod_model_->index(r, ModListModel::Name),
                            enabled ? Qt::Checked : Qt::Unchecked,
                            Qt::CheckStateRole);
    sync_mod_enable_state(entry.id, enabled);
  }
}

void ModListController::toggle_root_override(const QList<int> &rows, bool on) {
  for (int r : rows) {
    if (r < 0 || r >= w_->mod_model_->mods().size())
      continue;
    const auto &entry = w_->mod_model_->mods()[r];
    if (entry.is_separator || entry.is_overwrite || entry.is_merged ||
        entry.is_game_native)
      continue;
    if (entry.root_override == on)
      continue;

    auto mod_dir = w_->mods_dir_path() / entry.id.toStdString();
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
  refresh_data_tab();
}

QString ModListController::current_nexus_domain() const {
  if (w_->plugin_loader_ && !w_->current_game_id_.empty()) {
    for (const auto &p : w_->plugin_loader_->plugins()) {
      if (p.game_id == w_->current_game_id_)
        return QString::fromStdString(p.nexus_domain);
    }
  }
  return {};
}

SourceVisitInfo
ModListController::source_visit_info(const QString &source_type,
                                     const QString &source_id,
                                     const QString &page_url) const {
  if (source_type == "steam") {
    return {tr("Visit on Workshop"),
            QString("https://steamcommunity.com/sharedfiles/filedetails/?id=%1")
                .arg(source_id)};
  }
  if (source_type == "nexus") {
    // The game domain comes from the plugin identity (e.g.
    // "skyrimspecialedition") - NEVER the mod id, and there is no
    // "nexus_domain" knowledge hook to read. Empty domain = disabled Visit.
    const QString domain = current_nexus_domain();
    if (domain.isEmpty())
      return {tr("Visit on Nexus"), QString()};
    return {
        tr("Visit on Nexus"),
        QString("https://www.nexusmods.com/%1/mods/%2").arg(domain, source_id)};
  }
  if (source_type == "loverslab") {
    // The stored page URL (the download link minus its ?do=download query)
    // wins - the slug cannot be reconstructed from the file id alone.
    QString url = page_url;
    if (url.isEmpty() && !source_id.isEmpty())
      url = QString("https://www.loverslab.com/files/file/%1/").arg(source_id);
    return {tr("Visit on LoversLab"), url};
  }
  if (source_type == "moddb") {
    return {tr("Visit on ModDB"),
            QString("https://www.moddb.com/mods/%1").arg(source_id)};
  }
  auto label = source_type;
  if (!label.isEmpty()) {
    label[0] = label[0].toUpper();
  }
  return {tr("Visit on %1").arg(label), QString()};
}

QString ModListController::create_separator_named(const QString &name,
                                                  const QString &color) {
  // Separators are instance-owned (folder under the instance mods dir);
  // no game dir required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return {};

  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  auto separator_suffix = w_->knowledge_->get(w_->current_game_id_,
                                              "separator_suffix", "_separator");
  if (mods_subpath.empty())
    return {};

  // Guard against duplicate names
  if (w_->mod_model_->existing_separator_names().contains(name))
    return {};

  auto folder_name = name.toStdString() + separator_suffix;
  auto sep_dir = w_->mods_dir_path() / folder_name;

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

void ModListController::create_separator() {
  // Instance-owned (Workspace-tnj) — see create_separator_named.
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // MO2 createSeparator (modlistviewactions.cpp:152-204): a name-only prompt
  // filtered through fixDirectoryName; the previously used separator color is
  // inherited automatically - there is no color picker in w_ step.
  QString name;
  while (true) {
    bool ok = false;
    name = QInputDialog::getText(
        w_, tr("Create Separator..."),
        tr("This will create a new separator.\nPlease enter a name:"),
        QLineEdit::Normal, name, &ok);
    if (!ok)
      return;
    name = QString::fromStdString(
        engine::sanitize_directory_name(name.toStdString()));
    if (!name.isEmpty())
      break;
  }

  // Check for duplicate names
  if (w_->mod_model_->existing_separator_names().contains(name)) {
    QMessageBox::warning(w_, tr("Create Separator..."),
                         tr("A separator with w_ name already exists."));
    return;
  }

  auto previous = Settings::instance().previous_separator_color();
  const QString color = previous ? previous->name(QColor::HexArgb) : QString();
  if (create_separator_named(name, color).isEmpty()) {
    QMessageBox::warning(w_, tr("Create Separator..."),
                         tr("Failed to create separator directory."));
  }
}

void ModListController::create_empty_mod() {
  // Instance-owned: the empty mod folder is written into the instance mods
  // dir; no game dir required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  bool ok;
  auto name = QInputDialog::getText(w_, tr("Create Empty Mod"), tr("Mod name:"),
                                    QLineEdit::Normal, QString(), &ok);
  if (!ok || name.trimmed().isEmpty())
    return;

  // Check for duplicate names
  QString trimmed = name.trimmed();
  for (const auto &m : w_->mod_model_->mods()) {
    if (!m.is_separator && !m.is_overwrite && !m.is_merged &&
        (m.name.compare(trimmed, Qt::CaseInsensitive) == 0 ||
         m.id.compare(trimmed, Qt::CaseInsensitive) == 0)) {
      QMessageBox::warning(w_, tr("Create Empty Mod"),
                           tr("A mod with w_ name already exists."));
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
    QMessageBox::warning(w_, tr("Create Empty Mod"),
                         tr("Failed to create mods directory."));
    return;
  }

  auto mod_dir = mods_dir / folder.toStdString();
  if (std::filesystem::exists(mod_dir, ec)) {
    QMessageBox::warning(
        w_, tr("Create Empty Mod"),
        tr("A folder named %1 already exists in the mods directory.")
            .arg(folder));
    return;
  }
  std::filesystem::create_directories(mod_dir, ec);
  if (ec) {
    QMessageBox::warning(w_, tr("Create Empty Mod"),
                         tr("Failed to create mod folder."));
    return;
  }

  // Write the game's metadata file into the mod folder so ModScanner picks
  // the mod up. MO2-style games get a meta.ini (same keys MO2 and
  // InstallStage write); XML games (Isaac) get their metadata.xml.
  auto metadata_file =
      w_->knowledge_->get(w_->current_game_id_, "metadata_file", "meta.ini");
  engine::ModMeta::write_game_metadata(mod_dir, metadata_file,
                                       trimmed.toStdString(), "1.0", "0");

  engine::Logger::instance().debug("Empty mod created: " +
                                   folder.toStdString());
  load_mods_from_game();
}

void ModListController::import_archives(const QStringList &paths) {
  if (w_->current_instance_root_.empty())
    return;
  auto dl_dir = w_->downloads_dir_path();
  std::error_code ec;
  std::filesystem::create_directories(dl_dir, ec);

  for (const auto &path : paths) {
    QFileInfo fi(path);
    auto dest = dl_dir / fi.fileName().toStdString();

    // Copy archive to instance downloads folder
    if (!QFile::exists(QString::fromStdString(dest.string()))) {
      if (!QFile::copy(path, QString::fromStdString(dest.string()))) {
        engine::Logger::instance().error(
            "Failed to copy archive to downloads: " + dest.string());
        continue;
      }
    }

    auto mod_id = fi.completeBaseName().toStdString();

    // Show in DownloadsTab immediately with file path, mark ready
    auto *dt = w_->right_panel_->downloads_tab();
    if (dt) {
      dt->add_download(mod_id, mod_id, "Manual", dest);
      dt->mark_complete(mod_id, true);
    }
  }
}

void ModListController::export_modlist() {
  if (w_->current_game_id_.empty())
    return;
  const QString path =
      QFileDialog::getSaveFileName(w_, tr("Export Modlist"), QString(),
                                   tr("CSV files (*.csv);;All files (*)"));
  if (path.isEmpty())
    return;

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(w_, tr("Export Modlist"), tr("Failed to write file."));
    return;
  }

  auto write_row = [&](const QStringList &fields) {
    QStringList escaped;
    escaped.reserve(fields.size());
    for (const auto &field : fields)
      escaped << csv_escape(field);
    f.write(escaped.join(",").toUtf8());
    f.write("\n");
  };

  write_row({QStringLiteral("type"), QStringLiteral("priority"),
             QStringLiteral("name"), QStringLiteral("source_link"),
             QStringLiteral("color"), QStringLiteral("modid"),
             QStringLiteral("folder_name")});

  const auto &mods = w_->mod_model_->mods();
  int exported = 0;
  for (int i = 0; i < mods.size(); ++i) {
    const auto &m = mods[i];
    if (m.is_overwrite || m.is_merged)
      continue;
    if (m.is_separator) {
      write_row({QStringLiteral("separator"), QString::number(i), m.name,
                 QString(), m.separator_color, QString(), m.id});
    } else {
      QString source;
      if (!m.source_type.isEmpty())
        source =
            source_visit_info(m.source_type, m.source_id, m.source_page_url)
                .url;
      write_row({QStringLiteral("mod"), QString::number(i), m.name, source,
                 QString(), m.source_id, m.id});
    }
    ++exported;
  }
  f.close();
  engine::Logger::instance().info(
      "Modlist exported: " + std::to_string(exported) + " entries");
}

void ModListController::import_modlist() {
  if (w_->current_game_id_.empty())
    return;
  const QString path =
      QFileDialog::getOpenFileName(w_, tr("Import Modlist"), QString(),
                                   tr("CSV files (*.csv);;All files (*)"));
  if (path.isEmpty())
    return;

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(w_, tr("Import Modlist"), tr("Failed to open file."));
    return;
  }
  auto rows = parse_csv(f.readAll());
  f.close();

  if (rows.empty()) {
    QMessageBox::warning(w_, tr("Import Modlist"), tr("Invalid modlist file."));
    return;
  }

  // Skip the header row if present
  size_t start = 0;
  if (rows[0].value(0).trimmed() == QLatin1String("type"))
    start = 1;

  // Match by strict priority: modid > folder name > display name. Each
  // criterion gets its own full pass so an early name collision can't
  // shadow a later, stronger folder/modid match.
  auto find_row = [this](const QString &modid, const QString &folder_name,
                         const QString &name, bool want_separator) -> int {
    const auto &mods = w_->mod_model_->mods();
    auto match_any = [&mods, want_separator](const QString &key,
                                             QString ModEntry::*field) -> int {
      if (key.isEmpty())
        return -1;
      for (int i = 0; i < mods.size(); ++i) {
        const auto &m = mods[i];
        if (m.is_overwrite || m.is_merged)
          continue;
        if (m.is_separator != want_separator)
          continue;
        if ((m.*field).compare(key, Qt::CaseInsensitive) == 0)
          return i;
      }
      return -1;
    };
    int idx = match_any(modid, &ModEntry::source_id);
    if (idx < 0)
      idx = match_any(folder_name, &ModEntry::id);
    if (idx < 0)
      idx = match_any(name, &ModEntry::name);
    return idx;
  };

  // Batch the reorder with disk syncs suppressed; persist once at the end.
  w_->loading_ = true;
  int placed = 0;
  int created = 0;
  int missing = 0;
  int cursor = 0;
  for (size_t r = start; r < rows.size(); ++r) {
    const auto &row = rows[r];
    QString type = row.value(0).trimmed().toLower();
    QString name = row.value(2).trimmed();
    QString modid = row.value(5).trimmed();
    QString folder_name = row.value(6).trimmed();
    bool is_separator = (type == QLatin1String("separator"));

    int idx = is_separator ? find_row(QString(), folder_name, name, true)
                           : find_row(modid, folder_name, name, false);
    if (idx < 0 && is_separator && !name.isEmpty()) {
      QString color = row.value(4).trimmed();
      if (create_separator_named(name, color.isEmpty() ? QString() : color)
              .isEmpty()) {
        ++missing;
        continue;
      }
      ++created;
      idx = find_row(QString(), QString(), name, true);
    }
    if (idx < 0) {
      ++missing;
      continue;
    }

    const auto &mods = w_->mod_model_->mods();
    if (idx != cursor)
      w_->mod_model_->move_mod(mods[idx].id, cursor);
    ++cursor;
    ++placed;
  }
  w_->loading_ = false;

  save_order();
  sync_priorities();
  sync_separator_ids();
  apply_mod_filter();

  const int total = static_cast<int>(rows.size() - start);
  engine::Logger::instance().info(
      "Modlist import: " + std::to_string(placed) + " of " +
      std::to_string(total) + " placed, " + std::to_string(created) +
      " separators created, " + std::to_string(missing) + " missing");
  QMessageBox::information(w_, tr("Import Modlist"),
                           tr("Placed %1 of %2 entries in order. %3 "
                              "separator(s) created. %4 not found.")
                               .arg(placed)
                               .arg(total)
                               .arg(created)
                               .arg(missing));
}

void ModListController::open_folder(ui::FolderKind kind) {
  std::filesystem::path target;
  switch (kind) {
  case ui::FolderKind::Game:
    target = w_->current_game_dir_;
    break;
  case ui::FolderKind::MyGames:
  case ui::FolderKind::Inis:
    // INIs live in the profile folder when local INIs are on, else in the
    // game's My Games folder (MO2's openIniFolder semantics).
    if (kind == ui::FolderKind::Inis && Settings::instance().local_inis()) {
      target = w_->profiles_dir_path() / w_->current_profile_name_;
    } else {
      target = w_->game_mygames_dir();
    }
    break;
  case ui::FolderKind::Instance:
    target = w_->current_instance_root_;
    break;
  case ui::FolderKind::Mods:
    target = w_->mods_dir_path();
    break;
  case ui::FolderKind::Profile:
    target = w_->profiles_dir_path() / w_->current_profile_name_;
    break;
  case ui::FolderKind::Downloads:
    target = w_->downloads_dir_path();
    break;
  case ui::FolderKind::Install:
    target = QCoreApplication::applicationDirPath().toStdString();
    break;
  case ui::FolderKind::Plugins:
    target = std::filesystem::path(
                 QCoreApplication::applicationDirPath().toStdString()) /
             "plugins";
    break;
  case ui::FolderKind::Themes:
    target = engine::theme_search_dirs(
                 QCoreApplication::applicationDirPath().toStdString())
                 .front();
    break;
  case ui::FolderKind::Logs:
    if (w_->platform_)
      target = w_->platform_->data_dir();
    break;
  }

  if (target.empty())
    return;

  std::error_code ec;
  if (!std::filesystem::exists(target, ec))
    std::filesystem::create_directories(target, ec);
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(target.string())));
}

void ModListController::create_separator_at_row(int row) {
  // Instance-owned (Workspace-tnj) — see create_separator_named.
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // Same MO2 flow as create_separator(): name-only prompt, previous color.
  QString name;
  while (true) {
    bool ok = false;
    name = QInputDialog::getText(
        w_, tr("Create Separator..."),
        tr("This will create a new separator.\nPlease enter a name:"),
        QLineEdit::Normal, name, &ok);
    if (!ok)
      return;
    name = QString::fromStdString(
        engine::sanitize_directory_name(name.toStdString()));
    if (!name.isEmpty())
      break;
  }

  // Check for duplicate names
  if (w_->mod_model_->existing_separator_names().contains(name)) {
    QMessageBox::warning(w_, tr("Create Separator..."),
                         tr("A separator with w_ name already exists."));
    return;
  }

  auto previous = Settings::instance().previous_separator_color();
  const QString color = previous ? previous->name(QColor::HexArgb) : QString();
  auto id = create_separator_named(name, color);
  if (id.isEmpty()) {
    QMessageBox::warning(w_, tr("Create Separator..."),
                         tr("Failed to create separator directory."));
    return;
  }

  // Move the new separator to the target row (below the clicked row)
  int insert_row = row + 1;
  w_->mod_model_->move_mod(id, insert_row);
  engine::Logger::instance().debug("Separator created at row " +
                                   std::to_string(insert_row) + ": " +
                                   name.toStdString());
}

void ModListController::rename_mod_inline(int row) {
  if (row < 0 || row >= w_->mod_model_->mods().size())
    return;
  const auto &mod = w_->mod_model_->mods()[row];
  if (mod.is_overwrite || mod.is_merged || mod.is_game_native)
    return;
  w_->mod_view_->edit(w_->mod_model_->index(row, ModListModel::Name));
}

void ModListController::apply_rename(int row, const QString &name) {
  const auto revert = [this, row]() {
    emit w_->mod_model_->dataChanged(
        w_->mod_model_->index(row, ModListModel::Name),
        w_->mod_model_->index(row, ModListModel::Version));
  };

  if (row < 0 || row >= w_->mod_model_->mods().size())
    return;
  const auto &entry = w_->mod_model_->mods()[row];
  if (entry.is_overwrite || entry.is_merged || entry.is_game_native) {
    revert();
    return;
  }

  if (name == entry.name) {
    revert();
    return;
  } // unchanged

  // Instance-owned: the rename moves the folder under the instance mods dir
  // (+ meta sidecar); no game dir required (Workspace-tnj).
  if (!w_->knowledge_ || w_->current_game_id_.empty()) {
    revert();
    return;
  }

  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  auto separator_suffix = w_->knowledge_->get(w_->current_game_id_,
                                              "separator_suffix", "_separator");
  if (mods_subpath.empty()) {
    revert();
    return;
  }

  // Internal name = folder name on disk (MO2 makeInternalName): separators
  // get the suffix appended, everything else is the raw name.
  auto clean = engine::sanitize_directory_name(name.toStdString());
  if (clean.empty()) {
    QMessageBox::warning(w_, tr("Rename"), tr("Invalid name."));
    revert();
    return;
  }
  const QString display_name = QString::fromStdString(clean);
  std::string internal = clean;
  if (entry.is_separator)
    internal += separator_suffix;
  const QString new_id = QString::fromStdString(internal);

  if (new_id == entry.id) {
    revert();
    return;
  } // sanitized back to the same folder

  // Duplicate check (case-insensitive, excluding self) - MO2 renameMod.
  for (const auto &m : w_->mod_model_->mods()) {
    if (m.id == entry.id)
      continue;
    if (m.id.compare(new_id, Qt::CaseInsensitive) == 0) {
      QMessageBox::warning(w_, tr("Rename"),
                           tr("Name is already in use by another mod."));
      revert();
      return;
    }
  }

  auto mods_dir = w_->mods_dir_path();
  const auto old_path = mods_dir / entry.id.toStdString();
  const auto new_path = mods_dir / new_id.toStdString();

  std::error_code ec;
  if (std::filesystem::exists(new_path, ec)) {
    QMessageBox::warning(
        w_, tr("Rename"),
        tr("A folder named %1 already exists in the mods directory.")
            .arg(new_id));
    revert();
    return;
  }

  if (std::filesystem::exists(old_path, ec)) {
    std::filesystem::rename(old_path, new_path, ec);
    if (ec) {
      QMessageBox::warning(w_, tr("Rename"),
                           tr("Failed to rename mod folder."));
      revert();
      return;
    }
  }

  // Move the instance-meta sidecar along with the folder so source/separator
  // info isn't lost; keep its [GameModManager] folder key in sync.
  auto meta_dir = w_->meta_dir_path();
  if (!meta_dir.empty()) {
    auto old_meta = meta_dir / (entry.id.toStdString() + ".ini");
    auto new_meta = meta_dir / (new_id.toStdString() + ".ini");
    if (std::filesystem::exists(old_meta, ec)) {
      std::filesystem::rename(old_meta, new_meta, ec);
      if (!ec) {
        auto meta = engine::ModMeta::load(meta_dir, new_id.toStdString());
        meta.set("GameModManager", "folder", new_id.toStdString());
        meta.save(meta_dir, new_id.toStdString());
      }
    }
  }

  w_->mod_model_->rename_mod_in_place(row, new_id, display_name);
  engine::Logger::instance().debug("Renamed mod: " + entry.name.toStdString() +
                                   " -> " + display_name.toStdString());
}

void ModListController::delete_separator(int row) {
  if (row < 0 || row >= w_->mod_model_->mods().size())
    return;
  const auto &mod = w_->mod_model_->mods()[row];
  if (!mod.is_separator)
    return;

  auto reply = QMessageBox::question(
      w_, tr("Delete Separator"),
      tr("Move separator \"%1\" to the trash bin?\n\nIt can be restored from "
         "the system trash.")
          .arg(mod.name),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply != QMessageBox::Yes)
    return;

  // Separators are pure UI/model constructs (+ an optional folder in the
  // instance mods dir), so deletion must proceed even without a game dir
  // (Workspace-tnj) — only knowledge/game_id gate the disk access below.
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  if (!mods_subpath.empty() && !w_->mods_dir_path().empty()) {
    auto sep_folder = w_->mods_dir_path() / mod.id.toStdString();
    if (!engine::remove_path(sep_folder)) {
      engine::Logger::instance().error(
          "Failed to move separator folder to trash: " + sep_folder.string());
    }
  }

  // Delete cleanup: drop the manager sidecar. Children of the separator are
  // detached by the model (parent_id cleared) and their sidecars rewritten by
  // sync_mod_ui_state() via mod_list_changed.
  remove_sidecar(w_->meta_dir_path(), mod.id);
  w_->mod_model_->remove_mod(mod.id);
  engine::Logger::instance().debug("Separator deleted: " +
                                   mod.name.toStdString());
}

void ModListController::select_color_for_selected() {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  const auto &ref = w_->mod_model_->mods()[sel.first().row()];
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

  for (const auto &idx : sel) {
    int row = idx.row();
    if (row < 0 || row >= w_->mod_model_->mods().size())
      continue;
    const auto &mod = w_->mod_model_->mods()[row];
    if (!mod.is_separator)
      continue;
    write_separator_color_file(w_->mods_dir_path() / mod.id.toStdString(), hex);
    w_->mod_model_->set_mod_color(mod.id, color);
  }
}

void ModListController::reset_color_for_selected() {
  auto sel = w_->mod_view_->selectionModel()->selectedRows();
  if (sel.isEmpty())
    return;

  for (const auto &idx : sel) {
    int row = idx.row();
    if (row < 0 || row >= w_->mod_model_->mods().size())
      continue;
    const auto &mod = w_->mod_model_->mods()[row];
    if (!mod.is_separator)
      continue;
    write_separator_color_file(w_->mods_dir_path() / mod.id.toStdString(),
                               QString());
    w_->mod_model_->clear_mod_color(mod.id);
  }
  Settings::instance().remove_previous_separator_color();
}

void ModListController::save_order() {
  if (w_->current_instance_root_.empty())
    return;

  // Save the ordered list of folder names (including separators) to
  // instance.toml
  auto toml_path = w_->current_instance_root_ / "instance.toml";

  // Read-modify-write: preserve every other key in the file.
  auto tbl = engine::parse_instance_toml(toml_path);
  if (!tbl)
    tbl = toml::table{};

  // Remove old mod_order / folded_separators / folded_mods / mod_parents /
  // toolbar_shortcut_icons keys. The three UI-state keys
  // (folded_separators/folded_mods/mod_parents) are no longer written — per-mod
  // fold/parent state lives in the manager sidecar (Issue #35) — but legacy
  // keys are stripped here so the next save heals old instance.toml files.
  // The icons key is only stripped for backward compat with pre-#34 files; it
  // is no longer written either.
  for (const char *legacy : {"mod_order", "folded_separators", "folded_mods",
                             "mod_parents", "toolbar_shortcut_icons"}) {
    tbl->erase(legacy);
  }

  // Toolbar shortcuts (Issue #34): game-relative executable paths referencing
  // the executables list. The icon, args/cwd/env, output mod and title are
  // inherited from the referenced Executables::Entry - no per-shortcut config is
  // duplicated here anymore. toolbar_shortcut_icons was removed with the
  // schema change (legacy files are migrated on load).
  auto ts = toml::array{};
  for (const auto &path : w_->toolbar_shortcut_paths_)
    ts.push_back(path.toStdString());
  tbl->insert_or_assign("toolbar_shortcuts", std::move(ts));

  std::ofstream out(toml_path);
  if (!out)
    return;
  out << engine::serialize_instance_toml(*tbl);
}

void ModListController::load_order() {
  if (w_->current_instance_root_.empty())
    return;

  auto toml_path = w_->current_instance_root_ / "instance.toml";
  auto tbl = engine::parse_instance_toml(toml_path);
  if (!tbl)
    return;

  std::vector<std::string>
      order; // migrated from mod_order (for backward compat)
  std::vector<std::string> folded_names;
  std::vector<std::string> folded_mod_names; // visual nesting (folded mods)
  std::vector<std::pair<std::string, std::string>>
      parent_links; // "child" -> "parent"
  std::vector<std::string> toolbar_paths;
  std::vector<std::string> toolbar_icons;

  // Legacy mod_order: array of folder names (strings; tolerate numeric ids
  // from very old files).
  if (auto arr = (*tbl)["mod_order"].as_array()) {
    for (const auto &e : *arr) {
      if (auto s = e.value<std::string>()) {
        order.push_back(*s);
      } else if (auto i = e.value<int64_t>()) {
        order.push_back(std::to_string(*i));
      }
    }
  }
  if (auto arr = (*tbl)["folded_separators"].as_array()) {
    for (const auto &e : *arr) {
      if (auto s = e.value<std::string>())
        folded_names.push_back(*s);
    }
  }
  if (auto arr = (*tbl)["folded_mods"].as_array()) {
    for (const auto &e : *arr) {
      if (auto s = e.value<std::string>())
        folded_mod_names.push_back(*s);
    }
  }
  // Legacy mod_parents: array of "child=parent" strings.
  if (auto arr = (*tbl)["mod_parents"].as_array()) {
    for (const auto &e : *arr) {
      if (auto s = e.value<std::string>()) {
        auto eq = s->find('=');
        if (eq != std::string::npos && eq != 0 && eq != s->size() - 1)
          parent_links.emplace_back(s->substr(0, eq), s->substr(eq + 1));
      }
    }
  }
  if (auto arr = (*tbl)["toolbar_shortcuts"].as_array()) {
    for (const auto &e : *arr) {
      if (auto s = e.value<std::string>())
        toolbar_paths.push_back(*s);
    }
  }
  if (auto arr = (*tbl)["toolbar_shortcut_icons"].as_array()) {
    for (const auto &e : *arr) {
      if (auto s = e.value<std::string>())
        toolbar_icons.push_back(*s);
    }
  }

  w_->loading_ = true;

  auto mods = w_->mod_model_->mods();
  if (mods.isEmpty()) {
    w_->loading_ = false;
    return;
  }

  // --- Per-mod UI state: manager sidecar primary, legacy instance.toml
  // fallback (one-release read-compat) ---
  //
  // folded + parent_id live in the manager sidecar
  // ({instance_root}/meta/{folder_name}.ini, [GameModManager] section).
  // Legacy instance.toml keys (folded_separators/folded_mods/mod_parents)
  // are only consulted when a sidecar lacks the new key, and are migrated
  // into the sidecar here (self-healing). save_order() strips the legacy
  // keys on the next write. Pseudo-rows (Overwrite/MERGED/game-native)
  // never persist fold/parent.
  auto meta_dir = w_->meta_dir_path();
  // id -> {folded, parent_id} effective values (sidecar wins, legacy fills
  // the gaps for one release).
  QHash<QString, std::pair<bool, QString>> ui_state;
  bool migrated = false;
  for (auto &m : mods) {
    if (m.is_overwrite || m.is_merged || m.is_game_native)
      continue;
    auto st = load_sidecar_ui_state(meta_dir, m.id);
    bool folded = false;
    QString parent;
    bool row_migrated = false;
    if (st.has_folded) {
      folded = st.folded;
    } else {
      // Legacy fallback: folded_separators/folded_mods are name-keyed (same
      // matching as the old apply_fold).
      const auto &names = m.is_separator ? folded_names : folded_mod_names;
      for (const auto &fn : names) {
        if (m.name.toStdString() == fn) {
          folded = true;
          st.meta.set_folded(true);
          row_migrated = true;
          break;
        }
      }
    }
    if (st.has_parent) {
      parent = st.parent_id;
    } else {
      // Legacy fallback: mod_parents is id-keyed ("child=parent").
      for (const auto &[child, par] : parent_links) {
        if (child == m.id.toStdString()) {
          parent = QString::fromStdString(par);
          st.meta.set_parent_id(par);
          row_migrated = true;
          break;
        }
      }
    }
    if (row_migrated) {
      if (st.meta.save(meta_dir, m.id.toStdString())) {
        migrated = true;
      } else {
        engine::Logger::instance().error(
            "Failed to migrate UI state sidecar for " + m.id.toStdString());
      }
    }
    ui_state.insert(m.id, {folded, parent});
  }
  if (migrated) {
    engine::Logger::instance().debug(
        "Migrated folded/parent UI state from instance.toml to manager "
        "sidecars");
  }

  // Apply fold state from the effective map (sidecar primary; legacy filled
  // the gaps above). The flag is reset first so a stale sidecar entry can't
  // resurrect a fold that was later unfolded.
  auto apply_fold = [&ui_state](ModEntry &m) {
    auto it = ui_state.constFind(m.id);
    m.folded = (it != ui_state.constEnd()) ? it->first : false;
  };

  // Id-based nesting links from the effective parent map.
  QHash<QString, QString> parent_map;
  for (auto it = ui_state.constBegin(); it != ui_state.constEnd(); ++it) {
    if (!it->second.isEmpty())
      parent_map.insert(it.key(), it->second);
  }

  if (!order.empty()) {
    QMap<QString, int> id_to_idx;
    for (int i = 0; i < mods.size(); ++i) {
      if (!mods[i].is_overwrite && !mods[i].is_merged)
        id_to_idx[mods[i].id] = i;
    }
    QVector<ModEntry> reordered;
    // Saved order
    int prio = 1;
    for (const auto &folder_str : order) {
      auto folder_id = QString::fromStdString(folder_str);
      if (id_to_idx.contains(folder_id)) {
        auto entry = mods[id_to_idx[folder_id]];
        entry.priority = prio++;
        apply_fold(entry);
        reordered.append(entry);
        id_to_idx.remove(folder_id);
      }
    }
    // Remaining (new) entries
    for (auto &m : mods) {
      if (id_to_idx.contains(m.id)) {
        m.priority = prio++;
        apply_fold(m);
        reordered.append(m);
      }
    }
    // Game-native (unmanaged) mods own the top band: hoist them above the
    // user entries, preserving relative order on both sides.
    std::stable_partition(reordered.begin(), reordered.end(),
                          [](const ModEntry &m) { return m.is_game_native; });
    // Overwrite always first, MERGED always second (only for games that use it)
    reordered.insert(0, ModEntry());
    reordered[0].is_overwrite = true;
    reordered[0].id = kOverwriteModId;
    reordered[0].name = kOverwriteModName;
    reordered[0].enabled = true;
    reordered[0].priority = 0;
    if (w_->mod_model_->uses_merged()) {
      reordered.insert(1, ModEntry());
      reordered[1].is_merged = true;
      reordered[1].id = kMergedModId;
      reordered[1].name = kMergedModName;
      reordered[1].enabled = true;
      reordered[1].priority = 1;
    }
    w_->mod_model_->reset_with_order(reordered);
    w_->mod_model_->renumber_priorities();
    engine::Logger::instance().debug("Migrated from mod_order (" +
                                     std::to_string(order.size()) +
                                     " entries)");
  } else {
    // New path: sort by priority from meta.ini
    QVector<ModEntry> sorted = mods;
    // Mods without a persisted priority (-1) sort to the bottom of the user
    // band, never the top - otherwise a freshly installed mod would win the
    // list (top = priority 0).
    auto key = [](const ModEntry &e) {
      return e.priority < 0 ? 1000000 : e.priority;
    };
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&key](const ModEntry &a, const ModEntry &b) {
                       if (a.is_overwrite)
                         return false;
                       if (b.is_overwrite)
                         return true;
                       if (a.is_merged)
                         return false;
                       if (b.is_merged)
                         return true;
                       // Game-native (unmanaged) mods form a fixed top band -
                       // they can never sort below user mods, whatever priority
                       // got persisted. A separator placed above the band
                       // (lower priority than the natives) keeps its place so
                       // its fold can hide the native mods.
                       if (a.is_game_native != b.is_game_native) {
                         if (a.is_separator)
                           return key(a) < key(b);
                         if (b.is_separator)
                           return key(b) > key(a);
                         return a.is_game_native;
                       }
                       return key(a) < key(b);
                     });
    bool needs_sort = false;
    for (int i = 0; i < mods.size(); ++i) {
      if (mods[i].id != sorted[i].id) {
        needs_sort = true;
        break;
      }
    }
    if (needs_sort) {
      for (auto &m : sorted)
        apply_fold(m);
      w_->mod_model_->reset_with_order(sorted);
      w_->mod_model_->renumber_priorities();
    } else {
      // Apply fold states directly to model
      for (int i = 0; i < w_->mod_model_->mods().size(); ++i) {
        ModEntry copy = w_->mod_model_->mods()[i];
        apply_fold(copy);
        w_->mod_model_->set_folded(i, copy.folded);
      }
    }
  }

  // Restore persisted visual-nesting parent links and re-validate them
  // (sanitize_parent_links clears dangling / kind-mismatched / cyclic links,
  // e.g. a child whose parent was deleted or renamed out of existence).
  w_->mod_model_->restore_parent_links(parent_map);

  // Ensure apply_fold_state() reflects current flags
  w_->mod_model_->apply_fold_state();

  // Delete cleanup (self-healing): drop orphaned sidecars — files in the
  // meta dir whose mod folder no longer exists in the model (deleted outside
  // the manager, or rows removed while it was closed). Pseudo-row sidecars
  // (Overwrite/MERGED, created by sync_priorities) are kept. Children of a
  // deleted row get their parent_id cleared by the model's detach path, then
  // rewritten by sync_mod_ui_state().
  {
    std::error_code ec;
    if (!meta_dir.empty() && std::filesystem::exists(meta_dir, ec)) {
      QSet<QString> valid_ids;
      for (const auto &m : w_->mod_model_->mods())
        valid_ids.insert(m.id);
      for (const auto &entry :
           std::filesystem::directory_iterator(meta_dir, ec)) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec))
          continue;
        auto fname = entry.path().filename().string();
        if (fname.size() < 5 || fname.compare(fname.size() - 4, 4, ".ini") != 0)
          continue;
        auto folder = QString::fromStdString(fname.substr(0, fname.size() - 4));
        if (valid_ids.contains(folder))
          continue;
        std::error_code remove_ec;
        if (std::filesystem::remove(entry.path(), remove_ec)) {
          engine::Logger::instance().debug("Removed orphaned sidecar: " +
                                           entry.path().string());
        }
      }
    }
  }

  // Restore toolbar shortcuts (Issue #34): pins are game-relative paths
  // referencing the executables list. Legacy pre-#34 files stored absolute
  // paths (and a parallel toolbar_shortcut_icons array) - migrate absolute
  // pins to game-relative refs when they resolve under the game dir, keep the
  // legacy icon as a fallback until materialize_toolbar_shortcuts folds it
  // into the referenced Executables::Entry.
  engine::Logger::instance().begin_group(engine::LogLevel::Debug,
                                         "Restored toolbar shortcuts");
  for (size_t i = 0; i < toolbar_paths.size(); ++i) {
    auto pin = QString::fromStdString(toolbar_paths[i]);
    if (!pin.isEmpty() && QFileInfo(pin).isAbsolute()) {
      const auto rel = to_game_relative_path(w_->current_game_dir_, pin);
      if (!rel.isEmpty() && !QFileInfo(rel).isAbsolute())
        pin = rel;
    }
    // "-" was the persisted empty-icon sentinel in legacy files (see the old
    // save_order); newer files have no icons array at all.
    auto icon = (i < toolbar_icons.size() && toolbar_icons[i] != "-")
                    ? QString::fromStdString(toolbar_icons[i])
                    : QString();
    w_->launch_->add_toolbar_shortcut_from_path(pin, icon);
  }
  engine::Logger::instance().end_group();

  w_->loading_ = false;

  sync_separator_ids();
}

void ModListController::sync_separator_ids() {
  if (w_->current_instance_root_.empty())
    return;
  auto meta_dir = w_->meta_dir_path();
  if (meta_dir.empty())
    return;

  const auto &mods = w_->mod_model_->mods();
  QString current_sep_id;
  for (int i = 0; i < mods.size(); ++i) {
    const auto &m = mods[i];
    if (m.is_separator) {
      current_sep_id = m.id;
      w_->mod_model_->set_separator_id(m.id, m.id);
    } else if (m.is_overwrite) {
      w_->mod_model_->set_separator_id(m.id, QString());
    } else {
      QString new_sid = current_sep_id.isEmpty() ? QString() : current_sep_id;
      if (m.separator_id != new_sid) {
        w_->mod_model_->set_separator_id(m.id, new_sid);
        // Persist to meta.ini
        auto folder_name = m.id.toStdString();
        auto meta = engine::ModMeta::load(meta_dir, folder_name);
        meta.set_separator_id(new_sid.toStdString());
        meta.save(meta_dir, folder_name);
      }
    }
  }
}

void ModListController::sync_mod_ui_state() {
  if (w_->loading_)
    return;
  if (w_->current_instance_root_.empty())
    return;
  auto meta_dir = w_->meta_dir_path();
  if (meta_dir.empty())
    return;

  // Persist per-mod UI state (folded + parent_id) to the manager sidecar.
  // Pseudo-rows (Overwrite/MERGED/game-native) never persist fold/parent.
  // Only rows whose sidecar diverges from the model are written, so a
  // steady-state mod_list_changed (e.g. a priority move) costs reads only.
  const auto &mods = w_->mod_model_->mods();
  for (const auto &m : mods) {
    if (m.is_overwrite || m.is_merged || m.is_game_native)
      continue;
    auto st = load_sidecar_ui_state(meta_dir, m.id);
    bool changed = false;
    if (!st.has_folded || st.folded != m.folded) {
      st.meta.set_folded(m.folded);
      changed = true;
    }
    if (m.parent_id.isEmpty()) {
      if (st.has_parent) {
        st.meta.unset("GameModManager", "parent_id");
        changed = true;
      }
    } else if (st.parent_id != m.parent_id) {
      st.meta.set_parent_id(m.parent_id.toStdString());
      changed = true;
    }
    if (changed && !st.meta.save(meta_dir, m.id.toStdString())) {
      engine::Logger::instance().error(
          "Failed to persist UI state sidecar for " + m.id.toStdString());
    }
  }
}

void ModListController::group_mods_by_separator() {
  const auto &mods = w_->mod_model_->mods();
  if (mods.isEmpty())
    return;
  auto meta_dir = w_->meta_dir_path();
  if (meta_dir.empty())
    return;

  // Collect separators first (in their current order)
  QVector<ModEntry> separators;
  QVector<ModEntry> ungrouped;
  QMap<QString, QVector<ModEntry>> grouped; // separator_id → mods

  ModEntry overwrite_entry;
  bool has_overwrite = false;

  for (const auto &m : mods) {
    if (m.is_separator) {
      separators.append(m);
    } else if (m.is_overwrite) {
      overwrite_entry = m;
      has_overwrite = true;
    } else {
      // Read separator_id from w_ mod's meta.ini
      auto meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
      auto sid = QString::fromStdString(meta.separator_id());
      if (!sid.isEmpty()) {
        grouped[sid].append(m);
      } else {
        ungrouped.append(m);
      }
    }
  }

  // Rebuild: separators with their grouped mods, then ungrouped, then Overwrite
  // at bottom
  QVector<ModEntry> reordered;

  for (const auto &sep : separators) {
    reordered.append(sep);
    auto it = grouped.find(sep.id);
    if (it != grouped.end()) {
      for (auto &m : it.value()) {
        m.separator_id = sep.id;
        reordered.append(m);
      }
      grouped.erase(it);
    }
  }

  // Any remaining grouped mods whose separator no longer exists → append
  // ungrouped
  for (auto it = grouped.begin(); it != grouped.end(); ++it) {
    for (auto &m : it.value()) {
      m.separator_id.clear();
      ungrouped.append(m);
    }
  }

  for (auto &m : ungrouped)
    reordered.append(m);

  // Overwrite always at bottom
  if (has_overwrite)
    reordered.append(overwrite_entry);

  w_->loading_ = true;
  w_->mod_model_->reset_with_order(reordered);
  w_->loading_ = false;

  engine::Logger::instance().debug(
      "Grouped mods by separator (fallback order)");
}

void ModListController::apply_mod_filter() {
  if (!w_->mod_model_ || !w_->mod_view_)
    return;

  // Start from a clean fold state
  w_->mod_model_->apply_fold_state();

  const QString text = w_->filter_bar_->filter_text().trimmed().toLower();
  const QString group = w_->filter_bar_->current_group();
  const auto &mods = w_->mod_model_->mods();

  // Category filter: with any category checked, a mod matches when it carries
  // at least one checked category id (MO2 OR semantics). No checked categories
  // = no category filter.
  const QSet<int> checked_categories =
      w_->category_filter_panel_
          ? w_->category_filter_panel_->checked_category_ids()
          : QSet<int>();
  const bool category_filter_active = !checked_categories.isEmpty();

  // Fold-hidden set (pure model computation): a folded separator band scope
  // or a folded mod subtree. Filtered-out rows inside a fold scope must stay
  // hidden and must never be re-shown by the ancestor propagation below.
  QVector<bool> fold_hidden(mods.size(), false);
  for (int row = 0; row < mods.size(); ++row)
    fold_hidden[row] = w_->mod_model_->is_row_fold_hidden(row);

  // First pass: compute visibility for each mod row
  QVector<bool> visible(mods.size(), false);
  for (int row = 0; row < mods.size(); ++row) {
    const auto &m = mods[row];

    // Separators: determined in second pass
    if (m.is_separator)
      continue;

    // Text filter: match against name or id
    bool text_match = text.isEmpty() || m.name.toLower().contains(text) ||
                      m.id.toLower().contains(text);

    // Group filter
    bool group_match = true;
    if (group == "Enabled")
      group_match = m.enabled;
    else if (group == "Disabled")
      group_match = !m.enabled;
    else if (group == "Conflicts")
      group_match = (m.conflict_wins > 0 || m.conflict_losses > 0);
    else if (group == "FOMOD")
      group_match = m.is_fomod;
    else if (group == "Separators")
      group_match = false; // regular mods hidden when viewing separators only

    // Category filter (OR semantics): the mod matches when any of its
    // category ids is checked.
    bool category_match = true;
    if (category_filter_active) {
      category_match = false;
      for (int cid : m.category_ids) {
        if (checked_categories.contains(cid)) {
          category_match = true;
          break;
        }
      }
    }

    visible[row] = text_match && group_match && category_match;

    // If an active fold scope (folded separator band or folded mod subtree)
    // hides w_ row, hide it too - fold overrides search.
    if (visible[row] && group == "All" && fold_hidden[row]) {
      visible[row] = false;
    }
  }

  // Second pass: separators are shown only if at least one child mod is visible
  for (int row = 0; row < mods.size(); ++row) {
    if (!mods[row].is_separator)
      continue;

    if (group == "Separators") {
      visible[row] = true;
    } else if (text.isEmpty() && !category_filter_active &&
               (group == "All" || group == "Enabled" || group == "Disabled" ||
                group == "Conflicts")) {
      visible[row] = true;
    } else {
      // Scan children for any visible mod
      visible[row] = false;
      for (int j = row + 1; j < mods.size() && !mods[j].is_separator; ++j) {
        if (visible[j]) {
          visible[row] = true;
          break;
        }
      }
    }

    // A separator inside a folded scope (a folded parent's band or a
    // folded mod subtree) must STAY hidden: apply_fold_state() hid it and
    // the force-show branches above would otherwise re-show it (the bug
    // was nested separators staying visible under a folded parent).
    // Mirrors the fold-overrides-search rule applied to mod rows (which
    // only overrides when group == "All").
    if (visible[row] && group == "All" && fold_hidden[row]) {
      visible[row] = false;
    }
    w_->mod_view_->setRowHidden(row, QModelIndex(), !visible[row]);
  }

  // Nesting: a filtered-out ancestor (mod parent or separator) stays visible
  // while any of its subtree members matches, so the tree never breaks
  // mid-level under a filter. Fold-hidden rows are never re-shown (the fold
  // override above already hid their visible members, so they can't re-show
  // via a descendant either - the check keeps it airtight).
  if (w_->mod_model_->nesting_enabled()) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (int row = 0; row < mods.size(); ++row) {
        if (visible[row] || fold_hidden[row])
          continue;
        if (w_->mod_model_->has_visible_descendant(row, visible)) {
          visible[row] = true;
          changed = true;
        }
      }
    }
  }

  // Apply visibility to all mod rows
  for (int row = 0; row < mods.size(); ++row) {
    if (mods[row].is_separator)
      continue;
    w_->mod_view_->setRowHidden(row, QModelIndex(), !visible[row]);
  }
}

} // namespace ui

#include "moc_mod_list_controller.cpp"
