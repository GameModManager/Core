#include "ui/panels/downloads_tab.h"
#include "ui/panels/panel_utils.h"
#include "ui/settings/settings.h"

#include "engine/log/logger.h"
#include "engine/util/fs_utils.h"
#include "engine/source/loverslab/provider.h"
#include "ui/theme/icon_manager.h"
#include "ui/widgets/task_dialog.h"
#include "ui/widgets/web_link.h"

#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace ui {

// Helper: state to display string
static QString state_label(DownloadState s) {
  switch (s) {
  case DownloadState::Downloading:
    return QCoreApplication::translate("DownloadsTab", "Downloading");
  case DownloadState::Paused:
    return QCoreApplication::translate("DownloadsTab", "Paused");
  case DownloadState::Complete:
    return QCoreApplication::translate("DownloadsTab", "Install");
  case DownloadState::Installed:
    return QCoreApplication::translate("DownloadsTab", "Installed");
  case DownloadState::Failed:
    return QCoreApplication::translate("DownloadsTab", "Failed");
  case DownloadState::Removed:
    return QCoreApplication::translate("DownloadsTab", "Removed");
  }
  return QCoreApplication::translate("DownloadsTab", "Unknown");
}

// Archive extensions the Downloads tab treats as installable: the untracked
// scan surfaces them and external drops (MO2-style) import them into the
// downloads dir.
static const std::vector<std::string> &download_archive_exts() {
  static const std::vector<std::string> exts = {".zip", ".7z",  ".tar", ".rar",
                                                ".gz",  ".bz2", ".xz",  ".fomod"};
  return exts;
}

// Case-insensitive check: does `path` carry one of the supported archive
// extensions?
static bool is_archive_path(const std::filesystem::path &path) {
  std::string lower;
  for (char c : path.extension().string())
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  const auto &exts = download_archive_exts();
  return std::find(exts.begin(), exts.end(), lower) != exts.end();
}

// MO2's downloads-tab drag gate: accept only when every URL is a local file
// with a supported archive extension (a single foreign URL rejects the whole
// drag). Non-local (http etc.) URLs are not handled by the tab.
static bool accepts_url_drop(const QMimeData *data) {
  if (!data || !data->hasUrls())
    return false;
  const auto urls = data->urls();
  if (urls.isEmpty())
    return false;

  for (const auto &url : urls) {
    if (!url.isLocalFile())
      return false;
    if (!is_archive_path(url.toLocalFile().toStdString()))
      return false;
  }
  return true;
}

// A drag carrying exactly one LoversLab download link (a browser URL drag) is
// routed to the same "Add from URL…" flow as the header button. Browsers drag
// URLs as text/uri-list (surfaced through QMimeData::urls()); a bare-text
// fallback covers text-only drags. Returns the URL, or "" when the drop is not
// a single LoversLab link (so the archive-drop gate below still applies).
static std::string loverslab_drop_url(const QMimeData *data) {
  if (!data)
    return {};
  QString candidate;
  if (data->hasUrls()) {
    const auto urls = data->urls();
    if (urls.size() != 1)
      return {};
    candidate = urls.first().toString();
  } else if (data->hasText()) {
    candidate = data->text().trimmed();
  } else {
    return {};
  }
  if (candidate.isEmpty())
    return {};
  const std::string url = candidate.toStdString();
  return engine::Source::LoversLab::Provider::is_loverslab_url(url) ? url
                                                                    : std::string();
}

// --- DownloadsTab ---

DownloadShortcut download_shortcut_for(DownloadState state, int qt_key) {
  switch (state) {
  case DownloadState::Complete:
  case DownloadState::Installed:
    if (qt_key == Qt::Key_Enter || qt_key == Qt::Key_Return)
      return DownloadShortcut::Install;
    if (qt_key == Qt::Key_Delete)
      return DownloadShortcut::Remove;
    break;
  case DownloadState::Downloading:
    if (qt_key == Qt::Key_Delete)
      return DownloadShortcut::Remove;
    if (qt_key == Qt::Key_Space)
      return DownloadShortcut::Pause;
    break;
  case DownloadState::Paused:
  case DownloadState::Failed:
    if (qt_key == Qt::Key_Delete)
      return DownloadShortcut::Remove;
    if (qt_key == Qt::Key_Space)
      return DownloadShortcut::Resume;
    break;
  case DownloadState::Removed:
    break;
  }
  return DownloadShortcut::None;
}

QString format_remaining_time(int64_t remaining_bytes, double bytes_per_second) {
  // No speed measured yet, nothing left to fetch, or a nonsensical input:
  // there is no estimate to show.
  if (bytes_per_second <= 0.0 || remaining_bytes <= 0)
    return {};

  const auto seconds = static_cast<int64_t>(remaining_bytes / bytes_per_second);
  // Below a second the estimate is noise and would render as "0s".
  if (seconds < 1)
    return {};

  // Largest unit that fits, so the cell never grows past four characters.
  if (seconds >= 86400)
    return QString::number(seconds / 86400) + "d";
  if (seconds >= 3600)
    return QString::number(seconds / 3600) + "h";
  if (seconds >= 60)
    return QString::number(seconds / 60) + "m";
  return QString::number(seconds) + "s";
}

bool download_in_batch(DownloadState state, DownloadBatch batch) {
  if (state == DownloadState::Removed)
    return false;
  if (batch == DownloadBatch::All)
    return true;
  // An in-flight row has not finished, so it is neither installed nor
  // uninstalled - MO2's issueDeleteCompleted / issueDeleteUninstalled skip
  // anything below STATE_READY.
  if (state == DownloadState::Downloading || state == DownloadState::Paused)
    return false;
  const bool installed = state == DownloadState::Installed;
  return batch == DownloadBatch::Installed ? installed : !installed;
}

QString download_batch_label(DownloadBatch batch, bool remove) {
  switch (batch) {
  case DownloadBatch::Installed:
    return remove ? QObject::tr("Delete Installed Downloads...")
                  : QObject::tr("Hide Installed...");
  case DownloadBatch::Uninstalled:
    return remove ? QObject::tr("Delete Uninstalled Downloads...")
                  : QObject::tr("Hide Uninstalled...");
  case DownloadBatch::All:
    return remove ? QObject::tr("Delete All Downloads...") : QObject::tr("Hide All...");
  }
  return {};
}

// MO2's header labels (downloadlist.cpp:76-91) for the columns we carry.
QString DownloadsTab::column_name(int column) {
  switch (column) {
  case Name:
    return tr("Name");
  case Source:
    return tr("Source");
  case Status:
    return tr("Status");
  case Size:
    return tr("Size");
  case Filetime:
    return tr("Filetime");
  case NexusId:
    return tr("Nexus ID");
  }
  return {};
}

QStringList DownloadsTab::column_names() {
  QStringList names;
  for (int c = 0; c < ColumnCount; ++c)
    names << column_name(c);
  return names;
}

QStringList DownloadsTab::header_tooltips() {
  // MO2's download list has no header tooltips (downloadlist.cpp's
  // headerData only answers DisplayRole; its single ToolTipRole is a cell
  // role at line 214). These describe what this build renders, including
  // when a column is legitimately blank.
  return {
      tr("Name the downloaded file is known by. A Nexus download shows a "
         "placeholder until the file name resolves."),
      tr("Site the file was downloaded from."),
      tr("Progress of the download, and what can be done with it."),
      tr("Archive size, or the current download speed while the file is "
         "being fetched."),
      tr("When the archive landed in the downloads folder. Empty until the "
         "download finishes."),
      tr("Mod ID on Nexus Mods. Empty for anything not downloaded from "
         "Nexus."),
  };
}

