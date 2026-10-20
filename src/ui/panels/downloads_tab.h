#pragma once

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/source/interface.h"

class QCheckBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QFileSystemWatcher;
class QMenu;
class QProgressBar;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QTimer;

namespace ui {

class TaskDialog;

// Testable seam for the Remove Download confirmation (Workspace-iqry): fills
// a TaskDialog with the Remove Download title, the archive file name as the
// main text, a trash-restore note as content, a Question icon, and Yes/No
// command links. Shared between the tab and the dialog tests.
void configure_remove_download_dialog(TaskDialog &dlg, const QString &file_name);

enum class DownloadState {
  Downloading,
  Paused,
  Complete,
  Installed,
  Failed,
  // Not implemented yet: reserved so manifests and rendering stay stable.
  Removed,
  // The user aborted the transfer instead of pausing it (MO2
  // DownloadManager::cancelDownload, downloadmanager.cpp:1002-1013). Paused
  // keeps its ".unfinished" partial so a resume picks up where it stopped;
  // Cancelled throws the partial away, so the row can only be deleted or
  // downloaded again from scratch. APPEND ONLY: the state is persisted into
  // the manifest as an int (serialize, downloads_tab.cpp), so a value
  // inserted above would silently re-label every row written by an older
  // build.
  Cancelled,
  // A download that WAS installed and no longer is, because a newer archive of
  // the same mod replaced the folder (MO2 STATE_UNINSTALLED,
  // downloadmanager.h:153). MO2 emits modReplaced with the superseded
  // archive's installationFile (installationmanager.cpp:418-424) and
  // MainWindow::modRemoved routes it to DownloadManager::markUninstalled
  // (mainwindow.cpp:507-510, :2552-2557), which sets exactly one meta flag:
  // "uninstalled" = true (downloadmanager.cpp:1588-1591). It reads "Uninstalled"
  // on the row (downloadlist.cpp:197-198) in dark yellow (:209-210).
  //
  // Without it the superseded archive reads "Install" forever, which is the row
  // that tells the user to install the version they just replaced.
  //
  // APPEND ONLY, same reason as Cancelled: the state is persisted into the
  // per-file meta as a DERIVED value and into the legacy JSON manifest as an
  // int, so a value inserted above would silently re-label every row written by
  // an older build.
  Uninstalled
};

// What Enter / Delete / Space do on the selected download row, as a function
// of the row's state (MO2 DownloadListView::keyPressEvent,
// downloadlistview.cpp:330-360). A finished row takes Enter as install and
// Delete as remove; a running row takes Space as pause and Delete as CANCEL
// (MO2 routes Delete on a live row to issueCancel, downloadlistview.cpp:345-349,
// because there is no finished archive to trash); a paused, failed or
// cancelled row takes Space as resume (except cancelled, whose partial is
// gone) and Delete as remove. Anything else does nothing. Free function so the
// state mapping is testable without synthesising key events on the table.
enum class DownloadShortcut { None, Install, Remove, Pause, Resume, Cancel };

[[nodiscard]] DownloadShortcut download_shortcut_for(DownloadState state, int qt_key);

// Remaining-time estimate for the download progress bar's format string (MO2
// DownloadManager::downloadProgress, downloadmanager.cpp:1745-1750:
// "~%3" with MOBase::localizedTimeRemaining).
//
// Returns an empty string when no estimate is possible - unknown total, no
// speed yet, or nothing left to fetch - so the caller drops the segment
// rather than printing a nonsense "0s" on a row that has just started. A
// speed or remaining-bytes value below what rounds to a second is likewise
// empty: "<1s" is noise, not information.
//
// Single unit, largest that fits ("45s", "3m", "2h", "4d"). Free function so
// the mapping is testable without a live download.
[[nodiscard]] QString format_remaining_time(int64_t remaining_bytes,
                                            double bytes_per_second);

// Average speed over the time the current transfer has actually been running,
// MO2 parity: DownloadManager emits `downloadSpeed` as
// (total - preResumeSize) / elapsedSeconds (downloadmanager.cpp:2448-2451) and
// suppresses it entirely for the first five seconds, because "if the download
// is shorter than a few seconds, the result is way to inprecise".
//
// `downloaded` is the row's current byte count, `base` the count the transfer
// started from (the resume offset, so a resumed download is not averaged over
// bytes it did not fetch this run), `elapsed_seconds` the monotonic seconds
// since that start.
//
// Returns bytes per second, or 0 while no honest figure exists: under the
// five-second floor, with no elapsed time, or with no bytes moved yet. Callers
// drop the speed segment rather than print a rate that is mostly noise.
//
// Free function so the rule is testable without a live transfer.
[[nodiscard]] double average_download_speed(int64_t downloaded, int64_t base,
                                            double elapsed_seconds);

// How a dropped archive that collides with an existing file in the downloads
// dir should be handled. The default resolver shows an MO2-style question
// dialog; tests inject a stub.
enum class DropConflictAction { Overwrite, Rename, Ignore };

// Which rows a batch context-menu action covers (MO2
// DownloadListView::downloadContextMenu, downloadlistview.cpp:300-325:
// "Delete Installed/Uninstalled/All Downloads..." and
// "Hide Installed/Uninstalled/All...", plus "Un-Hide All...").
enum class DownloadBatch {
  Installed,    // rows whose mod is installed
  Uninstalled,  // rows that finished but are not installed
  All           // every row, whatever its state
};

// True when a row's state puts it in the batch's scope. A download still in
// flight is NOT in Installed or Uninstalled (it has not finished, so nothing
// about it is known) but IS in All - MO2's issueDeleteAll and issueRemoveFrom-
// ViewAll take every row. Removed is in no scope: the entry is already gone.
//
// Free function so the scope rules are testable without building a tab.
[[nodiscard]] bool download_in_batch(DownloadState state, DownloadBatch batch);

// Label for a batch action, as MO2 words it ("Delete Installed Downloads...",
// "Hide Uninstalled...", "Un-Hide All..."). Empty scope names nothing.
[[nodiscard]] QString download_batch_label(DownloadBatch batch, bool remove);

// --- Per-file download meta (MO2 downloadmanager.cpp:88-164, 1771-1799) ---
//
// MO2 does not keep download state in one manifest. It writes a
// "<archive>.meta" file BESIDE each archive in the downloads dir, in
// QSettings IniFormat, and rebuilds the list by scanning the dir for "*.meta"
// (downloadmanager.cpp:412). The meta travels with the file it describes, so
// a single corrupt blob cannot describe - or destroy - the rest of the
// folder.
//
// Our meta is that file, field for field, for every key MO2 has a slot for:
//   MO2 key        ours
//   gameName       nexus_domain     (the game's identifier)
//   modID          parent_mod_id    (Nexus mod page id; MO2 stores an int,
//                                   we store the id string and MO2's
//                                   value("modID",0).toInt() parses it back)
//   fileID         file_id
//   name           name
//   version        version          (the file's version string; filled by
//                                   Query Info, MO2 downloadmanager.cpp:1789)
//   url            page_url
//   repository     source label ("Nexus Mods" / "LoversLab" / "Manual")
//   category       category
//   installed / uninstalled / paused / removed   the state, as MO2's flags
//
// MO2 has NO format-version key in this file - its "version" key is the MOD's
// version (VersionInfo::parse, downloadmanager.cpp:151), not a schema
// version - so there is nothing to honour and nothing invented here. Unknown
// keys are simply ignored by QSettings, which is what keeps the two or three
// keys MO2 has no slot for harmless below.
struct DownloadMeta {
  // The archive this meta describes. Derived from the meta's own file name
  // ("<archive>.meta"), never stored inside the file - that is the whole
  // point of the format.
  std::filesystem::path archive;
  std::string id;        // row key: "<modID>-<fileID>", or the archive stem
  std::string name;      // display name
  std::string version;   // the file's version string (MO2's "version" key)
  std::string source;    // repository label
  std::string page_url;  // "url"
  std::string parent_mod_id;
  int file_id = 0;
  std::string nexus_domain;
  std::string category;
  // Our keys. MO2 derives size from the file on disk, but archive_is_complete()
  // (downloads_tab.cpp) compares the bytes present against the RECORDED
  // total_size - the P0 fix - so it has to survive the restart.
  int64_t total_size = 0;
  // Bare file name: the partial always lives in the downloads dir, so the dir
  // is re-applied on read, same as the legacy manifest did.
  std::string partial_file;
  DownloadState state = DownloadState::Complete;
  bool hidden         = false;
};

// "<archive>.meta" beside the archive. Falls back to the partial for a row
// that has not landed its bytes yet, and to the row id for a queued row with
// neither on disk, so every row owns exactly one meta and no row is silently
// dropped for having nowhere to write itself.
[[nodiscard]] std::filesystem::path
download_meta_path(const DownloadMeta &meta,
                   const std::filesystem::path &downloads_dir);

// Writes one meta. False if the file could not be written, which is the
// caller's cue to keep the legacy manifest and retry on the next run.
bool write_download_meta(const DownloadMeta &meta,
                         const std::filesystem::path &meta_path);

// Reads one meta. std::nullopt when the file holds no parseable keys at all -
// a truncated or garbage meta. Callers must NOT read that as "Complete": a
// default-constructed DownloadMeta is a finished download, which is exactly
// the "meta is stale against the filesystem" failure this format exists to
// remove, so the honest answer is "unknown" and the caller restores Failed.
[[nodiscard]] std::optional<DownloadMeta>
read_download_meta(const std::filesystem::path &meta_path);

// Every meta in the dir, MO2's "*.meta" scan (downloadmanager.cpp:412), after
// MO2's orphan sweep (downloadmanager.cpp:409-422).
//
// An orphan - a meta with no archive and no partial on disk - is DELETED, to
// the trash bin, exactly as MO2 does. But only when its own state says the
// work is not in progress: a "paused" meta (Failed / Downloading / Paused) is
// kept. MO2 does not need that guard (its in-flight meta is named
// "<name>.unfinished.meta" after the file the bytes are in, so its base file
// exists and MO2's own check keeps it); we do, because we write "paused" into
// "<name>.meta" with nothing at "<name>" - a shape MO2 never produces. See
// meta_is_orphan() in downloads_tab.cpp for the exact swept/kept list. The
// other two paths agree: remove_entry() deletes the meta of an explicit Remove,
// and scan_downloads_dir() drops vanished rows, but now only FINISHED ones - it
// used to drop a paused row whose archive had never landed, which deleted the
// meta this sweep keeps.
[[nodiscard]] std::vector<DownloadMeta>
read_download_metas(const std::filesystem::path &downloads_dir);

// Writes every meta. False if any single write failed.
bool write_download_metas(const std::vector<DownloadMeta> &metas,
                          const std::filesystem::path &downloads_dir);

// The one-way migration off the legacy single-file manifest: write one meta
// per entry, then remove the legacy manifest ONLY once every write landed.
// False if any meta failed, in which case the legacy manifest is left in
// place untouched and the next run repeats the whole thing - so a migration
// that dies halfway costs the user nothing and is retried, not lost.
bool migrate_download_state(const std::vector<DownloadMeta> &metas,
                            const std::filesystem::path &downloads_dir,
                            const std::filesystem::path &legacy_manifest);

// Re-renders the metas as the legacy manifest JSON that DownloadsTab::
// deserialize() consumes, so BOTH on-disk formats restore through ONE
// per-item path - the archive_is_complete() verification and the legacy
// source repair both live there and are not duplicated or re-implemented.
[[nodiscard]] std::string
metas_to_manifest_json(const std::vector<DownloadMeta> &metas,
                       const std::filesystem::path &downloads_dir);

class DownloadsTab : public QWidget {
  Q_OBJECT
public:
  // Display order is the column order.
  //
  // MO2 (references/modorganizer/src/downloadlist.h:38-50) shows 8 columns:
  // COL_NAME, COL_STATUS, COL_SIZE, COL_FILETIME, COL_MODNAME, COL_VERSION,
  // COL_ID, COL_SOURCEGAME. This tab carries 6, chosen against that list:
  //
  //   MO2 column      here
  //   --------------  -------------------------------------------
  //   Name            Name
  //   Status          Status
  //   Size            Size
  //   Filetime        Filetime
  //   Mod name        - our Name column already holds the name the source
  //                     gave the file; a second column would repeat it
  //   Version         - nothing in the download flow produces a version
  //                     string. The Nexus provider's file metadata
  //                     (resolve_download_info) carries only the archive
  //                     name and the display name, and the one place a
  //                     version exists (fetch_mod_info) is a separate,
  //                     API-key-gated mod-list lookup
  //   Nexus ID        NexusId
  //   Source Game     - a GMM instance is single-game and this tab is
  //                     already scoped to it, so there is nothing to show
  //
  // and one column MO2 does not have: Source, which site a file came from.
  //
  // APPEND ONLY. Unlike the mod list, this tab's header state is a
  // QHeaderView::saveState() blob keyed by the right-panel tab name
  // (SettingsController::save_app_state), and that blob stores per-section
  // width and hidden flag by INDEX. Inserting or reordering here would
  // land a saved width on the wrong header after a restart, so new
  // columns go at the end and the four that shipped before keep their
  // indices.
  //
  // Qt applies a SHORTER saved state to the leading sections only, and
  // leaves the rest visible - so a blob written by a build that predates a
  // column would unhide that column on the next launch. apply_default_
  // hidden_columns() has to run again after every restoreState() for that
  // reason; SettingsController::restore_app_state does it for this tab, and
  // skips it when the saved flags are the user's own (a saved blob records
  // visibility, not the choice behind it, so the header separately flags
  // whether its toggle menu was ever used).
  enum Column {
    Name,
    Source,
    Status,
    Size,
    Filetime,
    NexusId,
    ColumnCount
  };

