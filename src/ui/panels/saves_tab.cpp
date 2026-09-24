#include "ui/panels/saves_tab.h"
#include "ui/panels/panel_utils.h"

#include "engine/core/log/logger.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QShowEvent>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace ui {

// --- SavesTab ---
QString SavesTab::missing_tooltip(const SavesScanResultEntry &entry) {
  QStringList lines;
  for (const auto &asset : entry.missing) {
    QString line = QString::fromStdString(asset.plugin_name);
    if (asset.inactive) {
      line += tr(" (disabled)");
    } else if (!asset.providing_mods.empty()) {
      QStringList providers;
      for (const auto &m : asset.providing_mods)
        providers << QString::fromStdString(m);
      line += " - " + providers.join(", ");
    }
    lines << line;
  }
  return lines.isEmpty() ? tr("No missing plugins") : lines.join('\n');
}

QString SavesTab::file_tooltip(const engine::SaveGame &save) {
  QString tip = QString::fromStdString(save.file_path.string());
  // Unparsed stub (Workspace-e2td: no parser, e.g. Isaac): the row's only
  // metadata is file identity, so surface size + modified date here. Parsed
  // saves keep the plain path tooltip - their metadata lives in the hover.
  if (save.pc_name.empty() && save.pc_location.empty()) {
    const QString when =
        save.creation_time > 0
            ? QLocale().toString(QDateTime::fromSecsSinceEpoch(save.creation_time),
                                 QLocale::ShortFormat)
            : tr("unknown date");
    tip += "\n" + tr("%1 bytes, modified %2").arg(save.file_size).arg(when);
  }
  return tip;
}

void SavesTab::update_empty_state() {
  const bool empty = table_->rowCount() == 0;
  if (empty) {
    if (saves_.saves_dir.empty()) {
      empty_label_->setText(tr("Save location unavailable."));
    } else {
      empty_label_->setText(
          tr("No saves found in %1")
              .arg(QString::fromStdString(saves_.saves_dir.string())));
    }
  }
  empty_label_->setVisible(empty);
}