QStringList DownloadsTab::default_hidden_column_names() {
  // MO2 shows six of its eight columns on a fresh profile and hides
  // COL_MODNAME, COL_VERSION, COL_ID and COL_SOURCEGAME
  // (downloadlistview.cpp:147-151), which leaves Filetime visible. This build
  // ships Name, Status and Size alone, so Source and Filetime are hidden here
  // too - a deliberate departure from MO2, reachable from the header's toggle
  // menu.
  return {column_name(Source), column_name(Filetime), column_name(NexusId)};
}

void DownloadsTab::apply_default_hidden_columns(QTableWidget *table) {
  if (!table || !table->horizontalHeader())
    return;
  const QStringList hidden_names = default_hidden_column_names();
  // Set both ways, not just hide: a saved state records what the columns
  // looked like under the previous default, so a column that is not in the
  // set has to be un-hidden for the new default to take effect at all.
  for (int c = 0; c < ColumnCount; ++c) {
    const bool hidden = hidden_names.contains(column_name(c));
    if (table->horizontalHeader()->isSectionHidden(c) != hidden)
      table->horizontalHeader()->setSectionHidden(c, hidden);
  }
}

DownloadsTab::DownloadsTab(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *top = new QHBoxLayout;
  top->setContentsMargins(4, 2, 4, 2);
  hide_installed_ = new QCheckBox(tr("Hide installed"), this);
  top->addWidget(hide_installed_);
  top->addStretch(1);
  auto *add_url_btn = new QPushButton(tr("Add from URL…"), this);
  add_url_btn->setObjectName("addUrlBtn");
  top->addWidget(add_url_btn);
  layout->addLayout(top);

  table_ = make_table(ColumnCount, column_names(), this);
  table_->setObjectName("downloadsTable");

  // Positional: every header needs an entry or the tooltips after the gap
  // sit on the wrong column.
  const QStringList tooltips = header_tooltips();
  for (int c = 0; c < ColumnCount; ++c) {
    if (auto *header_item = table_->horizontalHeaderItem(c))
      header_item->setToolTip(tooltips.value(c));
  }
  // Resolved by name, so a renamed or retired entry in the list is inert
  // rather than hiding whatever happens to sit at that index.
  apply_default_hidden_columns(table_);

  table_->setWhatsThis(
      tr("This is a list of the mods this instance has downloaded. Right-click a "
         "finished download to install it. You can also drop a download link from "
         "your browser onto this tab to start the download."));
  layout->addWidget(table_, 1);
  apply_compact_style();

  connect(table_, &QTableWidget::cellDoubleClicked, this,
          &DownloadsTab::on_cell_double_clicked);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(table_, &QTableWidget::customContextMenuRequested, this,
          &DownloadsTab::on_custom_context_menu);
  // Enter / Delete / Space on the selected row (MO2
  // DownloadListView::keyPressEvent). The table is the watched object, so
  // arrow keys and editing still reach Qt.
  table_->installEventFilter(this);
  connect(hide_installed_, &QCheckBox::toggled, this, [this](bool checked) {
    Settings::instance().set_hide_installed_downloads(checked);
    apply_installed_filter();
  });

  hide_installed_->setChecked(Settings::instance().hide_installed_downloads());

  connect(add_url_btn, &QPushButton::clicked, this, [this]() {
    bool ok           = false;
    const QString url = QInputDialog::getText(
        this, tr("Download from URL"),
        tr("Paste a download link from a supported source:\n"
           "Right-click a file's Download button and copy the link address"),
        QLineEdit::Normal, QString(), &ok);
    if (ok && !url.trimmed().isEmpty()) {
      emit loverslab_url_entered(url.trimmed().toStdString());
    }
  });

  // External archive drops (drag from a file manager) land in the downloads
  // dir and surface as "Manual" entries. The QTableWidget itself does not
  // accept drops, so events propagate up to this widget.
  setAcceptDrops(true);

  // Default conflict resolver: MO2-style question dialog
  // (Overwrite / Rename new file / Ignore file).
  conflict_resolver_ = [this](const std::filesystem::path &existing,
                              const std::filesystem::path &dropped) {
    QMessageBox box(QMessageBox::Question,
                    QString::fromStdString(dropped.filename().string()),
                    tr("A file with the same name has already been "
                       "downloaded. What would you like to do?"));
    box.addButton(tr("Overwrite"), QMessageBox::ActionRole);
    box.addButton(tr("Rename new file"), QMessageBox::YesRole);
    box.addButton(tr("Ignore file"), QMessageBox::RejectRole);
    box.exec();
    switch (box.buttonRole(box.clickedButton())) {
    case QMessageBox::RejectRole:
      return DropConflictAction::Ignore;
    case QMessageBox::ActionRole:
      return DropConflictAction::Overwrite;
    default:
    case QMessageBox::YesRole:
      return DropConflictAction::Rename;
    }
  };

  // Watch the downloads dir so external changes (file-manager copies,
  // modifier drops, removals) surface immediately. directoryChanged fires
  // for any add/remove/rename; the single-shot timer coalesces bursts (Qt
  // may emit several events for a single operation).
  dir_watcher_ = new QFileSystemWatcher(this);
  scan_timer_  = new QTimer(this);
  scan_timer_->setSingleShot(true);
  scan_timer_->setInterval(200);
  connect(dir_watcher_, &QFileSystemWatcher::directoryChanged, this,
          &DownloadsTab::on_downloads_dir_changed);
  connect(scan_timer_, &QTimer::timeout, this, &DownloadsTab::on_scan_timer_timeout);
}

void DownloadsTab::apply_compact_style() {
  const bool compact = Settings::instance().compact_downloads();
  table_->setProperty("compact", compact);
  // Explicit heights: independent of any stylesheet/theme, so compact always
  // applies at launch. The dynamic property is kept for theme diagnostics.
  const int h = row_height();
  table_->verticalHeader()->setMinimumSectionSize(h);
  table_->verticalHeader()->setDefaultSectionSize(h);
  for (int r = 0; r < table_->rowCount(); ++r) {
    table_->setRowHeight(r, h);
  }
}

int DownloadsTab::row_height() const {
  const bool compact = Settings::instance().compact_downloads();
  const int base     = table_->fontMetrics().height();
  return compact ? qMax(24, base + 8) : qMax(40, base + 22);
}

void DownloadsTab::on_downloads_dir_changed() {
  // Re-arm the debounce timer; a pending scan is deferred so burst events
  // (e.g. a large copy) still coalesce into one refresh.
  if (scan_timer_->isActive())
    scan_timer_->stop();
  scan_timer_->start();
  engine::Logger::instance().debug("downloads: dir changed, scan scheduled (entries=" +
                                   std::to_string(downloads_.size()) + ")");
}

void DownloadsTab::on_scan_timer_timeout() {
  engine::Logger::instance().debug("downloads: scan timer fired");
  scan_downloads_dir();
}

void DownloadsTab::set_conflict_resolver(ConflictResolver resolver) {
  conflict_resolver_ = std::move(resolver);
}

