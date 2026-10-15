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
#include <QDir>
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
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
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
  case DownloadState::Cancelled:
    return QCoreApplication::translate("DownloadsTab", "Cancelled");
  case DownloadState::Uninstalled:
    // MO2's own status string for STATE_UNINSTALLED (downloadlist.cpp:198).
    return QCoreApplication::translate("DownloadsTab", "Uninstalled");
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

// The suffix an in-progress download carries on disk until every byte has
// arrived (MO2 downloadmanager.cpp:57). Kept next to the scanner so the name
// the fetcher writes and the name the scanner skips cannot drift apart.
static constexpr const char *kUnfinishedSuffix = ".unfinished";

// Case-insensitive check: does `path` carry one of the supported archive
// extensions?
static bool is_archive_path(const std::filesystem::path &path) {
  // A partial download is never a finished archive, whatever it is named.
  // MO2 filters the ".unfinished" name explicitly (downloadmanager.cpp:405);
  // its own extension check alone would not, because the suffix replaces the
  // real extension.
  const std::string name = path.filename().string();
  std::string lower_name;
  for (char c : name)
    lower_name.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower_name.ends_with(kUnfinishedSuffix))
    return false;
  std::string lower;
  for (char c : path.extension().string())
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  const auto &exts = download_archive_exts();
  return std::find(exts.begin(), exts.end(), lower) != exts.end();
}

// Is this archive a finished download? Existence is not enough: a transfer that
// died at byte 0, or was killed half way, leaves a file that exists and is
// shorter than the transfer that produced it. Such a file must read as NOT
// downloaded, or a repeat link is refused against bytes that were never
// fetched - which is how a download that never started reports itself as
// already on disk.
//
// `expected_size` is the size the transfer reported for itself (the row's own
// total_size). 0 means "unknown" - an old manifest that predates the field, or
// a file that arrived outside a transfer. Unknown is not trusted in either
// direction: the bar is bytes on disk, and a zero-byte file fails it too.
static bool archive_is_complete(const std::filesystem::path &path,
                                int64_t expected_size) {
  if (path.empty())
    return false;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec)
    return false;
  const auto size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec || size <= 0)
    return false;
  // A known total_size is the only hard evidence of completeness available -
  // there is no hash and no completion marker. Short of it means truncated.
  return expected_size <= 0 || size >= expected_size;
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

// Monotonic milliseconds, so a clock change (NTP step, DST, the user moving
// the date) cannot make a running download's elapsed time jump backwards and
// report a negative rate. steady_clock is the one clock that does not.
static int64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