  // Per-column header label, empty for an out-of-range index.
  [[nodiscard]] static QString column_name(int column);

  // Every column's label in column order. The table header and the header's
  // toggle menu both read this, so the two lists cannot drift apart the way a
  // hand-written menu label list did when Filetime and Nexus ID were appended.
  [[nodiscard]] static QStringList column_names();

  // Per-column hover text, indexed by column and always ColumnCount long.
  // MO2's download list has no header tooltips at all (only a cell-level
  // ToolTipRole in downloadlist.cpp:214), so these describe what this build
  // actually shows rather than copying wording that does not exist.
  [[nodiscard]] static QStringList header_tooltips();

  // Column names hidden on a fresh tab and re-applied after a saved header
  // state is restored. Deliberately not MO2's set: MO2 shows Filetime and
  // hides only COL_ID (downloadlistview.cpp:147-151), while this build ships
  // Name, Status and Size alone, so Source and Filetime join Nexus ID here.
  [[nodiscard]] static QStringList default_hidden_column_names();

  // Set every column's visibility from default_hidden_column_names() on
  // `table`'s header - hidden for a listed name, visible for every other, so
  // it also un-hides a column a saved state had left showing. Resolved by
  // name, so a renamed or retired entry is inert rather than moving a
  // column's visibility. Idempotent, and safe to call again after
  // restoreState().
  static void apply_default_hidden_columns(QTableWidget *table);