bool DownloadsTab::add_download(const std::string &id, const std::string &name,
                                const std::string &source,
                                const std::filesystem::path &file_path,
                                const std::string &nexus_domain, int file_id,
                                const std::string &parent_mod_id,
                                const std::string &page_url) {
  auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
  if (!inserted)
    return false;  // already exists

  auto &entry = it->second;
  // Append at the end of the table. Do NOT keep a monotonic row counter:
  // remove_entry() reindexes survivors downward, so a stale counter would
  // point past the table end after entries are removed (rows then never
  // surface - the reported "dead tab after removing entries" bug).
  entry.row           = table_->rowCount();
  entry.file_path     = file_path;
  entry.state         = DownloadState::Downloading;
  entry.nexus_domain  = nexus_domain;
  entry.file_id       = file_id;
  entry.parent_mod_id = parent_mod_id;
  entry.page_url      = page_url;

  table_->insertRow(entry.row);

  entry.name_item = new QTableWidgetItem(QString::fromStdString(name));
  entry.name_item->setFlags(entry.name_item->flags() & ~Qt::ItemIsEditable);
  table_->setItem(entry.row, Name, entry.name_item);

  entry.source_item = new QTableWidgetItem(QString::fromStdString(source));
  entry.source_item->setFlags(entry.source_item->flags() & ~Qt::ItemIsEditable);
  entry.source_item->setTextAlignment(Qt::AlignCenter);
  const std::string vendor_key = engine::vendor_icon_key(source);
  if (!vendor_key.empty())
    entry.source_item->setIcon(engine::IconManager::instance().resolve_icon(
        QString::fromStdString(vendor_key)));
  table_->setItem(entry.row, Source, entry.source_item);

  entry.size_item = new QTableWidgetItem(QString());
  entry.size_item->setFlags(entry.size_item->flags() & ~Qt::ItemIsEditable);
  table_->setItem(entry.row, Size, entry.size_item);

  entry.filetime_item = new QTableWidgetItem(QString());
  entry.filetime_item->setFlags(entry.filetime_item->flags() & ~Qt::ItemIsEditable);
  entry.filetime_item->setTextAlignment(Qt::AlignCenter);
  table_->setItem(entry.row, Filetime, entry.filetime_item);
  update_filetime(entry);
  update_tooltip(entry);

  // Nexus ID: the parent mod page id. Only Nexus downloads carry one, so
  // every other source leaves this cell blank.
  auto *nexus_id_item = new QTableWidgetItem(QString::fromStdString(parent_mod_id));
  nexus_id_item->setFlags(nexus_id_item->flags() & ~Qt::ItemIsEditable);
  nexus_id_item->setTextAlignment(Qt::AlignCenter);
  table_->setItem(entry.row, NexusId, nexus_id_item);

  auto *bar = new QProgressBar(table_);
  bar->setRange(0, 100);
  bar->setValue(0);
  bar->setTextVisible(true);
  bar->setFormat("Starting...");
  table_->setCellWidget(entry.row, Status, bar);
  entry.progress_bar = bar;

  table_->setRowHeight(entry.row, row_height());
  return true;
}

void DownloadsTab::update_tooltip(DownloadEntry &entry) {
  if (!entry.name_item)
    return;
  QStringList lines;
  if (entry.state == DownloadState::Downloading && entry.file_path.empty()) {
    // MO2's pendingDownload branch (downloadlist.cpp:214-216).
    lines << tr("Pending download");
  } else {
    lines << (entry.name_item->text());
  }
  if (!entry.page_url.empty())
    lines << QString::fromStdString(entry.page_url);
  else if (!entry.nexus_domain.empty() && !entry.parent_mod_id.empty()) {
    lines << QString::fromStdString("https://www.nexusmods.com/" + entry.nexus_domain +
                                    "/mods/" + entry.parent_mod_id);
  }
  // The on-disk location is the part the columns never show, and it is what
  // "Show in Folder" opens.
  if (!entry.file_path.empty())
    lines << QString::fromStdString(entry.file_path.string());
  entry.name_item->setToolTip(lines.join('\n'));
}

void DownloadsTab::update_filetime(DownloadEntry &entry) {
  if (!entry.filetime_item)
    return;
  if (entry.file_path.empty()) {
    entry.filetime_item->setText(QString());
    return;
  }
  const QFileInfo info(QString::fromStdString(entry.file_path.string()));
  if (!info.exists()) {
    entry.filetime_item->setText(QString());
    return;
  }
  // MO2's DownloadManager::getFileTime (downloadmanager.cpp:1441-1447):
  // when the file was created, falling back through the times a filesystem
  // may not record. birthTime is the honest answer for a freshly landed
  // archive; lastModified is what a copy preserves.
  QDateTime stamp = info.birthTime();
  if (!stamp.isValid())
    stamp = info.metadataChangeTime();
  if (!stamp.isValid())
    stamp = info.lastModified();
  entry.filetime_item->setText(
      stamp.isValid() ? QLocale().toString(stamp, QLocale::ShortFormat) : QString());
}

DownloadsTab::DownloadEntry &DownloadsTab::entry_for(const std::string &id) {
  static DownloadEntry null_entry{-1};
  auto it = downloads_.find(id);
  if (it != downloads_.end())
    return it->second;
  return null_entry;
}

void DownloadsTab::rename_download(const std::string &id, const std::string &new_name) {
  auto it = downloads_.find(id);
  if (it == downloads_.end())
    return;
  auto &entry = it->second;
  if (entry.name_item) {
    entry.name_item->setText(QString::fromStdString(new_name));
    table_->setRowHeight(entry.row, row_height());
    update_tooltip(entry);
  }
}

void DownloadsTab::update_progress(const std::string &id, int64_t downloaded,
                                   int64_t total, double speed) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;
  if (entry.state != DownloadState::Downloading)
    return;

  entry.total_size = total;

  // Size column: show only download speed while downloading
  if (speed > 0.0) {
    entry.size_item->setText(format_size(static_cast<int64_t>(speed)) + "/s");
  } else {
    entry.size_item->setText(QString());
  }

  // Update progress bar
  int pct = 0;
  if (total > 0) {
    pct = static_cast<int>((downloaded * 100) / total);
    if (pct > 100)
      pct = 100;
  }
  entry.progress_bar->setValue(pct);

  if (speed > 0.0) {
    QString speed_str = format_size(static_cast<int64_t>(speed)) + "/s";
    if (total > 0) {
      // MO2 renders "%p% - speed - ~remaining"
      // (downloadmanager.cpp:1747-1750). The estimate is dropped when it
      // cannot be made, so a just-started row does not claim "~0s".
      QString format    = "%p% - " + speed_str;
      const QString eta = format_remaining_time(total - downloaded, speed);
      if (!eta.isEmpty())
        format += " - ~" + eta;
      entry.progress_bar->setFormat(format);
    } else {
      entry.progress_bar->setFormat(format_size(downloaded) + " - " + speed_str);
    }
  } else {
    if (total > 0) {
      entry.progress_bar->setFormat("%p%");
    } else {
      entry.progress_bar->setFormat(format_size(downloaded));
    }
  }
}

void DownloadsTab::replace_bar_with_label(const std::string &id, const QString &text,
                                          const QColor &bg, const QColor &fg) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  // Remove the progress bar widget
  table_->removeCellWidget(entry.row, Status);
  entry.progress_bar = nullptr;

  // Replace with a centered QTableWidgetItem
  auto *item = new QTableWidgetItem(text);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  item->setTextAlignment(Qt::AlignCenter);
  if (bg.isValid())
    item->setBackground(bg);
  if (fg.isValid())
    item->setForeground(fg);
  table_->setItem(entry.row, Status, item);
}

void DownloadsTab::mark_complete(const std::string &id, bool success) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  const auto prev_state = entry.state;
  entry.state           = success ? DownloadState::Complete : DownloadState::Failed;
  if (success) {
    // Done: normal background, green "Install" text.
    replace_bar_with_label(id, state_label(entry.state), QColor(), QColor("#4CAF50"));
  } else {
    replace_bar_with_label(id, state_label(entry.state), QColor("#f44336"),
                           QColor(Qt::white));
  }

  // Show final archive size
  if (entry.total_size > 0) {
    entry.size_item->setText(format_size(entry.total_size));
  }

  // A download finishing is the only way out of the active-download scan
  // guard; once none remain, re-scan so the completed archive (and anything
  // that landed in the dir meanwhile) surfaces.
  if ((prev_state == DownloadState::Downloading ||
       prev_state == DownloadState::Paused) &&
      !has_active_download())
    on_downloads_dir_changed();
}

