#pragma once

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

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
  Removed
};

// How a dropped archive that collides with an existing file in the downloads
// dir should be handled. The default resolver shows an MO2-style question
// dialog; tests inject a stub.
enum class DropConflictAction { Overwrite, Rename, Ignore };

// Which rows a batch hide/delete covers. MO2 encodes this as one method with an
// index triple - -1 all, -2 installed, -3 uninstalled
// (downloadmanager.cpp:954-996) - and DownloadListView offers all three for
// both the hide and the delete family (downloadlistview.cpp:299-325). An enum
// says the same thing without the magic numbers.
enum class DownloadBatchScope {
  // Every row whose state is ready-or-beyond, i.e. MO2's `removeAll` test
  // `downloadState >= STATE_READY`. Downloading, Paused and Failed all sit
  // BELOW that threshold in MO2 and are deliberately not covered: a batch
  // delete must never take the archive out from under a transfer in flight,
  // and a paused or failed row is still a row the user can act on.
  All,
  // Rows the user has installed from (MO2 STATE_INSTALLED).
  Installed,
  // Rows sitting on disk waiting to be installed. MO2 has a distinct
  // STATE_UNINSTALLED for this; this build reports it as DownloadState::Complete
  // (see mark_uninstalled), so that is what the scope resolves to.
  Uninstalled
};

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
  enum Column { Name, Source, Status, Size, Filetime, NexusId, ColumnCount };

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
  void mark_installed(const std::string &id);
  // A Replace deleted the mod this download installed, so the row must stop
  // claiming it is installed. `archive_filename` is the bare name recorded in
  // that mod's meta.ini. No-op when no installed row matches.
  void mark_uninstalled(const std::string &archive_filename);
  void mark_paused(const std::string &id);
  void mark_downloading(const std::string &id);

  // True when `id` already names a download that must not be fetched again.
  // Two cases, both of which lose data if a repeat link is queued again:
  // an in-flight transfer (a second one opens the same destination file for
  // writing while the first is still appending to it) and a finished one
  // (re-fetching resumes from a complete file, gets HTTP 416, and the failed
  // request removes the archive). A Failed row is deliberately NOT covered -
  // there is no Retry action on a row, so re-queueing its link is how a failed
  // download is retried. Neither is Paused: a fresh link means "carry on".
  [[nodiscard]] bool blocks_refetch(const std::string &id) const;

  // Update the displayed name of a download entry (e.g. the real mod name,
  // resolved from the source after the download was queued with a
  // placeholder). A no-op if the id is not present.
  void rename_download(const std::string &id, const std::string &new_name);

  // Record the on-disk archive for a download that was started without one
  // (e.g. Nexus downloads, whose path is only known after the fetch).
  void set_file_path(const std::string &id, const std::filesystem::path &path);

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

  // --- Hidden state (MO2 DownloadManager m_Hidden) ------------------------
  //
  // Real per-entry state, not a view toggle: MO2 persists it alongside every
  // other download property (downloadmanager.cpp:1798 writes the sidecar key
  // "removed" when a row is hidden, :101 reads it back), so a hidden download
  // stays hidden across restarts. Here it rides in the JSON manifest that
  // DownloadsTab::serialize already writes, under the key "hidden".
  [[nodiscard]] bool is_hidden(const std::string &id) const;
  void set_hidden(const std::string &id, bool hidden);
  // True when any entry carries the hidden flag, whatever its state - the
  // condition for offering "Un-Hide All" rather than the hide family.
  [[nodiscard]] bool has_hidden_entry() const;

  // MO2's "Hidden files" checkbox (mainwindow.ui:1426 showHiddenBox), which
  // toggles DownloadManager::setShowHidden (mainwindow.cpp:3818-3820) - i.e.
  // whether hidden entries are listed at all. Like "hide installed" this is a
  // view preference, so it is a Settings key rather than manifest state.
  [[nodiscard]] bool show_hidden() const;
  void set_show_hidden(bool on);

  // --- Batch operations (MO2 downloadlistview.cpp:299-325) ----------------
  //
  // Each returns how many entries it changed, so a caller can tell "nothing
  // matched the scope" from "everything did" without asking the table.
  int unhide_all();
  int delete_downloads(DownloadBatchScope scope);

  // Seam for the batch confirmations. Returns true when the user agrees.
  // The default asks through the MO2-style warning box; tests inject a stub.
  // (Same pattern as ConflictResolver below.)
  using BatchConfirm =
      std::function<bool(const QString &title, const QString &message)>;
  void set_batch_confirmer(BatchConfirm confirm);

  // Seam for the PER-ROW delete confirmation, shared by the Delete key and the
  // context menu's Remove. Takes the archive's file name, returns true when the
  // user agrees. Unset means the Remove Download question
  // (configure_remove_download_dialog); tests inject a stub so the Delete
  // shortcut's own path is drivable without a modal.
  using RowDeleteConfirm = std::function<bool(const QString &file_name)>;
  void set_row_delete_confirm(RowDeleteConfirm confirm);

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

