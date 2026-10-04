#pragma once

#include "engine/deploy/interface.h"
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
class ProfileModel;
class OrderEncodingHook;
class FomodViewModel;
class ModDataCheckerFeature;

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
  bool backup            = false;  // keep a <name>_backup copy of the old folder
  std::string new_name;            // for Rename: the new mod folder name
};

// Result of the FOMOD install wizard. The engine runs the wizard (when one is
// wired up), applies the chosen options to the extracted staging dir, and
// passes the choices back so the mod folder can persist them for reinstall
// restore (MO2-style "restore previous choices").
struct FomodDecision {
  bool accept = false;          // true = install with the chosen options
  bool manual = false;          // true = skip option selection, install the
                                // archive contents as-is (FOMOD "Manual")
  std::string choices_json;     // FOMOD Plus fomod.json shape
  std::string mod_name;         // wizard-edited mod name ("" = keep suggested)
  bool ignore_missing = false;  // skip sources missing from the archive
};

// What the user chose when the archive-layout dialog asked where the game's
// data directory is. Reached only after the silent peel and the silent repair
// both found nothing (MO2's getSimpleArchiveBase returning nullptr, the one
// branch that drops an archive into InstallerManual), so neither field is a
// judgement about the content - it is the user pointing at a subtree.
struct LayoutDecision {
  // Subtree of the extracted content root to install as the mod's data
  // directory, as a path relative to that root. Empty keeps the content root
  // as-is, which is what accepting a "does not look valid" verdict means.
  std::filesystem::path data_root;
  // The user backed out. Nothing is installed and nothing is left behind.
  bool cancel = false;
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
  Instance *instance                 = nullptr;
  ConflictIndex *conflict_index      = nullptr;
  ProfileModel *profile              = nullptr;
  Deploy::Interface *deploy_strategy = nullptr;
  OrderEncodingHook *order_hook      = nullptr;
  std::filesystem::path game_dir;  // live game directory (for Overwrite capture)
  std::filesystem::path
      mods_dir;  // where mod folders live (meta.ini sits inside each folder)

  // Game-relative prefix for deployed mod files (e.g. "Data" for Skyrim, "mods"
  // for Isaac)
  std::string deploy_prefix = "Data";

  // Whether to include the mod ID as a subdirectory in the deploy target path.
  // Skyrim-style (files go directly into Data/) = false.
  // Isaac-style (mods go into mods/ModName/) = true.
  bool deploy_include_mod_id = false;

  // Per-game metadata format inside mod folders. MO2-style games default to
  // "meta.ini"; games whose engine reads XML metadata from mod folders
  // (Isaac) register the filename via the metadata_file hook.
  std::string metadata_file = "meta.ini";

  // The game's declared data allow-lists (MO2 IModDataChecker): a registered
  // mod_data_checker feature, else the mod_valid_dirs / mod_valid_exts hooks,
  // as resolved by data_checker_for() from the same declaration the mod
  // scanner reads. ExtractStage hands it to the staging-layout decision, so
  // "is this top-level content the game's data or a wrapper?" is answered by
  // what the game declares - never by folder names baked into the engine.
  // Null when the game declares nothing.
  std::shared_ptr<const ModDataCheckerFeature> data_checker;

  // When using OverlayFS deploy strategy, staging_dir holds the mod symlink
  // tree that gets layered over game_dir at launch. Empty = deploy directly to
  // game_dir.
  std::filesystem::path staging_dir;

  // When the install target mod folder already exists, this callback asks the
  // user how to proceed (Merge/Replace/Rename/Cancel). Invoked on the pipeline
  // thread with the existing mod folder name; must be thread-safe (the UI
  // wires it to marshal the dialog onto the main thread). Unset = silently
  // replace (headless/CLI default, matching the pre-dialog behavior).
  std::function<OverwriteDecision(const std::string &mod_name)> overwrite_query_cb;

  // Non-FOMOD install name confirmation (MO2's SimpleInstallDialog). Invoked
  // on the pipeline thread with the suggested mod name (typically the Nexus
  // display name, falling back to the archive stem) and the archive filename;
  // returns the confirmed name, or nullopt when the user canceled the
  // install. Unset (headless/CLI): the suggested name is used as-is. FOMOD
  // archives are skipped - their wizard owns the name.
  std::function<std::optional<std::string>(const std::string &suggested_name,
                                           const std::string &archive_filename)>
      name_query_cb;

  // Asks for the decryption password of an encrypted archive. Invoked on the
  // pipeline thread with the archive's filename when a reader needs a key;
  // return true and write the password to `passphrase` to continue, or false
  // when the user dismissed the prompt, which cancels the install (it is not a
  // failure, and no reason is reported). Called again after a rejected password
  // so a mistyped one can be retyped, a few times at most. Must be thread-safe
  // (the UI wires it to marshal the dialog onto the main thread). Unset
  // (headless/CLI): an encrypted archive fails with a reason naming that.
  // The password is passed to the readers and nowhere else - not logged, not
  // stored, not persisted.
  std::function<bool(const std::string &archive_name, std::string &passphrase)>
      passphrase_query_cb;