void DownloadsTab::mark_installed(const std::string &id) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  entry.state = DownloadState::Installed;
  replace_bar_with_label(id, tr("Installed"), QColor(), QColor());

  // Show final archive size
  if (entry.total_size > 0) {
    entry.size_item->setText(format_size(entry.total_size));
  }

  // If "hide installed" is on, the row disappears immediately
  apply_installed_filter();
}

void DownloadsTab::mark_uninstalled(const std::string &archive_filename) {
  if (archive_filename.empty())
    return;

  const std::filesystem::path wanted(archive_filename);
  for (auto &[id, entry] : downloads_) {
    if (entry.row < 0 || entry.state != DownloadState::Installed)
      continue;
    // Match on the filename only: the old mod's meta.ini records the bare
    // archive name, while the row holds a full path that moves with the
    // downloads directory.
    if (entry.file_path.filename() != wanted)
      continue;

    // The archive is still on disk and still re-installable, so the honest
    // state is the one a finished download has - not a new state that would
    // need a label, a manifest value and a filter rule of its own. MO2 marks
    // the download "Uninstalled" here (downloadlist.cpp:198); saying "Complete"
    // tells the same story about the only thing the row is claiming.
    entry.state = DownloadState::Complete;
    replace_bar_with_label(id, state_label(entry.state), QColor(), QColor("#4CAF50"));
    if (entry.total_size > 0)
      entry.size_item->setText(format_size(entry.total_size));
    apply_installed_filter();
    return;
  }
}

void DownloadsTab::mark_paused(const std::string &id) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  entry.state = DownloadState::Paused;
  replace_bar_with_label(id, tr("Paused"), QColor("#FF9800"), QColor(Qt::white));
}

void DownloadsTab::mark_downloading(const std::string &id) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  entry.state = DownloadState::Downloading;

  // Drop any label/bar currently in the Status column and start fresh.
  if (entry.progress_bar) {
    entry.progress_bar->deleteLater();
    entry.progress_bar = nullptr;
  }
  table_->removeCellWidget(entry.row, Status);
  delete table_->takeItem(entry.row, Status);

  auto *bar = new QProgressBar(table_);
  bar->setRange(0, 100);
  bar->setValue(0);
  bar->setTextVisible(true);
  bar->setFormat("Starting...");
  table_->setCellWidget(entry.row, Status, bar);
  entry.progress_bar = bar;

  table_->setRowHeight(entry.row, row_height());
}

bool DownloadsTab::blocks_refetch(const std::string &id) const {
  auto it = downloads_.find(id);
  if (it == downloads_.end())
    return false;
  switch (it->second.state) {
  case DownloadState::Downloading:
  case DownloadState::Complete:
  case DownloadState::Installed:
    return true;
  case DownloadState::Paused:
  case DownloadState::Failed:
  case DownloadState::Removed:
    break;
  }
  return false;
}

void DownloadsTab::set_file_path(const std::string &id,
                                 const std::filesystem::path &path) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;
  entry.file_path = path;
  // The archive only exists now, so this is the first moment Filetime has
  // something to show.
  update_filetime(entry);
  update_tooltip(entry);
}

void DownloadsTab::set_downloads_dir(const std::filesystem::path &dir) {
  if (downloads_dir_ != dir) {
    // Re-point the watcher (a switched instance gets a new downloads dir).
    const auto old_str = QString::fromStdString(downloads_dir_.string());
    if (!old_str.isEmpty() && dir_watcher_->directories().contains(old_str))
      dir_watcher_->removePath(old_str);
    downloads_dir_ = dir;
  }
  if (downloads_dir_.empty())
    return;

  std::error_code ec;
  std::filesystem::create_directories(downloads_dir_, ec);
  if (ec) {
    engine::Logger::instance().error("downloads: cannot create dir " +
                                     downloads_dir_.string() + ": " + ec.message());
  }
  const auto dir_str = QString::fromStdString(downloads_dir_.string());
  if (!dir_watcher_->directories().contains(dir_str))
    dir_watcher_->addPath(dir_str);
  engine::Logger::instance().debug(
      "downloads: watching dir " + downloads_dir_.string() + " (watched=" +
      std::to_string(dir_watcher_->directories().contains(dir_str)) + ")");

  // Re-surface folder state immediately (new dir, or re-pointed after an
  // instance switch); the watcher covers later external changes.
  scan_downloads_dir();
}

void DownloadsTab::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  // Directory watcher handles external changes; no rescan needed on tab switch.
}

bool DownloadsTab::has_active_download() const {
  for (const auto &[id, entry] : downloads_) {
    (void)id;
    if (entry.state == DownloadState::Downloading ||
        entry.state == DownloadState::Paused)
      return true;
  }
  return false;
}

void DownloadsTab::scan_downloads_dir() {
  if (downloads_dir_.empty()) {
    engine::Logger::instance().debug("downloads: scan skipped (dir empty)");
    return;
  }
  // While a download is downloading or paused its partial archive sits in
  // the dir under its final name; don't surface it as a "Complete" entry.
  if (has_active_download()) {
    engine::Logger::instance().debug("downloads: scan skipped (active download)");
    return;
  }

  std::error_code ec;
  std::filesystem::directory_iterator it(downloads_dir_, ec);
  if (ec) {
    engine::Logger::instance().error("downloads: scan failed for " +
                                     downloads_dir_.string() + ": " + ec.message());
    return;
  }
  int scanned = 0;
  for (const auto &entry : it) {
    if (!entry.is_regular_file(ec))
      continue;
    ++scanned;
    add_downloads_dir_file(entry.path());
  }
  engine::Logger::instance().debug(
      "downloads: scanned " + std::to_string(scanned) +
      " files, tracked=" + std::to_string(downloads_.size()));

  // Drop rows whose archive file is gone from the dir, so the tab mirrors
  // disk state (the watchdog can't see a file a file manager just deleted).
  // Collect ids first: remove_entry() mutates downloads_, and this loop is
  // iterating it. remove_entry() skips the trash step for the missing file
  // and emits entry_removed so the manifest persists the removal.
  std::vector<std::string> vanished;
  for (const auto &[id, entry] : downloads_) {
    if (entry.file_path.empty())
      continue;
    std::error_code fec;
    if (!std::filesystem::exists(entry.file_path, fec))
      vanished.push_back(id);
  }
  for (const auto &id : vanished)
    remove_entry(id);

  apply_installed_filter();
}

bool DownloadsTab::add_downloads_dir_file(const std::filesystem::path &path) {
  if (!is_archive_path(path)) {
    engine::Logger::instance().debug("downloads: skip non-archive " +
                                     path.filename().string());
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    engine::Logger::instance().debug("downloads: skip non-file " + path.string());
    return false;
  }

  const auto id = path.filename().string();
  if (downloads_.count(id)) {
    // Already tracked under this name: an overwrite drop replaced the
    // file on disk, so refresh the row's size instead of adding a
    // duplicate (the "new archive" must surface immediately).
    auto &existing = downloads_.at(id);
    auto fsize     = std::filesystem::file_size(path, ec);
    if (!ec) {
      existing.total_size = static_cast<int64_t>(fsize);
      if (existing.size_item)
        existing.size_item->setText(format_size(existing.total_size));
    }
    update_filetime(existing);
    return true;
  }

  // Already backs a tracked entry under a different key (Nexus downloads
  // use "<mod_id>-<file_id>", not the filename).
  for (const auto &[eid, e] : downloads_) {
    (void)eid;
    if (e.file_path == path)
      return false;
  }

  add_download(id, path.stem().string(), tr("Manual").toStdString(), path);
  auto &added      = downloads_.at(id);
  added.total_size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec)
    added.total_size = 0;
  mark_complete(id, true);
  engine::Logger::instance().debug("downloads: added Manual entry '" + id +
                                   "' (total=" + std::to_string(downloads_.size()) +
                                   ")");
  return true;
}