DownloadShortcut download_shortcut_for(DownloadState state, int qt_key) {
  switch (state) {
  case DownloadState::Complete:
  case DownloadState::Installed:
  // MO2's keyPressEvent keys on `state >= STATE_READY`
  // (downloadlistview.cpp:337) and STATE_UNINSTALLED sits above STATE_READY
  // (downloadmanager.h:151-153), so a superseded archive takes the same two
  // actions as any other finished download: its bytes are still on disk and
  // still re-installable.
  case DownloadState::Uninstalled:
    if (qt_key == Qt::Key_Enter || qt_key == Qt::Key_Return)
      return DownloadShortcut::Install;
    if (qt_key == Qt::Key_Delete)
      return DownloadShortcut::Remove;
    break;
  case DownloadState::Downloading:
    // MO2 DownloadListView::keyPressEvent (downloadlistview.cpp:345-349)
    // routes Delete on a STATE_DOWNLOADING row to issueCancel, not
    // issueDelete: there is no finished archive to trash and the download
    // manager owns the transfer, so the transfer is the thing to stop.
    if (qt_key == Qt::Key_Delete)
      return DownloadShortcut::Cancel;
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
  case DownloadState::Cancelled:
    // A cancelled row has no archive and no resumable partial, so Delete is
    // the only thing a key can do to it - Space must not offer a Resume that
    // has nothing to resume.
    if (qt_key == Qt::Key_Delete)
      return DownloadShortcut::Remove;
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

double average_download_speed(int64_t downloaded, int64_t base,
                              double elapsed_seconds) {
  // MO2 suppresses downloadSpeed for the first five seconds
  // (downloadmanager.cpp:2449): a transfer shorter than that averages over
  // too little time to mean anything, and the number flaps as each chunk
  // lands. The same floor keeps the ETA next to it honest.
  constexpr double kMinimumSeconds = 5.0;
  if (elapsed_seconds < kMinimumSeconds || elapsed_seconds <= 0.0)
    return 0.0;
  const int64_t transferred = downloaded - base;
  if (transferred <= 0)
    return 0.0;
  return static_cast<double>(transferred) / elapsed_seconds;
}

bool download_in_batch(DownloadState state, DownloadBatch batch) {
  if (state == DownloadState::Removed)
    return false;
  if (batch == DownloadBatch::All)
    return true;
  // An in-flight row has not finished, so it is neither installed nor
  // uninstalled - MO2's issueDeleteCompleted / issueDeleteUninstalled skip
  // anything below STATE_READY. A cancelled row sits below STATE_READY in
  // MO2's enum too (downloadmanager.h:144 vs :150), so it is skipped the
  // same way rather than swept up by "Delete Uninstalled Downloads...".
  if (state == DownloadState::Downloading || state == DownloadState::Paused ||
      state == DownloadState::Cancelled)
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
  // MO2's showHiddenBox ("Hidden files", mainwindow.ui:1426): show the rows
  // the batch Hide actions flagged without clearing the flag.
  auto *show_hidden = new QCheckBox(tr("Hidden files"), this);
  show_hidden->setToolTip(tr("Show downloads marked as hidden."));
  show_hidden->setObjectName("showHiddenBox");
  top->addWidget(show_hidden);
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
    // MO2's "Hide downloads after installation" (settingsdialog.ui:157-168).
    // The box is in force the moment it is ticked, and unticking it brings
    // back exactly the rows it took: there is no notice to spend and no grace
    // period, because a ticked box that needed a second click to take effect
    // is not the setting it claims to be.
    apply_installed_filter();
  });
  connect(show_hidden, &QCheckBox::toggled, this, [this](bool checked) {
    show_hidden_ = checked;
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

// MO2 DownloadManager::isInfoIncomplete (downloadmanager.cpp:1480-1492): a
// Nexus download whose mod or file id never resolved is missing the identity
// every other row carries, and cannot be re-queried later.
bool DownloadsTab::info_is_incomplete(const DownloadEntry &entry) {
  if (entry.nexus_domain.empty() || entry.file_path.empty())
    return false;
  return entry.parent_mod_id.empty() || entry.file_id <= 0;
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
  // MO2 isInfoIncomplete (downloadmanager.cpp:1480-1492): a Nexus download
  // whose file or mod id never resolved cannot be re-queried later, and the
  // row is missing the identity every other row carries. Say so here rather
  // than leaving the user to wonder why "Open on <source>" is unavailable.
  if (info_is_incomplete(entry)) {
    lines << tr("Information missing, re-download this file from the mod page.");
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

  // MO2's DecorationRole branch on the Name cell (downloadlist.cpp:233-240)
  // marks an incomplete row with the warning icon, so the defect is visible
  // without hovering. Same icon key the mod list uses for a row that does not
  // describe what the game will see.
  const bool incomplete = info_is_incomplete(entry);
  entry.name_item->setIcon(incomplete
                               ? engine::IconManager::instance().resolve_icon(
                                     "plugin-warning", QStyle::SP_MessageBoxWarning)
                               : QIcon());
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

  // MO2 reports the AVERAGE speed since the transfer (re)started, not the
  // instantaneous rate libcurl hands us (downloadmanager.cpp:2448-2451).
  // The instantaneous figure is what makes a download row unreadable - it
  // swings to zero between chunks and spikes on every read - and the same
  // wobble lands in the ETA beside it. The first tick of a run stamps the
  // base byte count and the start time; every later tick measures against
  // them. `speed` stays the per-call fallback so a caller that cannot supply
  // a monotonic clock still moves the row.
  if (entry.speed_started_ms == 0) {
    entry.speed_started_ms = monotonic_ms();
    entry.speed_base       = downloaded;
  }
  const double elapsed =
      static_cast<double>(monotonic_ms() - entry.speed_started_ms) / 1000.0;
  const double avg_speed =
      average_download_speed(downloaded, entry.speed_base, elapsed);
  const double shown_speed = avg_speed > 0.0 ? avg_speed : speed;

  // Size column: show only download speed while downloading
  if (shown_speed > 0.0) {
    entry.size_item->setText(format_size(static_cast<int64_t>(shown_speed)) + "/s");
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

  if (shown_speed > 0.0) {
    QString speed_str = format_size(static_cast<int64_t>(shown_speed)) + "/s";
    if (total > 0) {
      // MO2 renders "%p% - speed - ~remaining"
      // (downloadmanager.cpp:1747-1750). The estimate is dropped when it
      // cannot be made, so a just-started row does not claim "~0s".
      QString format    = "%p% - " + speed_str;
      const QString eta = format_remaining_time(total - downloaded, shown_speed);
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

void DownloadsTab::mark_installed(const std::string &id,
                                  const std::string &archive_filename) {
  // MO2's markInstalled(QString) looks the row up by the archive's file name
  // (downloadmanager.cpp:1556-1560), because the caller only ever holds a
  // path. Same fallback, same reason: an install started from the mod list
  // arrives with the mod FOLDER as its id, which is not any row's key, and
  // without the fallback the row the install just came from keeps reading
  // "Install" for the rest of the session.
  std::string target = id;
  if (!downloads_.count(target) && !archive_filename.empty()) {
    for (const auto &[eid, entry] : downloads_) {
      (void)eid;
      if (!entry.file_path.empty() &&
          entry.file_path.filename() == std::filesystem::path(archive_filename)) {
        target = eid;
        break;
      }
    }
  }

  auto &entry = entry_for(target);
  if (entry.row < 0)
    return;

  entry.state = DownloadState::Installed;
  replace_bar_with_label(target, tr("Installed"), QColor(), QColor());

  // Which row absorbed the mark, and how it was found. "The archive" is the
  // fallback MO2 also uses (downloadmanager.cpp:1556-1560) and it is the one
  // that can silently land on a row the caller never named, so which of the two
  // lookups answered is part of the answer.
  engine::Logger::instance().debug(
      "[Flow] downloads tab: row marked installed id='" + target + "' archive='" +
      archive_filename + "' matched_by=" +
      (target == id ? std::string("id")
                    : (archive_filename == id
                           ? std::string("archive")
                           : std::string("archive (id '" + id + "' not a row)"))));

  // Show final archive size
  if (entry.total_size > 0) {
    entry.size_item->setText(format_size(entry.total_size));
  }

  // MO2 removes the row outright when "Hide downloads after installation" is
  // on (organizercore.cpp:910-913 -> removeDownload(index, false), which
  // writes removed=true into the meta, downloadmanager.cpp:907-909, and
  // createFromMeta then skips the row on the next build, :101-104). Ours is a
  // view filter over a state, not a destructive write, so the same call hides
  // the row here and every pass after - no notice, no grace period.
  apply_installed_filter();
}

void DownloadsTab::mark_uninstalled(const std::string &archive_filename) {
  if (archive_filename.empty())
    return;

  const std::filesystem::path wanted(archive_filename);
  for (auto &[id, entry] : downloads_) {
    // MO2's markUninstalled(QString) writes the flag without consulting the
    // row's current state (downloadmanager.cpp:1594-1611): the name it is
    // handed IS the evidence that the archive was installed once, because it
    // came out of that mod's meta.ini installationfile. So the guard here is
    // only "is this row a finished download at all" - a transfer in flight or
    // a partial was never installed by anything.
    if (entry.row < 0 || entry.state == DownloadState::Downloading ||
        entry.state == DownloadState::Paused || entry.state == DownloadState::Failed ||
        entry.state == DownloadState::Cancelled)
      continue;
    // Match on the filename only: the old mod's meta.ini records the bare
    // archive name, while the row holds a full path that moves with the
    // downloads directory.
    if (entry.file_path.filename() != wanted)
      continue;

    entry.state = DownloadState::Uninstalled;
    // MO2's status colour for STATE_UNINSTALLED is dark yellow
    // (downloadlist.cpp:209-210), against READY's dark green. The archive is
    // still on disk and still re-installable, so the colour has to say "there
    // is something here" and not "there is an update".
    replace_bar_with_label(id, state_label(entry.state), QColor(), QColor("#FFC107"));
    if (entry.total_size > 0)
      entry.size_item->setText(format_size(entry.total_size));
    engine::Logger::instance().debug(
        "[Flow] downloads tab: superseded download marked uninstalled id='" + id +
        "' archive='" + archive_filename + "'");
    // Re-apply: an Uninstalled row is no longer pending for this mod, so a
    // ticked "Hide installed" takes it off the screen with the row it replaced.
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

void DownloadsTab::mark_cancelled(const std::string &id) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  const auto prev_state = entry.state;
  entry.state           = DownloadState::Cancelled;

  // Cancel means "give up on this", so the partial goes with it: the row has
  // no Resume action (a resumed row would resume from a file we no longer
  // hold) and a ".unfinished" file the scanner deliberately skips would
  // otherwise sit in the downloads dir forever, owned by nobody. MO2 leaves
  // its .unfinished file in place (downloadmanager.cpp:1665-1668 only aborts
  // the reply); this build drops it, which is why the difference is called
  // out rather than discovered.
  //
  // A failure to delete is reported, never swallowed: leaving bytes on disk
  // the user asked to have removed is exactly the silent partial failure
  // this row's own history is about.
  if (!entry.partial_path.empty() && std::filesystem::exists(entry.partial_path)) {
    if (!engine::remove_path(entry.partial_path)) {
      QMessageBox::warning(
          this, tr("Cancel"),
          tr("The transfer was cancelled, but \"%1\" could not be deleted.")
              .arg(QString::fromStdString(entry.partial_path.filename().string())));
    }
  }
  entry.partial_path.clear();

  replace_bar_with_label(id, state_label(entry.state), QColor("#f44336"),
                         QColor(Qt::white));
  entry.size_item->setText(QString());

  // Cancelling the last in-flight row releases the scan guard, so the
  // downloads dir is re-read - exactly what finishing a download does.
  if (prev_state == DownloadState::Downloading && !has_active_download())
    on_downloads_dir_changed();
}

void DownloadsTab::mark_downloading(const std::string &id) {
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;

  entry.state = DownloadState::Downloading;

  // A (re)started transfer re-times the running average from here, so a resume
  // never reports the whole archive's average as if this run had just fetched
  // it.
  entry.speed_started_ms = 0;
  entry.speed_base       = 0;

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
  // A superseded archive is finished and on disk exactly like any other, so a
  // repeat link for it is still a duplicate rather than a fresh fetch. What
  // changed is only which mod folder the bytes belong to now.
  case DownloadState::Uninstalled:
    return true;
  case DownloadState::Paused:
  case DownloadState::Failed:
  case DownloadState::Cancelled:
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
  // FetchStage renamed the partial onto this path, so the row owns no orphan
  // any more and must not trash a same-named partial a later download creates.
  entry.partial_path.clear();
  // The archive only exists now, so this is the first moment Filetime has
  // something to show.
  update_filetime(entry);
  update_tooltip(entry);
}

void DownloadsTab::note_partial_archive(const std::string &id,
                                        const std::string &archive_name) {
  if (archive_name.empty() || downloads_dir_.empty())
    return;
  auto &entry = entry_for(id);
  if (entry.row < 0)
    return;
  entry.partial_path = downloads_dir_ / (archive_name + kUnfinishedSuffix);
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
  int scanned = 0, non_archives = 0;
  for (const auto &entry : it) {
    if (!entry.is_regular_file(ec))
      continue;
    ++scanned;
    // Counted here rather than inside add_downloads_dir_file so the tally is
    // the scan's own, and reported as the ONE line below. This is what
    // replaced 79 per-file "skip non-archive <name>" lines per scan: the
    // number is what the user asked about, the names never were.
    if (!is_archive_path(entry.path())) {
      ++non_archives;
      continue;
    }
    add_downloads_dir_file(entry.path());
  }
  engine::Logger::instance().debug(
      "downloads: scanned " + std::to_string(scanned) + " files, " +
      std::to_string(non_archives) + " not archives, tracked=" +
      std::to_string(downloads_.size()) + " dir=" + downloads_dir_.string());
  engine::Logger::instance().debug(
      "[Flow] downloads tab scan: dir=" + downloads_dir_.string() + " files_on_disk=" +
      std::to_string(scanned) + " non_archives=" + std::to_string(non_archives) +
      " rows=" + std::to_string(downloads_.size()));

  // Drop rows whose archive file is gone from the dir, so the tab mirrors
  // disk state (the watchdog can't see a file a file manager just deleted).
  // Collect ids first: remove_entry() mutates downloads_, and this loop is
  // iterating it. remove_entry() skips the trash step for the missing file
  // and emits entry_removed so the manifest persists the removal.
  //
  // "Gone" means a FINISHED download's bytes were deleted from outside the
  // app. A row that never finished has a file_path (deserialize() fills it in
  // from the meta's name) but no bytes behind it, so existence alone is not
  // evidence of a deletion - it is the normal state of a paused or failed
  // transfer, and remove_entry() takes its meta with it. Dropping those here
  // is how a paused download got silently forgotten on the next launch, and
  // it is the same thing meta_is_orphan() refuses to do on the read path.
  // A vanished finished download is still deleted - just by the orphan sweep,
  // which asks those two questions before it removes anything.
  std::vector<std::string> vanished;
  for (const auto &[id, entry] : downloads_) {
    if (entry.file_path.empty())
      continue;
    if (entry.state == DownloadState::Downloading ||
        entry.state == DownloadState::Paused || entry.state == DownloadState::Failed ||
        entry.state == DownloadState::Cancelled)
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
  // Deliberately silent. Every rejection below is a PER-FILE decision and the
  // scan asks it of every file in the downloads dir: 77 archives and their 77
  // ".meta" sidecars meant 79 identical "skip non-archive <name>" lines on
  // EVERY scan, every panel show, every watcher tick, drowning the handful of
  // lines that carry news. The count is what the scan reports instead (see
  // scan_downloads_dir's summary line); the name of a file we deliberately do
  // not track is not information the user acts on.
  if (!is_archive_path(path))
    return false;

  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return false;

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

  // A file that arrived in the dir is a finished archive only if it has bytes.
  // A 0-byte file is what a transfer that died before its first write leaves
  // behind; surfacing it as a Complete row would both show the user a
  // downloadable mod and make a repeat link refuse against nothing.
  if (!archive_is_complete(path, 0))
    return false;

  add_download(id, path.stem().string(), tr("Manual").toStdString(), path);
  auto &added      = downloads_.at(id);
  added.total_size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec)
    added.total_size = 0;
  mark_complete(id, true);
  engine::Logger::instance().debug(
      "[Flow] downloads tab: adopted an untracked archive from disk as row '" + id +
      "' archive='" + path.filename().string() + "' name='" + path.stem().string() +
      "' source=Manual bytes=" +
      std::to_string(static_cast<long long>(std::filesystem::file_size(path, ec))));
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
  // One line per pass, not per row: the pass is what has to be legible when a
  // row is on screen that should not be. A ticked "Hide installed" that leaves
  // an Installed row visible looks exactly like a filter that is not wired, so
  // the counts - hidden, hidden by the text filter, shown - are the answer, and
  // they are all a single line.
  int hidden = 0, text_hidden = 0;
  for (const auto &[id, entry] : downloads_) {
    (void)id;
    // A row the user hid through the batch actions stays hidden until
    // "Un-Hide All..." clears the flag OR the "Hidden files" box shows them
    // again (MO2's showHiddenBox, mainwindow.cpp:3818-3821 -> setShowHidden ->
    // refreshList). The box is a VIEW filter: MO2 tests it when the list is
    // built (downloadmanager.cpp:102) and never clears the flag, so unticking
    // it restores the same hidden set rather than a default one. This runs
    // first and `continue`s, because every branch below ends in
    // setRowHidden(.., false) and would otherwise resurrect the row on the
    // next filter pass.
    if (entry.hidden && !show_hidden_) {
      table_->setRowHidden(entry.row, true);
      ++hidden;
      continue;
    }
    // MO2's "Hide downloads after installation" (settingsdialog.ui:157-168,
    // "Automatically hide downloads after successful installation"). On the
    // install path it removes the row that was just installed
    // (organizercore.cpp:910-913); the row of the archive a Replace
    // superseded is separately marked Uninstalled and is, by the same logic,
    // no longer pending for that mod. Both are states in which the download
    // will not be installed again by pressing Install, so a ticked box takes
    // both off the screen, immediately - no grace period, no notice.
    if (hide && (entry.state == DownloadState::Installed ||
                 entry.state == DownloadState::Uninstalled)) {
      table_->setRowHidden(entry.row, true);
      ++hidden;
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
      if (!match)
        ++text_hidden;
    } else {
      table_->setRowHidden(entry.row, false);
    }
  }
  engine::Logger::instance().debug("[Flow] downloads tab filter: hide_installed=" +
                                   std::string(hide ? "on" : "off") +
                                   " rows=" + std::to_string(downloads_.size()) +
                                   " hidden=" + std::to_string(hidden) +
                                   " hidden_by_text=" + std::to_string(text_hidden));
}

void DownloadsTab::set_filter_text(const QString &text) {
  current_filter_text_ = text.trimmed().toLower();
}

void DownloadsTab::reapply_installed_filter() {
  // Nothing is spent here any more. This used to clear the install notice that
  // held a just-installed row on screen, which existed because the row used to
  // be hidden by the filter in the same call that recorded the install. The
  // filter no longer needs an escape hatch - "Hide installed" is MO2's "Hide
  // downloads after installation" and it takes effect immediately - so a
  // deliberate re-application is just the filter.
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
    // MO2's order for a running row (downloadlistview.cpp:262-272) is Cancel,
    // Pause, Reveal in Explorer. Cancel and Pause stop the same transfer; what
    // differs is what the row is afterwards - Cancel abandons the partial,
    // Pause keeps it so Resume can carry on from there.
    menu.addAction(icon_for("dialog-cancel", QStyle::SP_DialogCancelButton),
                   tr("Cancel"), this, [this, id]() {
                     emit cancel_requested(id);
                   });
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
      case DownloadShortcut::Cancel:
        // Same abort the menu's Cancel action performs, through the same
        // signal, so the key and the menu cannot drift apart.
        emit cancel_requested(id);
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

  // Trash whatever bytes the row owns: the archive when it completed, and the
  // "<archive>.unfinished" partial when it did not. A paused Nexus/modl/URL row
  // has an EMPTY file_path, so before partial_path existed this trashed
  // nothing, dropped the row, and stranded the partial on disk forever - and
  // the scanner hides ".unfinished", so nothing surfaced it either.
  for (const auto *path : {&entry.file_path, &entry.partial_path}) {
    if (path->empty() || !std::filesystem::exists(*path))
      continue;
    if (!engine::remove_path(*path)) {
      QMessageBox::warning(this, tr("Remove"),
                           tr("Failed to move \"%1\" to the trash bin.")
                               .arg(QString::fromStdString(path->filename().string())));
      return;
    }
  }

  // The row's meta goes with it. The meta outlives the archive by design (an
  // archive deleted from outside the app still restores as a Failed row), so
  // an explicit Remove has to take the meta too or the row reappears on the
  // next launch. MO2 removes the meta in removeFile alongside the download.
  DownloadMeta removed_meta;
  removed_meta.archive      = entry.file_path;
  removed_meta.id           = id;
  removed_meta.partial_file = entry.partial_path.empty()
                                  ? std::string{}
                                  : entry.partial_path.filename().string();
  std::error_code meta_ec;
  std::filesystem::remove(download_meta_path(removed_meta, downloads_dir_), meta_ec);

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

// --- Per-file download meta (MO2 downloadmanager.cpp:88-164, 1771-1799) ---
//
// MO2 writes one "<archive>.meta" beside each archive and rebuilds the list
// by scanning for "*.meta". State is not a stored enum but a set of flags
// derived in a fixed priority (downloadmanager.cpp:118-126):
//   paused -> PAUSED, else uninstalled -> UNINSTALLED, else installed ->
//   INSTALLED, else READY. We write the flags it has a slot for and derive
//   back the same way.

std::filesystem::path download_meta_path(const DownloadMeta &meta,
                                         const std::filesystem::path &downloads_dir) {
  // Prefer the archive, then the partial (a queued row has no archive yet),
  // then the row id (a queued row has neither on disk). Every branch yields a
  // name inside the downloads dir, so the "*.meta" scan always finds it.
  std::filesystem::path base = !meta.archive.empty() ? meta.archive
                               : !meta.partial_file.empty()
                                   ? downloads_dir / meta.partial_file
                                   : downloads_dir / meta.id;
  return base.string() + ".meta";
}

bool write_download_meta(const DownloadMeta &meta,
                         const std::filesystem::path &meta_path) {
  std::error_code ec;
  std::filesystem::create_directories(meta_path.parent_path(), ec);

  // MO2's own read path is `QSettings metaFile(metaFileName, IniFormat)`;
  // writing through the same class keeps the on-disk bytes identical in
  // shape to a meta MO2 wrote, which is the point of using its format.
  QSettings s(QString::fromStdString(meta_path.string()), QSettings::IniFormat);

  // MO2 keys, for everything MO2 has a slot for.
  s.setValue("gameName", QString::fromStdString(meta.nexus_domain));
  s.setValue("modID", QString::fromStdString(meta.parent_mod_id));
  s.setValue("fileID", meta.file_id);
  s.setValue("name", QString::fromStdString(meta.name));
  s.setValue("url", QString::fromStdString(meta.page_url));
  s.setValue("repository", QString::fromStdString(meta.source));
  s.setValue("category", QString::fromStdString(meta.category));
  // installed/uninstalled/paused/removed, MO2 downloadmanager.cpp:1794-1798.
  // Failed is our equivalent of MO2's STATE_ERROR, which MO2 also writes as
  // paused. Downloading and Paused join it deliberately: a transfer cannot
  // survive the restart - the resume links are in-memory only - and
  // deserialize() already coerces both to Failed for exactly that reason.
  // Writing them as "nothing set" would read back as STATE_READY, i.e. a
  // finished download, for a row whose bytes were never fetched. MO2 encodes
  // the same fact in the ".unfinished" file-name suffix
  // (downloadmanager.cpp:111-114) instead of a flag; ours is a flag, and the
  // outcome is the same.
  s.setValue("installed", meta.state == DownloadState::Installed ||
                              meta.state == DownloadState::Uninstalled);
  // MO2 writes this too (downloadmanager.cpp:1795), and its derivation reads
  // it BEFORE "installed" (:118-126). Both flags land for a superseded archive,
  // which is MO2's own shape and not an accident: its markInstalled sets
  // installed=true and clears uninstalled (:1549-1552), its markUninstalled sets
  // uninstalled=true and LEAVES installed alone (:1588-1591) - because the
  // archive WAS installed once, and the derivation order, not a cleared flag, is
  // what makes the row read Uninstalled. Writing installed=false here would be
  // the one shape MO2 cannot produce, and it would make the order dead code
  // here while the file itself stays readable by MO2 either way.
  s.setValue("uninstalled", meta.state == DownloadState::Uninstalled);
  s.setValue("paused", meta.state == DownloadState::Failed ||
                           meta.state == DownloadState::Downloading ||
                           meta.state == DownloadState::Paused);
  s.setValue("removed", meta.hidden);

  // Our keys. QSettings ignores what it does not know, so MO2 reads the file
  // above without tripping over these.
  s.setValue("id", QString::fromStdString(meta.id));
  s.setValue("totalSize", static_cast<qlonglong>(meta.total_size));
  s.setValue("partialFile", QString::fromStdString(meta.partial_file));

  s.sync();
  return s.status() == QSettings::NoError;
}

std::optional<DownloadMeta> read_download_meta(const std::filesystem::path &meta_path) {
  QSettings s(QString::fromStdString(meta_path.string()), QSettings::IniFormat);
  // An empty or garbage file parses to no keys at all. That is NOT a
  // zero-valued meta (which would read as a finished download) - it is an
  // unreadable one, and the caller must treat it as unknown.
  if (s.allKeys().isEmpty())
    return std::nullopt;

  DownloadMeta meta;
  // The name IS the path: "<archive>.meta" -> "<archive>".
  meta.archive      = meta_path.parent_path() / meta_path.stem().string();
  meta.partial_file = s.value("partialFile").toString().toStdString();
  // A row that never landed its bytes was written beside its PARTIAL (that is
  // all it had on disk). Leaving "archive" pointing at the partial would hand
  // the restore path the same file as both archive and partial, so drop it and
  // let partialFile carry the row, exactly as the legacy manifest did.
  if (!meta.partial_file.empty() && meta.archive.filename() == meta.partial_file)
    meta.archive.clear();
  meta.id            = s.value("id").toString().toStdString();
  meta.name          = s.value("name").toString().toStdString();
  meta.source        = s.value("repository").toString().toStdString();
  meta.page_url      = s.value("url").toString().toStdString();
  meta.parent_mod_id = s.value("modID").toString().toStdString();
  meta.file_id       = s.value("fileID").toInt();
  meta.nexus_domain  = s.value("gameName").toString().toStdString();
  meta.category      = s.value("category").toString().toStdString();
  meta.total_size    = static_cast<int64_t>(s.value("totalSize").toLongLong());
  meta.hidden        = s.value("removed", false).toBool();

// MO2's derivation order (downloadmanager.cpp:118-126), verbatim and in the
  // same order: paused, then UNINSTALLED, then installed, then ready. "removed"
  // is not part of it: it is visibility, not state, and folding it into the
  // state would relabel every hidden row as Removed.
  //
  // "uninstalled" is read BEFORE "installed" because MO2's markUninstalled sets
  // only that one flag (downloadmanager.cpp:1588-1591) and its markInstalled
  // clears it (:1549-1552) - so a meta carrying both is MO2's own shape for a
  // superseded archive, and reading them the other way round would put every one
  // of those rows back on screen as "Installed".
  if (s.value("paused", false).toBool())
    meta.state = DownloadState::Failed;
  else if (s.value("uninstalled", false).toBool())
    meta.state = DownloadState::Uninstalled;
  else if (s.value("installed", false).toBool())
    meta.state = DownloadState::Installed;
  else
    meta.state = DownloadState::Complete;

  // A meta that never learned its own row key (older writer, hand-edited
  // file) still needs one, or the row cannot be addressed. The archive name
  // is the same fallback the legacy manifest used for a Manual row.
  if (meta.id.empty())
    meta.id = meta.archive.stem().string();
  return meta;
}

// MO2's orphan sweep, downloadmanager.cpp:409-422, reads the directory and
// deletes every "*.meta" whose base file is gone ("sounds cruel but it's better
// for everyone"). We match it - with one thing MO2 has no way to know, and that
// is what its "paused" flag exists for (downloadmanager.cpp:1796-1797, where
// STATE_ERROR is written as paused too).
//
// MO2 IS safe here, by layout rather than by logic, and the reason is worth
// keeping straight: createMetaFile names the meta from m_Output.fileName()
// (downloadmanager.cpp:1775), and m_Output is the file the bytes are actually
// being written to - which in flight carries ".unfinished" (the rename at
// downloadmanager.cpp:247-264). So an in-progress download's meta is
// "<name>.unfinished.meta", its base file IS the file holding the bytes, and
// MO2's own base-file check keeps it. It does NOT sweep in-progress metas
// blind.
//
// We keep the extra guard anyway, because WE can produce a shape MO2 never
// has: we write "paused" into "<name>.meta" itself, with no "<name>" on disk
// to match it. Under MO2's rule that meta is an orphan; under ours it is the
// only record of an in-progress download. So the meta is asked before anything
// is deleted:
//
//   swept - no archive on disk AND no partial on disk AND not "paused".
//           A finished download whose archive the user deleted from outside
//           the app. The meta is the only thing left of it and it describes
//           nothing, which is the pile-up the user asked us to stop keeping.
//   kept  - no archive, but a partial IS on disk (the row was written beside
//           its partial, download_meta_path below), or state is Failed.
//           Failed is how "paused" comes back (read_download_meta: "paused" ->
//           Failed) and it also covers Downloading and Paused, all of which we
//           write as paused. Those rows have no completed archive yet, and their
//           meta is the only record of what was being fetched - deleting it
//           permanently forgets an in-progress download. A SHORT archive is
//           kept for the same reason and by a different route: it exists, so
//           it is not an orphan at all, and deserialize()'s
//           archive_is_complete() check (downloads_tab.cpp:2115) restores
//           it Failed so a fresh link can act on it.
static bool meta_is_orphan(const DownloadMeta &meta,
                           const std::filesystem::path &downloads_dir) {
  std::error_code ec;
  if (!meta.archive.empty() && std::filesystem::is_regular_file(meta.archive, ec))
    return false;
  if (!meta.partial_file.empty() &&
      std::filesystem::is_regular_file(downloads_dir / meta.partial_file, ec))
    return false;
  return meta.state != DownloadState::Failed;
}

std::vector<DownloadMeta>
read_download_metas(const std::filesystem::path &downloads_dir) {
  std::vector<DownloadMeta> out;
  if (downloads_dir.empty() || !std::filesystem::is_directory(downloads_dir))
    return out;
  const QStringList files = QDir(QString::fromStdString(downloads_dir.string()))
                                .entryList({QStringLiteral("*.meta")}, QDir::Files);
  int orphans             = 0;
  for (const auto &f : files) {
    auto meta_path = downloads_dir / f.toStdString();
    if (auto meta = read_download_meta(meta_path)) {
      if (meta_is_orphan(*meta, downloads_dir)) {
        // MO2 uses shellDelete(orphans, true) here, i.e. the trash bin, so the
        // deletion is recoverable. Same call, same default (permanent=false).
        if (!engine::remove_path(meta_path))
          out.push_back(*meta);  // undeletable: keep the row rather than lose it
        else
          ++orphans;
        continue;
      }
      out.push_back(*meta);
      continue;
    }
    // Corrupt meta: keep the row rather than dropping it, and let it read
    // back Failed. Silently reading a damaged meta as a finished download
    // is the failure this format was adopted to prevent.
    engine::Logger::instance().warn("downloads: meta '" + meta_path.string() +
                                    "' holds no readable keys - restored as failed");
    DownloadMeta broken;
    broken.archive = meta_path.parent_path() / meta_path.stem().string();
    broken.id      = broken.archive.stem().string();
    broken.state   = DownloadState::Failed;
    out.push_back(std::move(broken));
  }
  if (orphans > 0)
    engine::Logger::instance().debug("downloads: " + std::to_string(orphans) +
                                     " orphaned meta files were deleted");
  return out;
}

bool write_download_metas(const std::vector<DownloadMeta> &metas,
                          const std::filesystem::path &downloads_dir) {
  bool all_ok = true;
  for (const auto &meta : metas) {
    if (!write_download_meta(meta, download_meta_path(meta, downloads_dir))) {
      engine::Logger::instance().warn("downloads: could not write meta for '" +
                                      meta.id + "'");
      all_ok = false;
    }
  }
  return all_ok;
}

bool migrate_download_state(const std::vector<DownloadMeta> &metas,
                            const std::filesystem::path &downloads_dir,
                            const std::filesystem::path &legacy_manifest) {
  // Write everything first, remove the legacy manifest only after every meta
  // has landed. A crash partway through therefore always leaves the old
  // manifest fully intact and the next run simply repeats the migration
  // (writing the same meta twice is idempotent), so the user is never left
  // with neither the old state nor a valid new one.
  if (!write_download_metas(metas, downloads_dir)) {
    engine::Logger::instance().warn("downloads: migration incomplete - '" +
                                    legacy_manifest.string() +
                                    "' kept, will be retried next run");
    return false;
  }
  std::error_code ec;
  std::filesystem::remove(legacy_manifest, ec);
  if (ec)
    engine::Logger::instance().warn("downloads: could not remove migrated '" +
                                    legacy_manifest.string() +
                                    "'; it will be re-migrated next run");
  return !ec;
}

std::string metas_to_manifest_json(const std::vector<DownloadMeta> &metas,
                                   const std::filesystem::path &downloads_dir) {
  QJsonArray arr;
  for (const auto &meta : metas) {
    QJsonObject obj;
    obj["id"]     = QString::fromStdString(meta.id);
    obj["name"]   = QString::fromStdString(meta.name);
    obj["source"] = QString::fromStdString(meta.source);
    obj["size"]   = meta.total_size > 0 ? format_size(meta.total_size) : QString();
    // The archive path is not stored in the meta - it IS the meta's name, and
    // this is where that fact reaches the restore path.
    obj["file_path"]     = QString::fromStdString(meta.archive.string());
    obj["partial_file"]  = QString::fromStdString(meta.partial_file);
    obj["state"]         = static_cast<int>(meta.state);
    obj["total_size"]    = static_cast<qint64>(meta.total_size);
    obj["parent_mod_id"] = QString::fromStdString(meta.parent_mod_id);
    obj["file_id"]       = meta.file_id;
    obj["domain"]        = QString::fromStdString(meta.nexus_domain);
    obj["category"]      = QString::fromStdString(meta.category);
    obj["page_url"]      = QString::fromStdString(meta.page_url);
    obj["hidden"]        = meta.hidden;
    arr.append(obj);
  }
  QJsonDocument doc(arr);
  return doc.toJson(QJsonDocument::Compact).toStdString();
}

// --- Manifest persistence ---

std::vector<DownloadMeta> DownloadsTab::to_metas() const {
  std::vector<DownloadMeta> metas;
  metas.reserve(downloads_.size());
  for (const auto &[id, entry] : downloads_) {
    DownloadMeta meta;
    meta.archive = entry.file_path;
    meta.id      = id;
    meta.name = entry.name_item ? entry.name_item->text().toStdString() : std::string{};
    meta.source =
        entry.source_item ? entry.source_item->text().toStdString() : std::string{};
    meta.page_url      = entry.page_url;
    meta.parent_mod_id = entry.parent_mod_id;
    meta.file_id       = entry.file_id;
    meta.nexus_domain  = entry.nexus_domain;
    meta.category      = entry.category;
    meta.total_size    = entry.total_size;
    meta.partial_file  = entry.partial_path.empty()
                             ? std::string{}
                             : entry.partial_path.filename().string();
    meta.state         = entry.state;
    meta.hidden        = entry.hidden;
    metas.push_back(std::move(meta));
  }
  return metas;
}

std::string DownloadsTab::serialize() const {
  QJsonArray arr;
  for (const auto &[id, entry] : downloads_) {
    QJsonObject obj;
    obj["id"]        = QString::fromStdString(id);
    obj["name"]      = entry.name_item ? entry.name_item->text() : QString();
    obj["source"]    = entry.source_item ? entry.source_item->text() : QString();
    obj["size"]      = entry.size_item ? entry.size_item->text() : QString();
    obj["file_path"] = QString::fromStdString(entry.file_path.string());
    // Just the name: the partial always lives in the downloads dir, so the
    // dir is re-applied on load. Persisted because a row that was paused at
    // shutdown comes back Failed (deserialize below) and still has to be able
    // to take its orphan with it.
    obj["partial_file"] =
        QString::fromStdString(entry.partial_path.filename().string());
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
    auto obj       = val.toObject();
    auto id        = obj["id"].toString().toStdString();
    auto name      = obj["name"].toString().toStdString();
    auto source    = obj["source"].toString().toStdString();
    auto size_text = obj["size"].toString();
    auto file_path = std::filesystem::path(obj["file_path"].toString().toStdString());
    const auto partial_file  = obj["partial_file"].toString().toStdString();
    auto state               = static_cast<DownloadState>(obj["state"].toInt());
    auto total_size          = static_cast<int64_t>(obj["total_size"].toDouble());
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

    // Verify the archive still describes a finished download. "Exists" is not
    // that: a 0-byte or half-written file exists, and restoring it as
    // Complete/Installed would make blocks_refetch refuse the link forever
    // against bytes that were never fetched. Anything short of a finished
    // archive comes back Failed, which is a state a fresh link can act on.
    if (state == DownloadState::Complete || state == DownloadState::Installed) {
      if (!file_path.empty() && !archive_is_complete(file_path, total_size)) {
        // Try relative to downloads_dir
        auto abs_path = downloads_dir / file_path.filename();
        if (archive_is_complete(abs_path, total_size))
          file_path = abs_path;
      }
      if (!archive_is_complete(file_path, total_size)) {
        engine::Logger::instance().warn(
            "downloads: '" + id + "' claims " + state_label(state).toStdString() +
            " but its archive is missing, empty or short - marked failed");
        state = DownloadState::Failed;
      }
    }

    auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
    if (!inserted)
      continue;

    auto &entry     = it->second;
    entry.row       = table_->rowCount();
    entry.file_path = file_path;
    entry.partial_path =
        partial_file.empty() ? std::filesystem::path{} : downloads_dir / partial_file;
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
      } else if (state == DownloadState::Cancelled) {
        // Same red as a failure: the row is not going to finish on its own,
        // and MO2 paints STATE_CANCELED through the same failure branch
        // (downloadmanager.cpp:1665-1668 calls m_DownloadFailed).
        bg = QColor("#f44336");
        fg = QColor(Qt::white);
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
