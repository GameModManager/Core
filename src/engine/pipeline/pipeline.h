#pragma once

#include "engine/pipeline/stage.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace engine {

class Instance;
class ConflictIndex;
class Profile;
class DeploymentStrategy;
class OrderEncodingHook;
class FomodViewModel;

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

// Result of the FOMOD install wizard. The engine runs the wizard (when one is
// wired up), applies the chosen options to the extracted staging dir, and
// passes the choices back so the mod folder can persist them for reinstall
// restore (MO2-style "restore previous choices").
struct FomodDecision {
    bool accept = false;        // true = install with the chosen options
    bool manual = false;        // true = skip option selection, install the
                                // archive contents as-is (FOMOD "Manual")
    std::string choices_json;   // FOMOD Plus fomod.json shape
    std::string mod_name;       // wizard-edited mod name ("" = keep suggested)
    bool ignore_missing = false; // skip sources missing from the archive
};

// Result of a pipeline run. Canceled is distinct from Failed: the user aborted
// an interactive stage (FOMOD wizard, overwrite dialog), so callers must NOT
// mark the download as failed - it keeps whatever state it had.
enum class PipelineResult {
    Success,
    Failed,
    Canceled,
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

    // Non-FOMOD install name confirmation (MO2's SimpleInstallDialog). Invoked
    // on the pipeline thread with the suggested mod name (typically the Nexus
    // display name, falling back to the archive stem) and the archive filename;
    // returns the confirmed name, or nullopt when the user canceled the
    // install. Unset (headless/CLI): the suggested name is used as-is. FOMOD
    // archives are skipped - their wizard owns the name.
    std::function<std::optional<std::string>(const std::string& suggested_name,
                                             const std::string& archive_filename)>
        name_query_cb;

    // True once FomodStage recognizes the archive as a FOMOD (a fomod/
    // ModuleConfig.xml exists), even when the wizard was skipped via "Manual".
    // InstallStage uses it to skip the non-FOMOD name dialog.
    bool fomod_detected = false;

    // The final mod folder name produced by InstallStage (after any
    // name-confirmation / Rename). Empty when the install never reached the
    // copy step. PipelineWorker forwards it via install_complete so the UI can
    // add just that one row instead of rescanning the whole mods dir.
    std::string installed_mod_folder;

    // Set by an interactive stage when the user aborts (FOMOD wizard Cancel,
    // overwrite dialog Cancel). Pipeline::run stops and reports Canceled, which
    // the caller must not treat as a failure.
    bool canceled = false;

    // When the extracted archive is a FOMOD (fomod/ModuleConfig.xml), this
    // callback opens the installer wizard. Invoked on the pipeline thread with
    // the FomodViewModel already built (so the wizard drives the same view
    // model the engine installs from), the extracted content root, the
    // suggested mod name, and any previously persisted choices (from a
    // reinstall); must be thread-safe (the UI wires it to marshal the dialog
    // onto the main thread). Unset (headless/CLI): prior choices are restored
    // if available, otherwise the install aborts with an error - a FOMOD
    // install must never silently guess.
    std::function<FomodDecision(const std::shared_ptr<FomodViewModel>& view_model,
                                const std::filesystem::path& content_root,
                                const std::string& suggested_name,
                                const std::string& previous_choices)>
        fomod_query_cb;

    // Choices JSON produced by the FOMOD stage; InstallStage persists it as
    // [fomod] choices in the mod folder's meta.ini for reinstall restore.
    std::string fomod_choices_json;

    // Download progress callback (bytes downloaded, total bytes, speed in bytes/sec)
    std::function<void(int64_t downloaded, int64_t total, double speed)> on_progress;

    // Install-stage progress (extract/copy): current percent 0-100, or -1 when
    // the stage cannot estimate progress (indeterminate bar), plus a short
    // human status line ("Extracting SkyUI.zip…", "Installing to SkyUI…").
    // Invoked on the pipeline thread; the UI marshals it to its progress
    // dialog. Unset (headless/CLI default) = no reporting.
    std::function<void(int percent, const std::string& status)> on_stage_progress;

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
    PipelineResult run(Mod& mod);
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