bool DownloadsTab::import_dropped_file(const std::filesystem::path &source, bool move) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(source, ec)) {
    engine::Logger::instance().warn("drop: invalid source file " + source.string());
    return false;
  }

  if (downloads_dir_.empty())
    return false;
  std::filesystem::create_directories(downloads_dir_, ec);
  if (ec) {
    engine::Logger::instance().error("drop: cannot create downloads dir " +
                                     downloads_dir_.string() + ": " + ec.message());
    return false;
  }

  auto dest = downloads_dir_ / source.filename();

  // Dropping a file that already sits in the downloads dir: just surface it.
  std::error_code cmp_ec;
  const bool same_file =
      std::filesystem::equivalent(source, dest, cmp_ec) || (!cmp_ec && source == dest);
  if (same_file)
    return add_downloads_dir_file(dest);

  const bool exists = std::filesystem::exists(dest, ec);
  if (exists) {
    if (!conflict_resolver_)
      return false;
    switch (conflict_resolver_(dest, source)) {
    case DropConflictAction::Ignore:
      return false;
    case DropConflictAction::Rename: {
      // MO2's getDownloadFileName numbering: N_<name>, 1-based.
      int i = 1;
      while (std::filesystem::exists(
          downloads_dir_ / (std::to_string(i) + "_" + source.filename().string()),
          ec)) {
        ++i;
        ec.clear();
      }
      dest = downloads_dir_ / (std::to_string(i) + "_" + source.filename().string());
      break;
    }
    case DropConflictAction::Overwrite:
      break;
    }
  }

  bool ok = false;
  if (move) {
    ok = engine::move_path(source, dest);
  } else {
    std::error_code copy_ec;
    std::filesystem::copy_file(
        source, dest, std::filesystem::copy_options::overwrite_existing, copy_ec);
    if (!copy_ec)
      ok = true;
    else
      engine::Logger::instance().error("drop: failed to copy " + source.string() +
                                       " -> " + dest.string() + ": " +
                                       copy_ec.message());
  }
  if (!ok)
    return false;

  return add_downloads_dir_file(dest);
}

void DownloadsTab::dragEnterEvent(QDragEnterEvent *event) {
  if (accepts_url_drop(event->mimeData()) ||
      !loverslab_drop_url(event->mimeData()).empty())
    event->acceptProposedAction();
}

void DownloadsTab::dragMoveEvent(QDragMoveEvent *event) {
  if (accepts_url_drop(event->mimeData()) ||
      !loverslab_drop_url(event->mimeData()).empty())
    event->acceptProposedAction();
}

void DownloadsTab::dropEvent(QDropEvent *event) {
  // A single LoversLab download link dropped from a browser starts the same
  // download flow as the "Add from URL…" button.
  const std::string ll_url = loverslab_drop_url(event->mimeData());
  if (!ll_url.empty()) {
    emit loverslab_url_entered(ll_url);
    event->accept();
    return;
  }

  if (!accepts_url_drop(event->mimeData())) {
    event->ignore();
    return;
  }
  const bool move = event->proposedAction() == Qt::MoveAction;
  if (move) {
    // Tell the source we take ownership of the file (MO2 does the same
    // so the file manager does not also move it).
    event->setDropAction(Qt::TargetMoveAction);
  }

  // accepts_url_drop already guarantees every URL is a local archive file.
  for (const auto &url : event->mimeData()->urls())
    import_dropped_file(url.toLocalFile().toStdString(), move);
  apply_installed_filter();
  event->accept();
}

void DownloadsTab::apply_installed_filter() {
  if (!hide_installed_ || !table_)
    return;
  const bool hide    = hide_installed_->isChecked();
  const QString text = current_filter_text_;
  for (const auto &[id, entry] : downloads_) {
    // A row the user hid through the batch actions stays hidden until
    // "Un-Hide All..." clears the flag. This runs first and `continue`s,
    // because every branch below ends in setRowHidden(.., false) and would
    // otherwise resurrect the row on the next filter pass.
    if (entry.hidden) {
      table_->setRowHidden(entry.row, true);
      continue;
    }
    // "Hide installed" always wins over the text filter.
    if (hide && entry.state == DownloadState::Installed) {
      table_->setRowHidden(entry.row, true);
      continue;
    }
    // A row hidden by the shared text filter (RightFilterBar::apply_to) is
    // never unhidden here - without this the re-apply clobbered the text
    // filter and "Filter..." did nothing on the Downloads tab.
    if (!text.isEmpty()) {
      bool match = false;
      for (int col = 0; col < table_->columnCount(); ++col) {
        auto *item = table_->item(entry.row, col);
        if (item && item->text().toLower().contains(text)) {
          match = true;
          break;
        }
      }
      table_->setRowHidden(entry.row, !match);
    } else {
      table_->setRowHidden(entry.row, false);
    }
  }
}

void DownloadsTab::set_filter_text(const QString &text) {
  current_filter_text_ = text.trimmed().toLower();
}

void DownloadsTab::reapply_installed_filter() {
  apply_installed_filter();
}

void DownloadsTab::hide_batch(DownloadBatch batch, bool unhide) {
  // Copy the ids out first: the loop below only writes a flag, but the
  // manifest round-trip that follows reads the whole map, and keeping the
  // loop free of anything that can insert or erase keeps that trivially safe.
  std::vector<std::string> targets;
  for (const auto &[id, entry] : downloads_) {
    // Un-Hide All covers every row regardless of state; the Hide actions take
    // the batch's own scope.
    if (unhide || download_in_batch(entry.state, batch))
      targets.push_back(id);
  }

  int changed = 0;
  for (const auto &id : targets) {
    auto it = downloads_.find(id);
    if (it == downloads_.end())
      continue;
    const bool want = !unhide;
    if (it->second.hidden == want)
      continue;
    it->second.hidden = want;
    ++changed;
  }
  if (changed == 0)
    return;

  // Nothing was deleted, so the manifest must be rewritten through the normal
  // row-changed path rather than entry_removed.
  apply_installed_filter();
  emit entries_changed();
}

void DownloadsTab::remove_batch(DownloadBatch batch) {
  // Collect first: remove_entry() mutates downloads_ and reindexes rows, so
  // iterating the map while removing would invalidate the iterator.
  std::vector<std::string> targets;
  for (const auto &[id, entry] : downloads_) {
    if (download_in_batch(entry.state, batch))
      targets.push_back(id);
  }
  if (targets.empty())
    return;

  const QString what =
      batch == DownloadBatch::Installed
          ? tr("%n installed download(s)", "", static_cast<int>(targets.size()))
          : (batch == DownloadBatch::Uninstalled
                 ? tr("%n uninstalled download(s)", "",
                      static_cast<int>(targets.size()))
                 : tr("%n download(s)", "", static_cast<int>(targets.size())));
  TaskDialog dlg(this, {});
  dlg.title(tr("Remove Downloads"))
      .main(what)
      .content(tr("The archives stay in the system trash and can be restored."))
      .icon(QMessageBox::Question)
      .add_button({tr("Yes"), "", QMessageBox::Yes})
      .add_button({tr("No"), "", QMessageBox::No});
  if (dlg.exec() != QMessageBox::Yes)
    return;

  for (const auto &id : targets)
    remove_entry(id);
}

