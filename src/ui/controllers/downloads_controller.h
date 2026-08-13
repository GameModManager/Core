#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <string>

#include "ui/main_window/main_window.h"

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
  void update_install_progress(const std::string &mod_id, int percent,
                               const std::string &status);
  void hide_install_progress();
  void handle_nxm_download(const engine::NxmLink &link);
  void start_loverslab_download(const std::string &url);

private:
  MainWindow *w_ = nullptr;
};

} // namespace ui