SavesTab::SavesTab(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  table_ = make_table(3, {tr("Name"), tr("File"), tr("Missing")}, this);
  table_->setObjectName("savesTable");
  table_->setMouseTracking(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  layout->addWidget(table_, 1);

  // Empty-state label (Workspace-e2td): shown when a landed scan has zero
  // rows so a missing/empty saves dir (e.g. Isaac userdata with no saves
  // yet) degrades to a helpful message instead of a bare empty table.
  // Hidden until the first landed result - the tab scans lazily on first
  // show, so "no rows yet" initially means "not scanned", not "no saves".
  empty_label_ = new QLabel(this);
  empty_label_->setWordWrap(true);
  empty_label_->setAlignment(Qt::AlignCenter);
  empty_label_->setVisible(false);
  layout->addWidget(empty_label_);

  // Debounced directory watch (Workspace-69xt, MO2 savestab.cpp:21-27
  // parity): directoryChanged restarts a 500ms single-shot; the timeout
  // re-scans only while the tab is visible (refreshSavesIfOpen). The old
  // watcher was removed for spamming 1-per-second re-scans on Proton-prefix
  // churn; the visible-only gate plus debounce plus the in-flight coalescer
  // keep that bounded - hidden tabs never scan.
  dir_watcher_     = new QFileSystemWatcher(this);
  rescan_debounce_ = new QTimer(this);
  rescan_debounce_->setSingleShot(true);
  rescan_debounce_->setInterval(500);
  connect(dir_watcher_, &QFileSystemWatcher::directoryChanged, this,
          [this](const QString &) {
            rescan_debounce_->start();
          });
  connect(rescan_debounce_, &QTimer::timeout, this, &SavesTab::on_watcher_timeout);
  scan_thread_ = new SavesScanThread(this);
  // Per-save streaming (Workspace-0owv): one queued insert per parsed save
  // so the user sees rows fill in as they load. The final finished(int)
  // signal flips scanning_=false and drains any coalesced pending request.
  connect(scan_thread_->worker(), &SavesScanWorker::entryReady, this,
          &SavesTab::on_entry_ready, Qt::QueuedConnection);
  connect(scan_thread_->worker(), &SavesScanWorker::finished, this,
          &SavesTab::on_scan_complete, Qt::QueuedConnection);

  connect(table_, &QTableWidget::itemEntered, this, &SavesTab::on_item_entered);
  connect(table_, &QTableWidget::itemSelectionChanged, this,
          &SavesTab::on_selection_changed);
  connect(table_, &QTableWidget::customContextMenuRequested, this,
          &SavesTab::on_context_menu);

  table_->viewport()->installEventFilter(this);
  table_->installEventFilter(this);
}

SavesTab::~SavesTab() {
  delete info_popup_;
}

void SavesTab::set_saves_dir(const std::filesystem::path &dir) {
  if (dir_watcher_ && !saves_dir_.empty()) {
    dir_watcher_->removePath(QString::fromStdString(saves_dir_.string()));
  }
  saves_dir_ = dir;
  // New game/instance, new saves: the lazy-scan latch belongs to the old
  // dir, so the next first-show scans again (Workspace-ugm3).
  scanned_once_ = false;
  // MO2 startMonitorSaves parity: (re)watch the current saves dir. A
  // missing dir cannot be watched; the latch scan on show covers that case
  // and the watch arms once the dir exists and the dir is (re)set.
  if (dir_watcher_ && !saves_dir_.empty()) {
    std::error_code ec;
    if (std::filesystem::is_directory(saves_dir_, ec)) {
      dir_watcher_->addPath(QString::fromStdString(saves_dir_.string()));
    }
  }
}

void SavesTab::set_saves(SavesScanResult result) {
  // Batch path (kept for tests/ui/saves_tab_test.cpp which build a result
  // directly). Production scans now go through on_entry_ready + binary
  // insert. We rely on saves_.entries already being sorted newest-first
  // (the scanner's contract) so a single sequential insert is equivalent
  // to the streaming path.
  scanning_ = false;
  // Free the previous scan's entries (screenshots included) BEFORE taking
  // the new result, so peak memory never holds two full save lists at once
  // (Workspace-x5zg).
  saves_ = {};
  saves_ = std::move(result);
  if (saves_.entries.size() > kMaxSavesRetain) {
    // Batch path receives newest-first (the scanner's contract), so the
    // tail past the cap is the oldest - drop it with its screenshots.
    saves_.entries.erase(saves_.entries.begin() + kMaxSavesRetain,
                         saves_.entries.end());
  }
  table_->setRowCount(0);
  table_->setRowCount(static_cast<int>(saves_.entries.size()));
  engine::Logger::instance().debug(
      "Saves scan landed: " + std::to_string(saves_.entries.size()) + " save(s) from " +
      saves_.saves_dir.string());
  for (int row = 0; row < saves_.entries.size(); ++row) {
    const auto &entry = saves_.entries[row];
    auto *name =
        new QTableWidgetItem(QString::fromStdString(entry.save.display_name()));
    auto *file = new QTableWidgetItem(
        QString::fromStdString(entry.save.file_path.filename().string()));
    file->setToolTip(file_tooltip(entry.save));
    int missing = static_cast<int>(entry.missing.size());
    auto *miss =
        new QTableWidgetItem(missing > 0 ? QString::number(missing) : QString());
    miss->setToolTip(missing_tooltip(entry));
    table_->setItem(row, kColumnName, name);
    table_->setItem(row, kColumnFile, file);
    table_->setItem(row, kColumnMissing, miss);
  }
  hide_save_info();
  update_empty_state();
}

void SavesTab::clear_saves() {
  scanning_     = false;
  scanned_once_ = false;
  if (dir_watcher_) {
    dir_watcher_->removePaths(dir_watcher_->directories());
  }
  if (rescan_debounce_) {
    rescan_debounce_->stop();
  }
  saves_ = {};
  table_->setRowCount(0);
  hide_save_info();
  empty_label_->setVisible(false);
}

void SavesTab::on_watcher_timeout() {
  // MO2 refreshSavesIfOpen parity: a change on disk re-scans only while
  // the user is looking at the tab. Hidden tabs stay quiet no matter how
  // much the watched dir churns.
  if (!isVisible() || saves_dir_.empty()) {
    return;
  }
  emit scan_requested();
}

void SavesTab::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  ensure_scanned();
}