void DownloadsTab::on_cell_double_clicked(int row, int column) {
  (void)column;
  // Find the download id for this row
  for (const auto &[id, entry] : downloads_) {
    if (entry.row == row) {
      // MO2 parity: a double-click drives the row by its state. A paused
      // download resumes (the context menu's Resume action is the same
      // primitive), a finished one installs, an active one does nothing -
      // pausing an in-flight download stays a context-menu-only action.
      if (entry.state == DownloadState::Paused) {
        emit resume_requested(id);
        return;
      }
      if (entry.state == DownloadState::Complete ||
          entry.state == DownloadState::Failed ||
          entry.state == DownloadState::Installed) {
        if (!entry.file_path.empty() && std::filesystem::exists(entry.file_path)) {
          auto src = source_info_for(id, entry);
          emit install_requested(
              id, entry.file_path, src.source_type, src.source_id, src.file_id,
              entry.name_item ? entry.name_item->text().toStdString() : std::string(),
              src.page_url);
        }
      }
      return;
    }
  }
}

DownloadsTab::SourceInfo
DownloadsTab::source_info_for(const std::string &id, const DownloadEntry &entry) const {
  SourceInfo info;
  const QString source_text = entry.source_item ? entry.source_item->text() : QString();

  // Source attribution must match the entry's actual origin. The Source
  // column is filled with literal strings ("Nexus Mods", "LoversLab",
  // "Manual"), but a bare label is NOT enough - older manifests (or
  // entries added with a half-populated row) can carry "Nexus Mods" with
  // no nexus_domain / parent_mod_id / file_id, which is exactly what the
  // bug reported in Workspace-rvld was: a LoversLab / Steam / Isaac mod
  // whose Source cell said "Nexus Mods" because a stale row was reused
  // and downstream code treated it as Nexus-valid. Treat such rows as
  // local-only installs (no source) and let the install path write a
  // "manual" sidecar instead. Same defensive check for LoversLab: a row
  // labelled LoversLab without a file id / page URL is meaningless.
  if (source_text == QStringLiteral("Nexus Mods")) {
    const bool nexus_complete = !entry.parent_mod_id.empty() &&
                                !entry.nexus_domain.empty() && entry.file_id > 0;
    if (nexus_complete) {
      info.source_type = "nexus";
      info.source_id   = entry.parent_mod_id;
      info.file_id     = entry.file_id;
    }
    // else: leave info empty -> caller treats as manual local archive.
  } else if (source_text == QStringLiteral("LoversLab")) {
    const bool loverslab_complete = !id.empty() && !entry.page_url.empty();
    if (loverslab_complete) {
      // The entry id is the file id (or an ll-<hash> fallback when the
      // URL carried none); page_url is the mod page the download
      // came from.
      info.source_type = "loverslab";
      info.source_id   = id;
      info.page_url    = entry.page_url;
    }
    // else: leave info empty.
  } else if (source_text == QStringLiteral("ModPub")) {
    // ModPub is metadata-only; the actual download row arrives via
    // the modl:// flow. A row labeled "ModPub" without a page_url
    // is meaningless (we have no way to point at the mod) - treat
    // it as local-only install like the LoversLab/Steam gate above.
    const bool modpub_complete = !id.empty() && !entry.page_url.empty();
    if (modpub_complete) {
      info.source_type = "modpub";
      info.source_id   = id;
      info.page_url    = entry.page_url;
    }
    // else: leave info empty.
  }
  return info;
}

void DownloadsTab::on_custom_context_menu(const QPoint &pos) {
  auto *item = table_->itemAt(pos);
  if (!item)
    return;
  const int row = item->row();

  const std::string *found = nullptr;
  for (const auto &[id, entry] : downloads_) {
    if (entry.row == row) {
      found = &id;
      break;
    }
  }
  if (!found)
    return;

  QMenu menu(this);
  add_context_menu_actions(menu, *found);
  menu.exec(table_->viewport()->mapToGlobal(pos));
}

void DownloadsTab::add_context_menu_actions(QMenu &menu, const std::string &id) {
  const auto &entry = downloads_.at(id);

  // Resolved through the central IconManager (theme/pack -> system -> base pack),
  // with a standard-icon fallback for the download actions.
  auto icon_for = [](const QString &theme, QStyle::StandardPixmap fallback) -> QIcon {
    return engine::IconManager::instance().resolve_icon(theme, fallback);
  };

  // Install / Reinstall (enabled when the archive exists and no download
  // is running). Reuses the same pipeline as double-click.
  const bool can_install = entry.state != DownloadState::Downloading &&
                           entry.state != DownloadState::Paused &&
                           !entry.file_path.empty() &&
                           std::filesystem::exists(entry.file_path);
  auto *install_action   = menu.addAction(
      icon_for("download", QStyle::SP_ArrowDown),
      entry.state == DownloadState::Installed ? tr("Reinstall") : tr("Install"), this,
      [this, id, entry]() {
        // Origin metadata: derive the engine source_type from the Source
        // column (literal strings, serialized, so it survives restarts).
        // LoversLab rows get "loverslab" so a later reinstall keeps the
        // provenance. Not compared via tr(): the column is filled with the
        // literal text, not a translated one.
        auto src = source_info_for(id, entry);
        emit install_requested(
            id, entry.file_path, src.source_type, src.source_id, src.file_id,
            entry.name_item ? entry.name_item->text().toStdString() : std::string(),
            src.page_url);
      });
  install_action->setEnabled(can_install);

  if (entry.state == DownloadState::Downloading) {
    menu.addAction(icon_for("media-playback-pause", QStyle::SP_MediaPause), tr("Pause"),
                   this, [this, id]() {
                     emit pause_requested(id);
                   });
  } else if (entry.state == DownloadState::Paused) {
    menu.addAction(icon_for("media-playback-start", QStyle::SP_MediaPlay), tr("Resume"),
                   this, [this, id]() {
                     emit resume_requested(id);
                   });
  }

  menu.addAction(icon_for("folder", QStyle::SP_DirOpenIcon), tr("Show in Folder"), this,
                 [entry, this]() {
                   auto dir = downloads_dir_;
                   if (!entry.file_path.empty())
                     dir = entry.file_path.parent_path();
                   if (!dir.empty())
                     QDesktopServices::openUrl(
                         QUrl::fromLocalFile(QString::fromStdString(dir.string())));
                 });

  // Open on <Source>: LoversLab rows open the stored mod page URL (the
  // download link minus the ?do=download query, persisted in the manifest);
  // Nexus rows build the page from domain + mod id; ModPub rows open
  // the stored mod.pub page URL (carries the game-slug + slug-suffix).
  // Local ("Manual") rows have no page and get no action.
  const QString source_text = entry.source_item ? entry.source_item->text() : QString();
  QString page_url;
  QString page_label;
  if (!entry.page_url.empty()) {
    // Fall back to "LoversLab" only when the row has a page URL but
    // no source label at all (legacy data). All other rows use their
    // own source label (Nexus / ModPub / Steam / ...).
    QString fallback_source = source_text;
    if (fallback_source.isEmpty())
      fallback_source = tr("LoversLab");
    page_label = tr("Open on %1").arg(fallback_source);
    page_url   = QString::fromStdString(entry.page_url);
  } else if (!entry.nexus_domain.empty() && !entry.parent_mod_id.empty()) {
    page_label = tr("Open on Nexus");
    page_url =
        QString::fromStdString("https://www.nexusmods.com/" + entry.nexus_domain +
                               "/mods/" + entry.parent_mod_id);
  }
  if (!page_url.isEmpty()) {
    auto *page_action =
        menu.addAction(icon_for("text-html", QStyle::SP_FileDialogInfoView), page_label,
                       this, [page_url]() {
                         WebLink::open(page_url);
                       });
    page_action->setData(page_url);  // test handle for the target URL
  }

  menu.addSeparator();
  menu.addAction(icon_for("edit-delete", QStyle::SP_TrashIcon), tr("Remove"), this,
                 [this, id]() {
                   confirm_and_remove(id);
                 });

  // Batch actions (MO2 DownloadListView::downloadContextMenu,
  // downloadlistview.cpp:300-325). Every row in scope, not just the one under
  // the cursor, so these sit below the per-row Remove.
  const bool any_hidden =
      std::any_of(downloads_.begin(), downloads_.end(), [](const auto &kv) {
        return kv.second.hidden;
      });
  menu.addSeparator();
  for (auto batch :
       {DownloadBatch::Installed, DownloadBatch::Uninstalled, DownloadBatch::All}) {
    const auto label = download_batch_label(batch, /*remove=*/true);
    menu.addAction(icon_for("edit-delete", QStyle::SP_TrashIcon), label, this,
                   [this, batch]() {
                     remove_batch(batch);
                   });
  }
  if (any_hidden) {
    menu.addAction(icon_for("dialog-cancel", QStyle::SP_DialogResetButton),
                   tr("Un-Hide All..."), this, [this]() {
                     hide_batch(DownloadBatch::All, /*unhide=*/true);
                   });
  } else {
    for (auto batch :
         {DownloadBatch::Installed, DownloadBatch::Uninstalled, DownloadBatch::All}) {
      menu.addAction(icon_for("view-conceal", QStyle::SP_DialogResetButton),
                     download_batch_label(batch, /*remove=*/false), this,
                     [this, batch]() {
                       hide_batch(batch, /*unhide=*/false);
                     });
    }
  }
}