  explicit DownloadsTab(QWidget *parent = nullptr);
  [[nodiscard]] QTableWidget *table() const { return table_; }

  // Returns false when `id` already has a row, so the caller can tell a new
  // download from a repeat of one it already queued.
  bool add_download(const std::string &id, const std::string &name,
                    const std::string &source,
                    const std::filesystem::path &file_path = {},
                    const std::string &nexus_domain = {}, int file_id = 0,
                    const std::string &parent_mod_id = {},
                    const std::string &page_url      = {});
  void update_progress(const std::string &id, int64_t downloaded, int64_t total,
                       double speed);
  void mark_complete(const std::string &id, bool success);
  // `id` is the download row's key. `archive_filename` is the fallback lookup
  // for installs that did not come from this tab's own rows (a reinstall hands
  // over the mod FOLDER as the id), matched on the row's archive file name -
  // the lookup MO2 does when it marks a download installed
  // (downloadmanager.cpp:1556-1560, indexByName).
  void mark_installed(const std::string &id, const std::string &archive_filename = {});
  // A newer archive of the same mod replaced the folder this download
  // installed, so the row must stop claiming it is installed: it reads
  // "Uninstalled" (MO2 STATE_UNINSTALLED, downloadlist.cpp:198), not "Install",
  // which would tell the user to install the version they just replaced.
  // `archive_filename` is the bare name the engine read out of that mod's
  // meta.ini (ctx.replaced_archive), the same name MO2's modReplaced carries
  // (installationmanager.cpp:423). No-op when no finished row matches.
  void mark_uninstalled(const std::string &archive_filename);
  void mark_paused(const std::string &id);
  void mark_downloading(const std::string &id);
  // The user abandoned the transfer: the row reads Cancelled, its
  // ".unfinished" partial is deleted so the bytes are not left behind as a
  // file nothing owns, and the entry stays in the table (MO2 keeps a cancelled
  // row - STATE_CANCELED sits below STATE_READY in the enum, so
  // downloadmanager.cpp:327 keeps it in the list too) with no Resume action.
  void mark_cancelled(const std::string &id);

