#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <string>

#include "ui/main_window/main_window.h"

namespace engine {
class ModMeta;
}  // namespace engine

namespace ui {

// Downloads & saves: NXM/LoversLab download routing, the Downloads and Saves
// tabs, the download manifest, install-progress popup, and the pipeline
// wiring (PipelineThread + source providers + NXM IPC). Split out of the
// 7211-line main_window.cpp (Issue #16).
class DownloadsController : public QObject {
  Q_OBJECT
public:
  explicit DownloadsController(MainWindow *w, QObject *parent = nullptr);

  // Creates the pipeline thread, registers source providers, and connects
  // the pipeline signals (download/install progress, completion, pause).
  // Called from the MainWindow ctor.
  void setup_pipeline();
  // Starts the NXM IPC server and routes received nxm:// URLs.
  void setup_nxm_ipc();
  // Ask the user before closing with active downloads (respects the "Don't
  // Ask" preference). Returns true when the close may proceed; the caller
  // (MainWindow::closeEvent) ignores the event otherwise.
  bool confirm_close();

  // Ask the mod update database about the current game, gated on the
  // "check for mods for updates after install" setting. Called after a mod
  // finishes installing; a no-op when the setting is off or no game is bound.
  // This is a MOD update check, never an application update check.
  void check_mod_updates_after_install();

  // Fill in the version an install left blank. An install rewrites meta.ini
  // from scratch and nothing in the download path knows a version, so the
  // Version column reads "?" until Nexus is asked. One files.json request for
  // the file that was just installed - a few a day against a 20000/day cap -
  // then the version is written into meta.ini and pushed into the row. A
  // no-op for a non-Nexus mod, a mod that already records one, or a mod with
  // no API key.
  void record_installed_version(const std::string &folder_name);

public:
  // Record the version Nexus answered with into one mod's meta.ini and repaint
  // its row. Split out of record_installed_version so the write is one named
  // step rather than a lambda buried in a thread callback - it runs on the GUI
  // thread, once, after the request has come back, and it is the step that has
  // to be right that the version lands in the row the user is looking at.
  static void apply_installed_version(ui::MainWindow *w,
                                      const std::filesystem::path &mods_dir,
                                      const std::string &folder_name,
                                      const std::string &version);

  // Clear the update-available symbol when the version that was just recorded
  // IS the newest the sweep knows about. Both version-recording paths call this
  // (record_installed_version and apply_installed_version), because either one
  // can be the version that makes a sweep's verdict stale, and neither knew
  // about the other. Public so the e2e test can drive the real step rather than
  // a stand-in for it.
  static void reconcile_update_symbol(ui::MainWindow *w,
                                      const std::filesystem::path &mods_dir,
                                      const std::string &folder_name,
                                      engine::ModMeta &meta);

public slots:
  void save_download_manifest();
  void load_download_manifest();
  // Wire the Downloads tab for the current instance: load its manifest,
  // point it at the instance downloads dir (starts the watchdog), and
  // connect its install/pause/resume/removal signals. Called after every
  // right-panel rebuild (startup and instance switch).
  void wire_downloads_tab();
  // Wire the Saves tab for the current instance: point it at the game's
  // saves dir (starts the directory watchdog), connect its refresh/delete
  // signals, and run an initial scan. Called after every right-panel rebuild.
  void wire_saves_tab();
  void on_saves_refresh_requested();
  void on_saves_delete_requested(const QStringList &filepaths);
  // User picked a save > Information... Opens the Save Information dialog
  // with a LIVE snapshot of the current load order (PluginDb::Database).
  // The snapshot is taken at click time, not at scan time, so toggles that
  // happened between scan and click are reflected.
  void on_save_information_requested(int row);
  void on_save_fix_missing_requested(int row);
  // "Query Info" on a download whose identity never resolved (MO2
  // DownloadManager::queryInfoMd5): hash the archive's contents, ask the
  // row's source - through the source layer's query_info(), never a source
  // client directly - which mod/file they belong to, and write what came
  // back into the row (and, through the next manifest save, its per-file
  // meta). Runs the hash + request off the GUI thread; the row is updated
  // when the answer lands. A source that cannot identify the download
  // confidently is reported and left untouched - a guessed match would
  // write wrong metadata permanently.
  void query_download_info(const std::string &id,
                           const std::filesystem::path &file_path,
                           const std::string &source_type, const std::string &page_url,
                           const std::string &domain);
  void update_install_progress(const std::string &mod_id, int percent,
                               const std::string &status);
  void hide_install_progress();
  void handle_nxm_download(const engine::Uri::NxmLink &link);
  void handle_modl_download(const engine::Uri::ModlLink &link);
  void start_loverslab_download(const std::string &url);

private:
  MainWindow *w_ = nullptr;
};

}  // namespace ui