void SavesTab::ensure_scanned() {
  if (scanned_once_)
    return;
  scanned_once_ = true;
  emit scan_requested();
}

const engine::SaveGame *SavesTab::save_at(int row) const {
  if (row < 0 || row >= saves_.entries.size())
    return nullptr;
  return &saves_.entries[row].save;
}

const std::vector<engine::SaveMissingAsset> *SavesTab::missing_at(int row) const {
  if (row < 0 || row >= saves_.entries.size())
    return nullptr;
  return &saves_.entries[row].missing;
}

void SavesTab::request_scan(SavesScanRequest request) {
  if (scanning_) {
    // Coalesce: the in-flight scan will pick up the latest request when it
    // finishes (k53a). A second request arriving during a scan replaces
    // any earlier pending one - the user only ever cares about the most
    // recent state, not every intermediate one.
    pending_request_ = std::move(request);
    return;
  }
  // Drop any stale rows so the streaming insert starts on a clean table.
  // A subsequent request_scan while scanning_=true does NOT clear (it
  // coalesces); clearing only on the new-scan branch keeps any in-flight
  // stream intact until the worker emits its first entryReady.
  saves_           = {};
  saves_.saves_dir = request.saves_dir;
  table_->setRowCount(0);
  empty_label_->setVisible(false);
  scanning_ = true;
  scan_thread_->start(std::move(request));
}

void SavesTab::on_entry_ready(std::shared_ptr<SavesScanResultEntry> entry, int done,
                              int total) {
  if (!entry)
    return;
  // Binary search by creation_time desc (matches the scanner's sort).
  // saves_.entries and the table are kept in lockstep so save_at(row) and
  // missing_at(row) stay correct as the table grows.
  const auto t = entry->save.creation_time;
  int lo       = 0;
  int hi       = saves_.entries.size();
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (saves_.entries[mid].save.creation_time > t) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const int row = lo;
  table_->insertRow(row);
  saves_.entries.insert(saves_.entries.begin() + row, std::move(*entry));
  const auto &inserted = saves_.entries[row];
  auto *name =
      new QTableWidgetItem(QString::fromStdString(inserted.save.display_name()));
  auto *file = new QTableWidgetItem(
      QString::fromStdString(inserted.save.file_path.filename().string()));
  file->setToolTip(file_tooltip(inserted.save));
  const int missing = static_cast<int>(inserted.missing.size());
  auto *miss = new QTableWidgetItem(missing > 0 ? QString::number(missing) : QString());
  miss->setToolTip(missing_tooltip(inserted));
  table_->setItem(row, kColumnName, name);
  table_->setItem(row, kColumnFile, file);
  table_->setItem(row, kColumnMissing, miss);
  // Retain cap (Workspace-x5zg): entries stay sorted newest-first, so the
  // tail is the oldest - evict it (with its screenshot) to bound memory.
  while (saves_.entries.size() > kMaxSavesRetain) {
    saves_.entries.removeLast();
    table_->removeRow(table_->rowCount() - 1);
  }
  // Hide the hover popup if the inserted row pushed the previously-hovered
  // row off-position. Cheaper than recomputing: the next mouse move will
  // re-show it via itemEntered.
  if (info_popup_ && row <= 0) {
    hide_save_info();
  }
  (void)done;
  (void)total;
}

void SavesTab::on_scan_complete(int count) {
  scanning_ = false;
  engine::Logger::instance().debug("Saves scan landed: " + std::to_string(count) +
                                   " save(s) from " + saves_.saves_dir.string());
  hide_save_info();
  if (pending_request_) {
    auto next = std::move(*pending_request_);
    pending_request_.reset();
    request_scan(std::move(next));
  } else {
    update_empty_state();
  }
}

void SavesTab::on_item_entered(QTableWidgetItem *item) {
  if (!item)
    return;
  show_save_info(item->row());
}

void SavesTab::on_selection_changed() {
  const auto selected = table_->selectionModel()->selectedRows();
  if (selected.isEmpty()) {
    hide_save_info();
    return;
  }
  show_save_info(selected.first().row());
}