  // True when `id` already names a download that must not be fetched again.
  // Two cases, both of which lose data if a repeat link is queued again:
  // an in-flight transfer (a second one opens the same destination file for
  // writing while the first is still appending to it) and a finished one
  // (re-fetching resumes from a complete file, gets HTTP 416, and the failed
  // request removes the archive). A Failed row is deliberately NOT covered -
  // there is no Retry action on a row, so re-queueing its link is how a failed
  // download is retried. Neither is Paused: a fresh link means "carry on".
  //
  // Note that add_download() also starts its row in Downloading - that is the
  // pre-fetch state, before any worker runs. A caller that has just created the
  // row must therefore NOT ask this question about it: a brand new row is not a
  // transfer in flight, and the answer would be yes. Gate the call on
  // add_download() having returned false (the row already existed).
  [[nodiscard]] bool blocks_refetch(const std::string &id) const;

  // Update the displayed name of a download entry (e.g. the real mod name,
  // resolved from the source after the download was queued with a
  // placeholder). A no-op if the id is not present.
  void rename_download(const std::string &id, const std::string &new_name);

  // Record the on-disk archive for a download that was started without one
  // (e.g. Nexus downloads, whose path is only known after the fetch).
  void set_file_path(const std::string &id, const std::filesystem::path &path);

