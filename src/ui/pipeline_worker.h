#pragma once

#include "engine/pipeline/pipeline.h"
#include "engine/nxm/nxm_router.h"

#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace engine {
class Pipeline;
struct Mod;
}

namespace ui {

class PipelineWorker : public QObject {
    Q_OBJECT
public:
    explicit PipelineWorker(QObject* parent = nullptr);
    ~PipelineWorker();

    void set_pipeline(std::unique_ptr<engine::Pipeline> pipeline);
    void set_context(engine::PipelineContext ctx);

public slots:
    // Install an already-downloaded archive (downloads are decoupled from
    // installs). source_type/source_id/file_id preserve the origin metadata
    // for meta.ini (e.g. "nexus" + parent mod id + file id). name is the
    // display name from the Downloads tab (e.g. "SkyUI") and becomes the mod
    // folder name; empty falls back to the download id.
    void install_mod(const std::string& id, const std::string& zip_path,
                     const std::string& source_type = {},
                     const std::string& source_id = {}, int file_id = 0,
                     const std::string& name = {});

    // Download only (fetch stage): produces the archive in the instance
    // downloads dir, then emits download_complete. Resume-safe - a partial
    // file from a paused download is continued via HTTP Range.
    void download_mod(const std::string& id, const engine::NxmLink& link,
                      const std::string& game_id,
                      const std::string& mods_dir,
                      const std::string& meta_dir);

    // Download only, by pre-assembled URL (LoversLab and other no-API sites).
    // The provider fetches mod.download_url with the configured session
    // cookie. Same pause/progress/resume contract as download_mod.
    void download_mod_url(const std::string& id, const std::string& url,
                          const std::string& game_id,
                          const std::string& mods_dir,
                          const std::string& meta_dir);

    // Request a pause of an in-flight download (cooperative: the transfer
    // callback polls the flag and aborts, keeping the partial file).
    void pause_download(const std::string& id);

signals:
    void progress(const std::string& mod_id, int stage_index, const std::string& stage_name);
    void download_progress(const std::string& mod_id, int64_t bytes_downloaded, int64_t bytes_total, double speed_bytes_per_sec);
    // name is the real display name resolved by the provider (e.g. "SkyUI"),
    // or empty when no provider info was available.
    void download_complete(const std::string& mod_id, bool success,
                           const std::string& archive_path,
                           const std::string& name = {});
    void install_complete(const std::string& mod_id, bool success,
                          const std::string& message);
    void paused(const std::string& mod_id);
    void all_done();

private:
    std::unique_ptr<engine::Pipeline> pipeline_;
    engine::PipelineContext ctx_;
    // Fetch-only pipeline used by download_mod (built in set_context).
    std::unique_ptr<engine::Pipeline> fetch_pipeline_;
    // Per-download pause flags, keyed by the download id.
    std::unordered_map<std::string, std::atomic_bool> cancel_flags_;

    // Shared body of download_mod / download_mod_url: runs the fetch-only
    // pipeline with the given pre-assembled Mod and emits the completion
    // signals. The caller sets the provider-specific download fields
    // (download_source_type / download_nxm or download_url).
    void run_fetch(engine::Mod mod, const std::string& id,
                   const std::string& mods_dir, const std::string& meta_dir);
};

// Wrapper to run PipelineWorker in a thread
class PipelineThread : public QObject {
    Q_OBJECT
public:
    explicit PipelineThread(QObject* parent = nullptr);
    ~PipelineThread();

    PipelineWorker* worker() const { return worker_; }

    void start();
    void stop();

signals:
    void operation_started();
    void operation_finished();

private:
    QThread* thread_ = nullptr;
    PipelineWorker* worker_ = nullptr;
};

}  // namespace ui