// Workspace-de5v: lazy heavy data. Scan entries carry header + plugins only
// (screenshot stripped by the worker). On first hover/selection, re-parse
// the full save through the registry and cache it in place so later hovers
// are free. Runs on the main thread - one file, ms-scale, same as MO2's
// on-demand parse. A failed re-parse keeps the header-only entry so the
// popup still shows what the scan found.
void SavesTab::ensure_heavy_data(int row) {
  if (row < 0 || row >= saves_.entries.size())
    return;
  auto &save = saves_.entries[row].save;
  if (save.has_heavy_data)
    return;
  try {
    auto full =
        engine::SaveParserRegistry::instance().parse_save(save.file_path, save.game_id);
    if (full) {
      save = std::move(*full);
    }
  } catch (...) {
  }
}

void SavesTab::show_save_info(int row) {
  ensure_heavy_data(row);
  const auto *save = save_at(row);
  if (!save)
    return;
  const auto *missing = missing_at(row);

  hide_save_info();
  auto *popup = new QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
  popup->setAttribute(Qt::WA_DeleteOnClose);
  info_popup_ = popup;

  auto *v = new QVBoxLayout(popup);
  v->setContentsMargins(8, 8, 8, 8);
  v->setSpacing(2);

  // Screenshot (MO2 GamebryoSaveGameInfoWidget::screenshotLabel).
  if (save->screenshot_width > 0 && save->screenshot_height > 0 &&
      save->screenshot.size() >= static_cast<std::size_t>(save->screenshot_width) *
                                     save->screenshot_height * 3) {
    const bool rgba =
        save->screenshot.size() ==
        static_cast<std::size_t>(save->screenshot_width) * save->screenshot_height * 4;
    QImage img(static_cast<const uchar *>(save->screenshot.data()),
               save->screenshot_width, save->screenshot_height,
               rgba ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    auto *shot = new QLabel(popup);
    shot->setPixmap(QPixmap::fromImage(img));
    v->addWidget(shot);
  }

  const auto add_row = [&](const QString &label, const QString &value) {
    auto *row = new QLabel(QString("<b>%1</b> %2").arg(label, value), popup);
    v->addWidget(row);
  };
  add_row(tr("Character:"), QString::fromStdString(save->pc_name));
  add_row(tr("Level:"), QString::number(save->pc_level));
  add_row(tr("Location:"), QString::fromStdString(save->pc_location));
  add_row(tr("Save #:"), QString::number(save->save_number));
  add_row(tr("Time:"),
          QLocale().toString(QDateTime::fromSecsSinceEpoch(save->creation_time),
                             QLocale::ShortFormat));

  // v2.1+ save overlay: per-game plugin-supplied kv rows (MO2
  // ISaveGameInfoWidget parity). The plugin that registered the parser
  // for this game can supply extra metadata (e.g. quest stage, weather,
  // current cell). Only shown when the overlay is non-empty; otherwise
  // the default metadata view (above) is the only info. The bare
  // "Details" header was floating with no context - removed; the row
  // keys already self-describe.
  if (!save->overlay.empty()) {
    for (const auto &row : save->overlay) {
      v->addWidget(
          new QLabel(QString("    <b>%1</b> %2")
                         .arg(QString::fromStdString(row.key).toHtmlEscaped(),
                              QString::fromStdString(row.value).toHtmlEscaped()),
                     popup));
    }
  }

  if (save->has_script_extender_file()) {
    auto *skse = new QLabel(tr("<b>Has Script Extender Data</b>"), popup);
    v->addWidget(skse);
  }

  auto *header = new QLabel(tr("<i>Missing ESPs</i>"), popup);
  v->addWidget(header);
  int shown = 0;
  if (missing && !missing->empty()) {
    for (const auto &asset : *missing) {
      if (shown >= 7)
        break;
      ++shown;
      auto *label = new QLabel(
          QString("    %1").arg(QString::fromStdString(asset.plugin_name)), popup);
      v->addWidget(label);
    }
    if (static_cast<std::size_t>(shown) < missing->size()) {
      v->addWidget(new QLabel("...", popup));
    }
  } else {
    v->addWidget(new QLabel(tr("    None"), popup));
  }

  popup->adjustSize();
  popup->setWindowOpacity(
      popup->style()->styleHint(QStyle::SH_ToolTipLabel_Opacity, nullptr, popup) /
      qreal(255.0));

  // Position near the cursor, flipped when it would leave the screen
  // (MO2 displaySaveGameInfo geometry logic).
  QPoint pos         = QCursor::pos();
  const QRect screen = QGuiApplication::screenAt(pos)
                           ? QGuiApplication::screenAt(pos)->geometry()
                           : QGuiApplication::primaryScreen()->geometry();
  if (pos.x() + popup->width() > screen.right()) {
    pos.setX(pos.x() - popup->width() - 2);
  } else {
    pos.setX(pos.x() + 5);
  }
  if (pos.y() + popup->height() > screen.bottom()) {
    pos.setY(pos.y() - popup->height() - 10);
  } else {
    pos.setY(pos.y() + 20);
  }
  popup->move(pos);
  popup->show();
}

void SavesTab::hide_save_info() {
  if (info_popup_) {
    info_popup_->close();
    info_popup_ = nullptr;
  }
}

bool SavesTab::eventFilter(QObject *object, QEvent *event) {
  if (object == table_ || object == table_->viewport()) {
    if (event->type() == QEvent::Leave || event->type() == QEvent::WindowDeactivate) {
      hide_save_info();
    } else if (event->type() == QEvent::KeyPress) {
      auto *key = static_cast<QKeyEvent *>(event);
      if (key->key() == Qt::Key_Delete) {
        on_delete_key();
        return true;
      }
    }
  }
  return QWidget::eventFilter(object, event);
}

void SavesTab::on_delete_key() {
  const auto selected = table_->selectionModel()->selectedRows();
  if (selected.isEmpty())
    return;

  QStringList delete_files;
  QStringList label_rows;
  for (const auto &idx : selected) {
    const auto *save = save_at(idx.row());
    if (!save)
      continue;
    delete_files << QString::fromStdString(save->file_path.string());
    label_rows << save->file_path.filename().string().c_str();
    // The script-extender co-save travels with its save (MO2 allFiles()).
    const auto skse          = save->file_path;
    std::filesystem::path co = skse;
    co.replace_extension("skse");
    if (std::filesystem::is_regular_file(co)) {
      delete_files << QString::fromStdString(co.string());
    }
  }
  if (delete_files.isEmpty())
    return;

  QString msg;
  const int total = label_rows.size();
  for (int i = 0; i < std::min(10, total); ++i) {
    msg += "<li>" + label_rows[i].toHtmlEscaped() + "</li>";
  }
  if (total > 10) {
    msg += "<li><i>... " + tr("%1 more").arg(total - 10) + "</i></li>";
  }
  const auto answer = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove the following %n save(s)?<br>"
         "<ul>%1</ul><br>Removed saves will be sent to the trash.",
         "", total)
          .arg(msg),
      QMessageBox::Yes | QMessageBox::No);
  if (answer == QMessageBox::Yes) {
    emit delete_requested(delete_files);
  }
}