  // Record where an in-progress download is writing, from the archive name the
  // provider resolves before the bytes flow. The partial is
  // "<archive_name>.unfinished" in the downloads dir, so this is what makes a
  // Delete on a paused row take its partial with it.
  void note_partial_archive(const std::string &id, const std::string &archive_name);

  // Apply the metadata a Query Info lookup fetched (MO2
  // nxmFileInfoFromMd5Available -> createMetaFile): fills the row's identity
  // (Nexus mod id, file id, domain when the answer carries them), renames the
  // row to the display name, relabels a Manual row's Source cell to the
  // answering source so a later install routes through its provenance, and
  // clears the incomplete-info warning. The next save_download_manifest()
  // persists it into the per-file meta. An empty `name` leaves the row's
  // name alone.
  void apply_query_info(const std::string &id, const std::string &source_type,
                        const engine::Source::DownloadInfoResult &result);

  // Trash the entry's archive AND its ".unfinished" partial, then drop the
  // row. The workhorse behind every removal path (context menu, Delete key,
  // batch Delete); the MO2 "Move to the Recycle Bin" question lives in
  // confirm_and_remove(), so a caller that has already asked can come here
  // directly. Trashing nothing is not an option - that is what stranded the
  // bytes of every paused download.
  void remove_entry(const std::string &id);

