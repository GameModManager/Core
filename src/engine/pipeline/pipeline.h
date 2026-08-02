#pragma once

#include "engine/pipeline/stage.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace engine {

class Instance;
class ConflictIndex;
class Profile;
class DeploymentStrategy;
class OrderEncodingHook;

// How an install should handle a mod folder that already exists. Mirrors
// MO2's QueryOverwriteDialog actions (queryoverwritedialog.h).
enum class OverwriteAction {
    Merge,    // add files into the existing folder, overwriting on conflict
    Replace,  // delete the existing folder and install fresh
    Rename,   // install under a new folder name (decision.new_name)
    Cancel,   // abort the install
};

struct OverwriteDecision {
    OverwriteAction action = OverwriteAction::Cancel;
    bool backup = false;       // keep a <name>_backup copy of the old folder
    std::string new_name;      // for Rename: the new mod folder name
};

struct PipelineContext {
    Instance* instance = nullptr;
    ConflictIndex* conflict_index = nullptr;
    Profile* profile = nullptr;
    DeploymentStrategy* deploy_strategy = nullptr;
    OrderEncodingHook* order_hook = nullptr;
    std::filesystem::path game_dir;  // live game directory (for Overwrite capture)
    std::filesystem::path meta_dir;  // instance meta/ directory
    std::filesystem::path mods_dir;  // where mod folders live

    // Game-relative prefix for deployed mod files (e.g. "Data" for Skyrim, "mods" for Isaac)
    std::string deploy_prefix = "Data";

    // Whether to include the mod ID as a subdirectory in the deploy target path.
    // Skyrim-style (files go directly into Data/) = false.
    // Isaac-style (mods go into mods/ModName/) = true.
    bool deploy_include_mod_id = false;

    // Per-game metadata format inside mod folders. MO2-style games default to
    // "meta.ini"; games whose engine reads XML metadata from mod folders
    // (Isaac) register the filename via the metadata_file hook.
    std::string metadata_file = "meta.ini";

    // When using OverlayFS deploy strategy, staging_dir holds the mod symlink tree
    // that gets layered over game_dir at launch. Empty = deploy directly to game_dir.
    std::filesystem::path staging_dir;

    // When the install target mod folder already exists, this callback asks the
    // user how to proceed (Merge/Replace/Rename/Cancel). Invoked on the pipeline
    // thread with the existing mod folder name; must be thread-safe (the UI
    // wires it to marshal the dialog onto the main thread). Unset = silently
    // replace (headless/CLI default, matching the pre-dialog behavior).
    std::function<OverwriteDecision(const std::string& mod_name)> overwrite_query_cb;

    // Download progress callback (bytes downloaded, total bytes, speed in bytes/sec)
    std::function<void(int64_t downloaded, int64_t total, double speed)> on_progress;

    // Download pause/resume control. `should_abort` is polled by the download
    // provider's transfer callback; returning true aborts the fetch and keeps
    // the partial file on disk (so a later run can resume it via Range).
    std::function<bool()> should_abort;
    bool download_paused = false;

    // Fetch stages set this to the size of an existing partial file so the
    // provider can resume from that offset instead of re-downloading.
    int64_t download_resume_from = 0;
};

class Pipeline {
public:
    void set_context(PipelineContext ctx);
    void add_stage(std::unique_ptr<Stage> stage);
    bool run(Mod& mod);
    PipelineContext& ctx() { return ctx_; }

    // TraceRecorder flow id this pipeline reports under (default "install").
    void set_flow_id(std::string flow_id) { flow_id_ = std::move(flow_id); }
    const std::string& flow_id() const { return flow_id_; }

private:
    PipelineContext ctx_;
    std::vector<std::unique_ptr<Stage>> stages_;
    std::string flow_id_ = "install";
};

}  // namespace engine