void SavesTab::on_context_menu(const QPoint &pos) {
  const auto selected = table_->selectionModel()->selectedRows();
  if (selected.isEmpty())
    return;

  QMenu menu(this);
  // Information... only makes sense for a single row: it opens a dialog
  // showing that save's plugins/basic info. For multi-select it's hidden,
  // matching MO2's "Open in Explorer" / detail windows that always target
  // a single save.
  auto *info_action =
      menu.addAction(tr("Information..."), this, &SavesTab::on_information_action);
  info_action->setEnabled(selected.size() == 1);
  menu.addSeparator();
  menu.addAction(tr("Delete %n save(s)", "", selected.size()), this,
                 &SavesTab::on_delete_key);
  menu.addAction(tr("Open in file manager"), this, [this] {
    const auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty())
      return;
    const auto *save = save_at(sel.first().row());
    if (!save)
      return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromStdString(save->file_path.parent_path().string())));
  });
  menu.exec(table_->viewport()->mapToGlobal(pos));
}

void SavesTab::on_information_action() {
  const auto sel = table_->selectionModel()->selectedRows();
  if (sel.size() != 1)
    return;
  // Right-click "Information..." can fire without a prior hover, so load
  // the screenshot here too - the dialog renders it from save_at(row).
  ensure_heavy_data(sel.first().row());
  emit information_requested(sel.first().row());
}

}  // namespace ui