  // Directory the instance downloads archives land in (for "Show in Folder"
  // when an entry has no file yet).
  void set_downloads_dir(const std::filesystem::path &dir);

  // True while any entry is still downloading or paused, so the app can warn
  // (and the tab can guard rescanning) while a fetch is in flight.
  bool has_active_download() const;

  // Remember the shared filter bar's current text so "hide installed"
  // re-applications never unhide rows the text filter hid. Called by
  // RightPanel::apply_filter on every filter change/tab switch.
  void set_filter_text(const QString &text);

  // Re-apply the "hide installed" filter on top of any other row filter.
  void reapply_installed_filter();

  // Re-read the compact-downloads setting and set explicit row heights
  // (MO2 standard/compact) so the look does not depend on any stylesheet.
  void apply_compact_style();

  // Replace the conflict resolver shown when a dropped archive's name
  // collides with an existing file in the downloads dir. Defaults to the
  // MO2-style question dialog; callers (tests) may inject a stub.
  using ConflictResolver =
      std::function<DropConflictAction(const std::filesystem::path &existing_file,
                                       const std::filesystem::path &dropped_file)>;
  void set_conflict_resolver(ConflictResolver resolver);

  // Persistence
  [[nodiscard]] std::string serialize() const;
  void deserialize(const std::string &json, const std::filesystem::path &downloads_dir);
  // The tab's rows as per-file metas, for writing one meta per archive.
  [[nodiscard]] std::vector<DownloadMeta> to_metas() const;

signals:
  void install_requested(const std::string &id, const std::filesystem::path &file_path,
                         const std::string &source_type, const std::string &source_id,
                         int file_id, const std::string &display_name,
                         const std::string &page_url = {});
  void pause_requested(const std::string &id);
  void resume_requested(const std::string &id);
  // The user chose Cancel on a running row (MO2 issueCancel,
  // downloadlistview.cpp:262-264). Distinct from pause_requested: the worker
  // aborts the same transfer but must report it as a cancel so the row is not
  // left resumable and its partial is discarded.
  void cancel_requested(const std::string &id);
  // Emitted with the raw pasted URL when the user triggers "Add from URL"
  // (LoversLab and other no-API sites). MainWindow validates and routes it.
  void loverslab_url_entered(const std::string &url);
  // Emitted after a download entry (and its file) has been removed, so the
  // manifest can be persisted. The entry is already gone from the table.
  void entry_removed(const std::string &id);
  // Emitted when entries changed but nothing was removed (the batch Hide
  // actions), so the manifest is rewritten without any link bookkeeping or a
  // kDownloadRemoved event - no row left the table.
  void entries_changed();
  // The user picked "Query Info" on a row whose identity never resolved
  // (MO2 issueQueryInfoMd5, downloadlistview.cpp:235). Carries the row key,
  // the archive on disk (the file whose contents get hashed) and the origin
  // the answer is asked for: the source_type the menu gate resolved, the
  // row's page URL and its game-domain hint (empty for a Manual row - the
  // controller falls back to the current game's domain).
  void query_info_requested(const std::string &id,
                            const std::filesystem::path &file_path,
                            const std::string &source_type,
                            const std::string &page_url,
                            const std::string &domain);

private slots:
  // The downloads dir changed on disk (watcher fired): (re)arm the debounce
  // timer so bursts from large copies coalesce into a single scan.
  void on_downloads_dir_changed();
  void on_scan_timer_timeout();

private:
  struct DownloadEntry {
    int row = -1;
    std::filesystem::path file_path;
    // Where an in-progress download is writing: "<archive>.unfinished" in the
    // downloads dir (fetch_stage.cpp:118-125). Nexus/modl/URL rows are created
    // with an EMPTY file_path and never get one until the bytes are all in, so
    // without this the row owns a partial nothing can find or remove - Delete
    // trashed nothing and left the bytes on disk forever. Recorded when the
    // provider resolves the archive name, cleared when the archive lands.
    std::filesystem::path partial_path;
    DownloadState state = DownloadState::Downloading;
    int64_t total_size  = 0;
    // Running average inputs (MO2 downloadSpeed, downloadmanager.cpp:2448):
    // the byte count the current transfer started from and the monotonic
    // moment it started. Both are reset by mark_downloading, so a resume
    // averages only the bytes that run actually fetched instead of reporting
    // the whole file's average as if it had just arrived. started ==
    // zero means "no transfer has been timed yet".
    int64_t speed_base       = 0;
    int64_t speed_started_ms = 0;
    // Origin metadata (Nexus): parent mod page id, file id, domain.
    std::string parent_mod_id;
    int file_id = 0;
    std::string nexus_domain;
    std::string category;
    // The file's version string, filled by Query Info (MO2's meta "version"
    // key). Persisted so save_download_manifest() does not clobber it.
    std::string version;
    // Source page URL (LoversLab: the download link minus the
    // ?do=download query). Persisted in the manifest so the "Open on ..."
    // context action and install provenance survive restarts.
    std::string page_url;
    // Hidden from the table without being deleted (MO2 DownloadInfo::m_Hidden
    // / DownloadManager::isHidden, downloadmanager.cpp:1523-1529). Persisted
    // in the manifest, so a row the user hid stays hidden across restarts and
    // "Un-Hide All..." can bring it back.
    bool hidden                     = false;
    QTableWidgetItem *name_item     = nullptr;
    QTableWidgetItem *source_item   = nullptr;
    QTableWidgetItem *size_item     = nullptr;
    QTableWidgetItem *filetime_item = nullptr;
    QProgressBar *progress_bar      = nullptr;
  };

