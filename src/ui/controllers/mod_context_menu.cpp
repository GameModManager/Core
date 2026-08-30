#include "ui/controllers/mod_context_menu.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/mod/meta/categories.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/overwrite/overwrite_utils.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "ui/controllers/mod_actions.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/controllers/overwrite_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QActionGroup>
#include <QDesktopServices>
#include <QMenu>

#include "engine/game/detect/mod_scanner.h"
#include "engine/source/source_provider.h"
#include "ui/theme/icon_manager.h"

namespace ui {

ModContextMenu::ModContextMenu(MainWindow *w, ModActions *actions)
    : w_(w), actions_(actions) {}

void ModContextMenu::set_on_data_mod_info(
    std::function<void(const QString &, int)> cb) {
  on_data_mod_info_cb_ = std::move(cb);
}

void ModContextMenu::set_source_visit_info(
    std::function<SourceVisitInfo(const QString &, const QString &,
                                  const QString &)>
        cb) {
  source_visit_info_cb_ = std::move(cb);
}

void ModContextMenu::setup_mod_list_context_menu() {
  w_->mod_view_->setContextMenuPolicy(Qt::CustomContextMenu);

  QObject::connect(
      w_->mod_view_, &QWidget::customContextMenuRequested, w_,
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
              QObject::tr("Sync to Mods..."), w_,
              [this]() { w_->overwrite_->sync_overwrite_to_mods(); });
          auto *create_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("document-new"),
              QObject::tr("Create Mod..."), w_,
              [this]() { w_->overwrite_->create_mod_from_overwrite(); });
          auto *move_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("go-down"),
              QObject::tr("Move content to Mod..."), w_,
              [this]() { w_->overwrite_->move_overwrite_content_to_mod(); });
          auto *clear_act = menu.addAction(
              engine::IconManager::instance().resolve_icon("edit-clear"),
              QObject::tr("Clear Overwrite..."), w_,
              [this]() { w_->overwrite_->clear_overwrite(); });
          for (auto *act : {sync_act, create_act, move_act, clear_act})
            act->setEnabled(has_content);

          menu.addAction(engine::IconManager::instance().resolve_icon("folder"),
                         QObject::tr("Open in File Manager"), w_, [this]() {
                           w_->overwrite_->open_overwrite_in_file_manager();
                         });

          menu.addSeparator();
          menu.addAction(engine::IconManager::instance().resolve_icon(
                             "dialog-information"),
                         QObject::tr("Information..."), w_, [this]() {
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
              QObject::tr("Rename Separator..."), w_,
              [this, row]() { actions_->rename_mod_inline(row); });
          menu.addAction(
              engine::IconManager::instance().resolve_icon("edit-delete"),
              QObject::tr("Remove Separator..."), w_,
              [this, row]() { actions_->delete_separator(row); });
          menu.addSeparator();
          menu.addAction(
              engine::IconManager::instance().resolve_icon("color-picker"),
              QObject::tr("Select Color..."), w_,
              [this]() { actions_->select_color_for_selected(); });
          if (!entry.separator_color.isEmpty()) {
            menu.addAction(
                engine::IconManager::instance().resolve_icon("edit-clear"),
                QObject::tr("Reset Color"), w_,
                [this]() { actions_->reset_color_for_selected(); });
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
              QObject::tr("Enable Selected"), w_,
              [this]() { actions_->toggle_selected_mods(true); });
          menu.addAction(
              engine::IconManager::instance().resolve_icon("dialog-cancel"),
              QObject::tr("Disable Selected"), w_,
              [this]() { actions_->toggle_selected_mods(false); });
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
                             QObject::tr("Tweaks"));
            auto *root_act =
                tweaks->addAction(QObject::tr("Treat mod as root dir"));
            root_act->setCheckable(true);
            const bool all_on = any_on && !any_off;
            root_act->setChecked(all_on);
            root_act->setEnabled(!rows.isEmpty());
            QObject::connect(root_act, &QAction::triggered, w_,
                             [this, rows, all_on]() {
                               actions_->toggle_root_override(rows, !all_on);
                             });
          }
          menu.addSeparator();
          menu.addAction(
              engine::IconManager::instance().resolve_icon("edit-delete"),
              QObject::tr("Remove"), w_,
              [this]() { actions_->remove_selected_mods(); });
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
            QObject::tr("Send to..."));
        send_to->addAction(
            engine::IconManager::instance().resolve_icon("go-top"),
            QObject::tr("Send to Highest Priority"), w_,
            [this, mod_id]() { actions_->send_to_highest_priority(mod_id); });
        send_to->addAction(
            engine::IconManager::instance().resolve_icon("go-bottom"),
            QObject::tr("Send to Lowest Priority"), w_,
            [this, mod_id]() { actions_->send_to_lowest_priority(mod_id); });
        bool any_seps = false;
        for (const auto &m : w_->mod_model_->mods())
          if (m.is_separator) {
            any_seps = true;
            break;
          }
        auto *sep_act = send_to->addAction(
            engine::IconManager::instance().resolve_icon("view-sort"),
            QObject::tr("Separator..."), w_,
            [this, mod_id]() { actions_->send_to_separator(mod_id); });
        sep_act->setEnabled(any_seps);
        if (!entry.separator_id.isEmpty() &&
            w_->mod_model_->has_conflicts_within_separator(mod_id)) {
          send_to->addAction(
              engine::IconManager::instance().resolve_icon("go-up"),
              QObject::tr("Send to Highest in Separator"), w_,
              [this, mod_id]() {
                actions_->send_to_highest_in_separator(mod_id);
              });
          send_to->addAction(
              engine::IconManager::instance().resolve_icon("go-down"),
              QObject::tr("Send to Lowest in Separator"), w_, [this, mod_id]() {
                actions_->send_to_lowest_in_separator(mod_id);
              });
        }

        menu.addAction(engine::IconManager::instance().resolve_icon("list-add"),
                       QObject::tr("Create Separator"), w_, [this, row]() {
                         actions_->create_separator_at_row(row);
                       });

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
              QObject::tr("Ignore missing data"), w_, [this, mod_id]() {
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
            QObject::tr("Enable Selected"), w_,
            [this]() { actions_->toggle_selected_mods(true); });
        menu.addAction(
            engine::IconManager::instance().resolve_icon("dialog-cancel"),
            QObject::tr("Disable Selected"), w_,
            [this]() { actions_->toggle_selected_mods(false); });

        menu.addSeparator();
        menu.addAction(
            engine::IconManager::instance().resolve_icon("document-edit"),
            QObject::tr("Rename Mod..."), w_,
            [this, row]() { actions_->rename_mod_inline(row); });

        // Tweaks submenu - per-mod deploy options (MO2's per-mod tweaks).
        {
          auto *tweaks = menu.addMenu(
              engine::IconManager::instance().resolve_icon("preferences-other"),
              QObject::tr("Tweaks"));
          auto *root_act =
              tweaks->addAction(QObject::tr("Treat mod as root dir"));
          root_act->setCheckable(true);
          root_act->setChecked(entry.root_override);
          root_act->setEnabled(!entry.is_separator && !entry.is_overwrite &&
                               !entry.is_merged && !entry.is_game_native);
          root_act->setStatusTip(
              QObject::tr("Deploy w_ mod's files to the game root "
                          "instead of the data dir"));
          QObject::connect(root_act, &QAction::triggered, w_, [this, row]() {
            actions_->toggle_root_override(
                {row}, !w_->mod_model_->mods()[row].root_override);
          });
        }

        menu.addSeparator();
        if (!entry.source_type.isEmpty() && source_visit_info_cb_) {
          auto src = source_visit_info_cb_(entry.source_type, entry.source_id,
                                           entry.source_page_url);
          if (!src.label.isEmpty()) {
            auto *visit_act = menu.addAction(
                engine::IconManager::instance().resolve_icon("text-html"),
                src.label, w_, [src]() {
                  if (!src.url.isEmpty())
                    QDesktopServices::openUrl(QUrl(src.url));
                });
            visit_act->setEnabled(!src.url.isEmpty());
          }
        }
        menu.addAction(
            engine::IconManager::instance().resolve_icon("folder"),
            QObject::tr("Open in File Manager"), w_, [this, mod_id]() {
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
            QObject::tr("Remove"), w_,
            [this]() { actions_->remove_selected_mods(); });

        // MO2 puts Information last (modlistcontextmenu.cpp:267-273), the
        // default action after all per-type actions.
        menu.addSeparator();
        menu.addAction(
            engine::IconManager::instance().resolve_icon("dialog-information"),
            QObject::tr("Information..."), w_, [this, mod_id]() {
              if (on_data_mod_info_cb_)
                on_data_mod_info_cb_(mod_id, -1);
            });

        menu.exec(w_->mod_view_->viewport()->mapToGlobal(pos));
      });
}

void ModContextMenu::add_category_menus(QMenu &menu, const QString &mod_id) {
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
    // Re-apply the filter via ModListController callback.
    // This is a bit awkward — we need ModListController to call
    // apply_mod_filter. For now, we'll emit a signal or use a callback.
    // TODO: This should be wired through ModListController.
  };

  const QVector<int> current = load_current();

  // "Change Categories": one checkable action per category, alphabetized like
  // the filter panel (MO2's flat category list). Checking appends the id
  // (first checked becomes primary); unchecking removes it and the first
  // remaining id becomes primary.
  auto *change_menu = menu.addMenu(
      engine::IconManager::instance().resolve_icon("preferences-other"),
      QObject::tr("Change Categories"));
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
    QObject::connect(act, &QAction::triggered, w_,
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
                   QObject::tr("Primary Category"));
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
      QObject::connect(act, &QAction::triggered, w_,
                       [this, mod_id, id, load_current, apply]() {
                         QVector<int> ids = load_current();
                         ids.removeAll(id);
                         ids.prepend(id);
                         apply(ids);
                       });
    }
  }
}

} // namespace ui