void DownloadsTab::confirm_and_remove(const std::string &id) {
  const auto &entry = downloads_.at(id);
  const QString file_name =
      entry.file_path.empty()
          ? QString::fromStdString(id)
          : QString::fromStdString(entry.file_path.filename().string());
  TaskDialog dlg(this, {});
  configure_remove_download_dialog(dlg, file_name);
  if (dlg.exec() != QMessageBox::Yes)
    return;
  remove_entry(id);
}

bool DownloadsTab::eventFilter(QObject *watched, QEvent *event) {
  if (watched == table_ && event->type() == QEvent::KeyPress) {
    auto *key     = static_cast<QKeyEvent *>(event);
    const int row = table_->currentRow();
    // A row the filters hid is not a row the user is standing on, so the key
    // must not reach it. Qt leaves currentRow() naming a row that
    // setRowHidden has just hidden, so without this guard a keypress would
    // install / pause / remove a download that is no longer on screen.
    // on_custom_context_menu never had this problem: it hit-tests with
    // itemAt(pos), and a hidden row has no visual extent, so itemAt returns
    // nullptr and the handler bails before it reads downloads_. Resolve the
    // target the same way here - a hidden row is simply not reachable.
    if (row < 0 || table_->isRowHidden(row))
      return QWidget::eventFilter(watched, event);
    // Copy the id out before acting: every action below can delete the entry
    // (and with it the map row), so holding a reference across it would
    // dangle.
    std::string id;
    DownloadState state = DownloadState::Removed;
    bool found          = false;
    for (const auto &[candidate, entry] : downloads_) {
      if (entry.row == row) {
        id    = candidate;
        state = entry.state;
        found = true;
        break;
      }
    }
    if (found) {
      switch (download_shortcut_for(state, key->key())) {
      case DownloadShortcut::Install:
        // Same primitive the double-click uses, so one code path installs.
        on_cell_double_clicked(row, 0);
        return true;
      case DownloadShortcut::Remove:
        confirm_and_remove(id);
        return true;
      case DownloadShortcut::Pause:
        emit pause_requested(id);
        return true;
      case DownloadShortcut::Resume:
        emit resume_requested(id);
        return true;
      case DownloadShortcut::None:
        break;
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void configure_remove_download_dialog(TaskDialog &dlg, const QString &file_name) {
  dlg.title(QObject::tr("Remove Download"))
      .main(QObject::tr("Move \"%1\" to the trash bin?").arg(file_name))
      .content(QObject::tr("The archive stays in the system trash and can be "
                           "restored."))
      .icon(QMessageBox::Question)
      .add_button({QObject::tr("Yes"), "", QMessageBox::Yes})
      .add_button({QObject::tr("No"), "", QMessageBox::No});
}

void DownloadsTab::remove_entry(const std::string &id) {
  auto it = downloads_.find(id);
  if (it == downloads_.end())
    return;
  auto &entry = it->second;

  // Trash the archive file (if any) before dropping the entry.
  if (!entry.file_path.empty() && std::filesystem::exists(entry.file_path)) {
    if (!engine::remove_path(entry.file_path)) {
      QMessageBox::warning(
          this, tr("Remove"),
          tr("Failed to move \"%1\" to the trash bin.")
              .arg(QString::fromStdString(entry.file_path.filename().string())));
      return;
    }
  }

  const int removed_row = entry.row;
  table_->removeRow(removed_row);
  downloads_.erase(it);

  // Rows below the removed one shift up - reindex the survivors.
  for (auto &[eid, e] : downloads_) {
    if (e.row > removed_row)
      --e.row;
  }

  emit entry_removed(id);
}

// --- Manifest persistence ---

std::string DownloadsTab::serialize() const {
  QJsonArray arr;
  for (const auto &[id, entry] : downloads_) {
    QJsonObject obj;
    obj["id"]         = QString::fromStdString(id);
    obj["name"]       = entry.name_item ? entry.name_item->text() : QString();
    obj["source"]     = entry.source_item ? entry.source_item->text() : QString();
    obj["size"]       = entry.size_item ? entry.size_item->text() : QString();
    obj["file_path"]  = QString::fromStdString(entry.file_path.string());
    obj["state"]      = static_cast<int>(entry.state);
    obj["total_size"] = static_cast<qint64>(entry.total_size);
    // Origin metadata - kept for update checks and the future
    // parent-mod + submods grouping (optional files).
    obj["parent_mod_id"] = QString::fromStdString(entry.parent_mod_id);
    obj["file_id"]       = entry.file_id;
    obj["domain"]        = QString::fromStdString(entry.nexus_domain);
    obj["category"]      = QString::fromStdString(entry.category);
    obj["page_url"]      = QString::fromStdString(entry.page_url);
    obj["hidden"]        = entry.hidden;
    arr.append(obj);
  }
  QJsonDocument doc(arr);
  return doc.toJson(QJsonDocument::Compact).toStdString();
}

void DownloadsTab::deserialize(const std::string &json,
                               const std::filesystem::path &downloads_dir) {
  auto doc = QJsonDocument::fromJson(QString::fromStdString(json).toUtf8());
  if (!doc.isArray())
    return;

  for (const auto &val : doc.array()) {
    auto obj        = val.toObject();
    auto id         = obj["id"].toString().toStdString();
    auto name       = obj["name"].toString().toStdString();
    auto source     = obj["source"].toString().toStdString();
    auto size_text  = obj["size"].toString();
    auto file_path  = std::filesystem::path(obj["file_path"].toString().toStdString());
    auto state      = static_cast<DownloadState>(obj["state"].toInt());
    auto total_size = static_cast<int64_t>(obj["total_size"].toDouble());
    const auto file_id       = obj["file_id"].toInt();
    const auto parent_mod_id = obj["parent_mod_id"].toString().toStdString();
    const auto nexus_domain  = obj["domain"].toString().toStdString();
    const auto page_url      = obj["page_url"].toString().toStdString();

    // Manifest repair for Workspace-rvld: older builds persisted
    // "Nexus Mods" as the source string even for LoversLab / Steam /
    // Isaac rows that had no real Nexus origin (no parent_mod_id,
    // no nexus_domain, no file_id). The Source label was the only
    // truth, so the deserialized row kept saying "Nexus Mods" and
    // source_info_for returned source_type="nexus" forever. Detect
    // that exact shape and coerce the label to "Manual" so the row
    // is treated as a local archive from now on. Same for LoversLab
    // rows that lost their page_url / file id.
    if (source == "Nexus Mods" && parent_mod_id.empty() && nexus_domain.empty() &&
        file_id == 0) {
      source = "Manual";
      engine::Logger::instance().debug("downloads: repaired legacy manifest entry '" +
                                       id +
                                       "' (Nexus Mods label without origin -> Manual)");
    } else if (source == "LoversLab" && (page_url.empty() || id.empty())) {
      source = "Manual";
      engine::Logger::instance().debug(
          "downloads: repaired legacy manifest entry '" + id +
          "' (LoversLab label without page_url/id -> Manual)");
    } else if (source == "ModPub" && (page_url.empty() || id.empty())) {
      // ModPub rows without a page URL or id carry no provenance -
      // same defensive treatment as the LoversLab/Steam gate.
      source = "Manual";
      engine::Logger::instance().debug(
          "downloads: repaired legacy manifest entry '" + id +
          "' (ModPub label without page_url/id -> Manual)");
    } else if (source == "Modl" || source == "Modl download" || source == "Direct" ||
               source == "Direct download") {
      // modl is a transport, not a source - it is no longer written
      // to the manifest. Any row carrying this label predates the
      // qvi6 fix; coerce to "Manual" so the source column reads
      // correctly and source_info_for does not try to dispatch it
      // through a (now-removed) "modl" provider lookup.
      source = "Manual";
      engine::Logger::instance().debug(
          "downloads: repaired legacy manifest entry '" + id +
          "' (modl/direct label without source attribution -> Manual)");
    }

    // Skip if already loaded
    if (downloads_.count(id))
      continue;

    // A persisted Downloading/Paused state describes a transfer that lived in
    // the previous process. Nothing is fetching it now, and the link maps that
    // resume needs are in-memory only, so the row cannot be continued - worse,
    // has_active_download() counts both states, so leaving one in place wedges
    // scan_downloads_dir() and the folder watcher stop auto-detecting archives
    // for the rest of the session. Failed is the one state that tells the truth
    // ("this did not finish") and that the row can still act on.
    if (state == DownloadState::Downloading || state == DownloadState::Paused) {
      engine::Logger::instance().warn(
          "downloads: '" + id + "' was " + (state == DownloadState::Downloading
                                                 ? "downloading"
                                                 : "paused") +
          " at shutdown and did not survive the restart; marked failed");
      state = DownloadState::Failed;
    }

    // Verify the archive file still exists
    if (!file_path.empty() && !std::filesystem::exists(file_path)) {
      // Try relative to downloads_dir
      auto abs_path = downloads_dir / file_path.filename();
      if (!std::filesystem::exists(abs_path))
        continue;
      file_path = abs_path;
    }

    auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
    if (!inserted)
      continue;

    auto &entry         = it->second;
    entry.row           = table_->rowCount();
    entry.file_path     = file_path;
    entry.state         = state;
    entry.total_size    = total_size;
    entry.parent_mod_id = parent_mod_id;
    entry.file_id       = file_id;
    entry.nexus_domain  = nexus_domain;
    entry.category      = obj["category"].toString().toStdString();
    entry.page_url      = page_url;
    entry.hidden        = obj["hidden"].toBool();

    table_->insertRow(entry.row);

    entry.name_item = new QTableWidgetItem(QString::fromStdString(name));
    entry.name_item->setFlags(entry.name_item->flags() & ~Qt::ItemIsEditable);
    table_->setItem(entry.row, Name, entry.name_item);

    entry.source_item = new QTableWidgetItem(QString::fromStdString(source));
    entry.source_item->setFlags(entry.source_item->flags() & ~Qt::ItemIsEditable);
    entry.source_item->setTextAlignment(Qt::AlignCenter);
    const std::string vendor_key = engine::vendor_icon_key(source);
    if (!vendor_key.empty())
      entry.source_item->setIcon(engine::IconManager::instance().resolve_icon(
          QString::fromStdString(vendor_key)));
    table_->setItem(entry.row, Source, entry.source_item);

    entry.filetime_item = new QTableWidgetItem(QString());
    entry.filetime_item->setFlags(entry.filetime_item->flags() & ~Qt::ItemIsEditable);
    entry.filetime_item->setTextAlignment(Qt::AlignCenter);
    table_->setItem(entry.row, Filetime, entry.filetime_item);
    update_filetime(entry);
    update_tooltip(entry);

    auto *nexus_id_item = new QTableWidgetItem(QString::fromStdString(parent_mod_id));
    nexus_id_item->setFlags(nexus_id_item->flags() & ~Qt::ItemIsEditable);
    nexus_id_item->setTextAlignment(Qt::AlignCenter);
    table_->setItem(entry.row, NexusId, nexus_id_item);

    // For non-downloading states, show the file size; during download the
    // size cell is empty until update_progress sets the speed text.
    if (state == DownloadState::Downloading) {
      entry.size_item = new QTableWidgetItem(QString());
      entry.size_item->setFlags(entry.size_item->flags() & ~Qt::ItemIsEditable);
      table_->setItem(entry.row, Size, entry.size_item);

      auto *bar = new QProgressBar(table_);
      bar->setRange(0, 100);
      bar->setValue(0);
      bar->setTextVisible(true);
      bar->setFormat("Starting...");
      table_->setCellWidget(entry.row, Status, bar);
      entry.progress_bar = bar;
    } else {
      // If total_size wasn't persisted (old manifest), stat the file
      int64_t resolved_size = total_size;
      if (resolved_size == 0 && !file_path.empty()) {
        std::error_code ec;
        auto fsize = std::filesystem::file_size(file_path, ec);
        if (!ec)
          resolved_size = static_cast<int64_t>(fsize);
      }
      entry.total_size = resolved_size;

      QString display_size =
          (resolved_size > 0) ? format_size(resolved_size) : QString();
      entry.size_item = new QTableWidgetItem(display_size);
      entry.size_item->setFlags(entry.size_item->flags() & ~Qt::ItemIsEditable);
      table_->setItem(entry.row, Size, entry.size_item);

      QColor bg, fg;
      if (state == DownloadState::Complete) {
        fg = QColor("#4CAF50");
      } else if (state == DownloadState::Failed) {
        bg = QColor("#f44336");
        fg = QColor(Qt::white);
      } else if (state == DownloadState::Removed) {
        fg = QColor("#B8860B");
      }
      auto *item = new QTableWidgetItem(state_label(state));
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      item->setTextAlignment(Qt::AlignCenter);
      if (bg.isValid())
        item->setBackground(bg);
      if (fg.isValid())
        item->setForeground(fg);
      table_->setItem(entry.row, Status, item);
    }

    table_->setRowHeight(entry.row, row_height());
  }

  apply_installed_filter();
}

}  // namespace ui