  DownloadEntry &entry_for(const std::string &id);
  // Filetime cell content: the on-disk archive's mtime, empty while no
  // archive exists yet. Re-run whenever entry.file_path or the file changes.
  void update_filetime(DownloadEntry &entry);
  // Name-cell hover text (MO2 DownloadListModel::data ToolTipRole,
  // downloadlist.cpp:213-232). There is no Nexus description to show on a
  // download row in this build - the Version column was dropped for exactly
  // that reason - so the tooltip carries what the columns do not: the full
  // on-disk path, the source page, and a pending notice while no archive has
  // landed yet.
  void update_tooltip(DownloadEntry &entry);
  // MO2 DownloadManager::isInfoIncomplete (downloadmanager.cpp:1480-1492): a
  // Nexus download whose mod or file id never resolved is missing the identity
  // every other row carries and cannot be re-queried later. Other sources are
  // never incomplete - their identity is the page URL they were downloaded
  // from (LoversLab / ModPub) or nothing at all (Manual), so a warning would
  // name a fix that does not exist. A queued row has not been given an id yet,
  // which is why MO2 gates the same branch on state >= STATE_READY.
  static bool info_is_incomplete(const DownloadEntry &entry);
  void replace_bar_with_label(const std::string &id, const QString &text,
                              const QColor &bg, const QColor &fg);
  void on_cell_double_clicked(int row, int column);
  // Trash the entry's archive after the MO2 "Move to the Recycle Bin"
  // question. Shared by the context-menu Remove action and the Delete key
  // (MO2 issueDelete, downloadlistview.cpp:377-389).
  void confirm_and_remove(const std::string &id);
  void apply_installed_filter();