signals:
  void install_requested(const std::string &id, const std::filesystem::path &file_path,
                         const std::string &source_type, const std::string &source_id,
                         int file_id, const std::string &display_name,
                         const std::string &page_url = {});
  void pause_requested(const std::string &id);
  void resume_requested(const std::string &id);
  // Emitted with the raw pasted URL when the user triggers "Add from URL"
  // (LoversLab and other no-API sites). MainWindow validates and routes it.
  void loverslab_url_entered(const std::string &url);
  // Emitted after a download entry (and its file) has been removed, so the
  // manifest can be persisted. The entry is already gone from the table.
  void entry_removed(const std::string &id);

private slots:
  // The downloads dir changed on disk (watcher fired): (re)arm the debounce
  // timer so bursts from large copies coalesce into a single scan.
  void on_downloads_dir_changed();
  void on_scan_timer_timeout();

private:
  struct DownloadEntry {
    int row = -1;
    std::filesystem::path file_path;
    DownloadState state = DownloadState::Downloading;
    int64_t total_size  = 0;
    // Origin metadata (Nexus): parent mod page id, file id, domain.
    std::string parent_mod_id;
    int file_id = 0;
    std::string nexus_domain;
    std::string category;
    // Source page URL (LoversLab: the download link minus the
    // ?do=download query). Persisted in the manifest so the "Open on ..."
    // context action and install provenance survive restarts.
    std::string page_url;
    // Hidden from the list (MO2 m_Hidden). Persisted in the manifest and
    // independent of the "Hide installed" filter, which is a view
    // preference rather than a fact about the entry.
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
  void replace_bar_with_label(const std::string &id, const QString &text,
                              const QColor &bg, const QColor &fg);
  void on_cell_double_clicked(int row, int column);
  void remove_entry(const std::string &id);
  // Confirm and remove one entry, then its archive. The single path behind both
  // the Delete key and the context menu's Remove.
  bool delete_one(const std::string &id);
  // The one row-visibility pass: hidden entries, then "hide installed", then
  // the shared text filter - in that order, so the first two always win.
  void apply_row_filter();

  // Which states a batch operation covers, per DownloadBatchScope's
  // documented scope. A batch delete must never take the archive out from
  // under a transfer in flight, so Downloading and Paused are never in scope.
  [[nodiscard]] static bool in_batch_scope(DownloadState state,
                                           DownloadBatchScope scope);
  // How many entries `scope` covers right now, for the confirmation text.
  [[nodiscard]] int batch_scope_count(DownloadBatchScope scope) const;

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
  // pause/resume, show in folder, source-aware "Open on ...", hide/un-hide,
  // remove).
  // Split out of on_custom_context_menu so tests can drive it without
  // exec()-ing a modal menu (DataTab/PluginsTab pattern).
  void add_context_menu_actions(QMenu &menu, const std::string &id);

  // The per-row Hide / Un-Hide action, kept separate so the state gating
  // (a hidden row offers Un-Hide, a listed one offers Hide) is one line.
  void add_hide_action(QMenu &menu, const std::string &id);

  // MO2 DownloadListView::keyPressEvent (downloadlistview.cpp:330-359): drive
  // the current row by key, gated on what its state allows. Returns true when
  // the key was consumed and must not reach the view.
  //
  // Exposed (and the event filter below is its only caller in the app) so
  // tests can drive the whole gating table without synthesizing key events at
  // whatever widget happens to hold focus.
  bool handle_download_key(int key);

  // Confirm the batch `scope` with the user and apply it. Split out of the
  // menu builders so tests drive the whole action, confirmation included,
  // without exec()-ing a modal box (same pattern as
  // add_context_menu_actions). Returns false when the user declined or the
  // scope matched nothing.
  bool run_batch_delete(DownloadBatchScope scope);

private:
  void on_custom_context_menu(const QPoint &pos);

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

  // Scoped to table_ and its viewport ONLY. That is what makes the keyboard
  // shortcuts safe: an Enter / Delete / Space meant for a text field, a
  // checkbox or a dialog elsewhere on the tab never reaches the filter,
  // because those widgets are not being watched. A QShortcut on the tab or
  // the table would fire for those instead (SavesTab filters for Delete;
  // DataTab uses shortcuts and has to scope them by hand).
  bool eventFilter(QObject *watched, QEvent *event) override;

  QTableWidget *table_       = nullptr;
  QCheckBox *hide_installed_ = nullptr;
  // MO2's showHiddenBox (mainwindow.ui:1426). Kept as a member so
  // set_show_hidden() drives the same path the checkbox does and
  // apply_row_filter() reads one source of truth for the view preference.
  QCheckBox *show_hidden_ = nullptr;
  std::unordered_map<std::string, DownloadEntry> downloads_;
  // Last filter text passed by RightPanel::apply_filter (trimmed, lowered),
  // re-applied by apply_row_filter so it never unhides rows the text
  // filter hid. Empty when no text filter is active.
  QString current_filter_text_;
  std::filesystem::path downloads_dir_;
  ConflictResolver conflict_resolver_;
  BatchConfirm batch_confirm_;
  RowDeleteConfirm row_delete_confirm_;
  QFileSystemWatcher *dir_watcher_ = nullptr;
  QTimer *scan_timer_              = nullptr;
};

}  // namespace ui