  // Asks which subtree of an extracted archive maps onto the game's data
  // directory. Called on the pipeline thread ONLY when the silent peel and the
  // silent repair both found nothing, so an archive that installs fine on its
  // own never reaches it. Invoked with the extracted content root (the staging
  // dir) and the game's deploy prefix, which is the name the dialog shows in
  // its "<prefix>" pseudo-root and in "Set as <prefix> directory". Must be
  // thread-safe (the UI wires it to marshal the dialog onto the main thread).
  // Unset (headless/CLI): the content installs as-is and nobody is asked.
  std::function<LayoutDecision(const std::filesystem::path &content_root,
                               const std::string &data_prefix)>
      layout_query_cb;

  // True once FomodStage recognizes the archive as a FOMOD (a fomod/
  // ModuleConfig.xml exists), even when the wizard was skipped via "Manual".
  // InstallStage uses it to skip the non-FOMOD name dialog.
  bool fomod_detected = false;

  // The final mod folder name produced by InstallStage (after any
  // name-confirmation / Rename). Empty when the install never reached the
  // copy step. PipelineWorker forwards it via install_complete so the UI can
  // add just that one row instead of rescanning the whole mods dir.
  std::string installed_mod_folder;

  // The archive filename of the mod folder a Replace just deleted, read off
  // that folder's meta.ini before the delete took it away. Empty unless this
  // install answered the Mod Exists dialog with Replace, and only ever set
  // once: Merge, Rename and Cancel leave it alone. PipelineWorker forwards it
  // so the UI can stop claiming that archive is still installed - MO2 emits
  // modReplaced for the same reason
  // (references/modorganizer/src/installationmanager.cpp:418-424, :320).
  std::string replaced_archive;

  // Set by an interactive stage when the user aborts (FOMOD wizard Cancel,
  // overwrite dialog Cancel). Pipeline::run stops and reports Canceled, which
  // the caller must not treat as a failure.
  bool canceled = false;

  // Why the pipeline failed, written by the stage that failed (e.g. the
  // archive extractor's own diagnostic). Empty when the run succeeded, was
  // canceled, or no failing stage produced one - callers must fall back to a
  // generic message rather than invent a reason. Cleared at the start of every
  // run, like the other per-install fields above, so one mod's failure can
  // never be reported against the next.
  std::string error_message;

  // When the extracted archive is a FOMOD (fomod/ModuleConfig.xml), this
  // callback opens the installer wizard. Invoked on the pipeline thread with
  // the FomodViewModel already built (so the wizard drives the same view
  // model the engine installs from), the extracted content root, the
  // suggested mod name, and any previously persisted choices (from a
  // reinstall); must be thread-safe (the UI wires it to marshal the dialog
  // onto the main thread). Unset (headless/CLI): prior choices are restored
  // if available, otherwise the install aborts with an error - a FOMOD
  // install must never silently guess.
  std::function<FomodDecision(const std::shared_ptr<FomodViewModel> &view_model,
                              const std::filesystem::path &content_root,
                              const std::string &suggested_name,
                              const std::string &previous_choices)>
      fomod_query_cb;

  // Choices JSON produced by the FOMOD stage; InstallStage persists it as
  // [fomod] choices in the mod folder's meta.ini for reinstall restore.
  std::string fomod_choices_json;

  // Download progress callback (bytes downloaded, total bytes, speed in
  // bytes/sec)
  std::function<void(int64_t downloaded, int64_t total, double speed)> on_progress;

  // Resolved download metadata. FetchStage fires this right after the
  // provider's resolve_download_info - before any bytes flow - so the UI can
  // replace its placeholder row name with the real mod/file name immediately
  // instead of waiting for download_complete. Invoked on the pipeline thread.
  // Both values may be empty when the provider could not resolve anything.
  std::function<void(const std::string &archive_name, const std::string &display_name)>
      on_download_meta;

  // Install-stage progress (extract/copy): current percent 0-100, or -1 when
  // the stage cannot estimate progress (indeterminate bar), plus a short
  // human status line ("Extracting SkyUI.zip…", "Installing to SkyUI…").
  // Invoked on the pipeline thread; the UI marshals it to its progress
  // dialog. Unset (headless/CLI default) = no reporting.
  std::function<void(int percent, const std::string &status)> on_stage_progress;

  // Download pause/resume control. `should_abort` is polled by the download
  // provider's transfer callback; returning true aborts the fetch and keeps
  // the partial file on disk (so a later run can resume it via Range).
  std::function<bool()> should_abort;
  bool download_paused = false;

  // Fetch stages set this to the size of an existing partial file so the
  // provider can resume from that offset instead of re-downloading.
  int64_t download_resume_from = 0;

  // Lower the CPU priority of archive extraction so large mod archives don't
  // monopolize the system. The engine is Qt-free and never reads QSettings
  // directly, so the UI layer reads the "extraction/low_priority" setting on
  // each install and supplies it here; ExtractStage forwards it to
  // ArchiveExtractor::extract. Defaults to true (most users want this).
  bool low_priority_extraction = true;
};

class Pipeline {
public:
  void set_context(PipelineContext ctx);
  void add_stage(std::unique_ptr<Stage> stage);
  PipelineResult run(Mod &mod);
  PipelineContext &ctx() { return ctx_; }

  // TraceRecorder flow id this pipeline reports under (default "install").
  void set_flow_id(std::string flow_id) { flow_id_ = std::move(flow_id); }
  const std::string &flow_id() const { return flow_id_; }

private:
  PipelineContext ctx_;
  std::vector<std::unique_ptr<Stage>> stages_;
  std::string flow_id_ = "install";
};

}  // namespace engine