  // The batch context-menu actions (MO2 DownloadListView
  // ::downloadContextMenu, downloadlistview.cpp:300-325). The Hide family flags
  // the rows and re-applies the row filter, the Delete family trashes each
  // archive and drops the entry. Both confirm once for the whole batch rather
  // than per row.
  //
  // Confirm-then-trash every entry in the batch, and report the count. No-op
  // when the batch is empty.
  void remove_batch(DownloadBatch batch);
  // Flag the batch's rows hidden (or every row, for unhide) and re-apply the
  // row filter. Emits entry_removed-free: nothing is deleted, so the manifest
  // is rewritten through the existing entry change path.
  void hide_batch(DownloadBatch batch, bool unhide);

  // Derive the origin metadata for an install from a download entry:
  // source_type ("nexus"/"loverslab"/""), source_id, file_id, and the
  // source page URL. LoversLab rows key off the entry id (the file id) and
  // carry page_url; Nexus rows carry parent_mod_id/file_id. Mirrors the
  // Source column's literal strings (not tr()).
  struct SourceInfo {
    std::string source_type;
    std::string source_id;
    int file_id = 0;
    std::string page_url;
  };
  SourceInfo source_info_for(const std::string &id, const DownloadEntry &entry) const;

protected:
  // Fills `menu` with the actions for the download entry at `id` (install,
  // pause/resume, show in folder, source-aware "Open on ...", remove).
  // Split out of on_custom_context_menu so tests can drive it without
  // exec()-ing a modal menu (DataTab/PluginsTab pattern).
  void add_context_menu_actions(QMenu &menu, const std::string &id);

private:
  void on_custom_context_menu(const QPoint &pos);

protected:
  // Enter / Delete / Space on the selected row (MO2
  // DownloadListView::keyPressEvent). The state mapping lives in
  // download_shortcut_for; this only routes the chosen action to the same
  // primitives the context menu uses. Unhandled keys fall through to Qt.
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  // Move or copy a dropped local archive into downloads_dir_ and surface it
  // as a "Manual" Complete entry. Returns true if an entry was added.
  bool import_dropped_file(const std::filesystem::path &source, bool move);

  // Add a download entry for a file already sitting in downloads_dir_
  // (e.g. a drop that resolves to the same location, or one moved/copied in
  // by import_dropped_file). Returns true if an entry was added. Adds
  // nothing if the file is not an archive or already backs a tracked entry.
  bool add_downloads_dir_file(const std::filesystem::path &path);

  // Add untracked archives sitting in the downloads dir as "Manual"
  // Complete entries so they can be installed from the tab. Refresh the size
  // of tracked same-named files and remove rows whose archive no longer
  // exists. Skip files that already back a tracked entry and any scan while
  // a download is in flight (the in-progress archive would otherwise appear
  // as a bogus "Complete" row).
  void scan_downloads_dir();

  // MO2 standard/compact row height in pixels, derived from the current font
  // so text never clips. Compact ~ font + 8; standard ~ font + 22.
  int row_height() const;

protected:
  void showEvent(QShowEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dropEvent(QDropEvent *event) override;

  QTableWidget *table_       = nullptr;
  QCheckBox *hide_installed_ = nullptr;
  // MO2's showHiddenBox (mainwindow.ui:1426, "Hidden files"). A VIEW filter
  // over the persisted per-row hidden flags, not a state change: the flags
  // stay untouched, so unticking the box hides the same rows again. Not
  // persisted either, matching MO2 where m_ShowHidden starts false every run.
  bool show_hidden_ = false;
  std::unordered_map<std::string, DownloadEntry> downloads_;
  // Last filter text passed by RightPanel::apply_filter (trimmed, lowered),
  // re-applied by apply_installed_filter so it never unhides rows the text
  // filter hid. Empty when no text filter is active.
  QString current_filter_text_;
  std::filesystem::path downloads_dir_;
  ConflictResolver conflict_resolver_;
  QFileSystemWatcher *dir_watcher_ = nullptr;
  QTimer *scan_timer_              = nullptr;
};

}  // namespace ui
