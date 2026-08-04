#pragma once

#include <filesystem>
#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

// Outcome of normalizing an extracted archive's root so it matches the game's
// data directory (MO2 InstallerQuick::getSimpleArchiveBase + the game's
// ModDataChecker). GMM's historical behavior stripped whatever single top-level
// folder an archive had - which treated a real data folder like "SKSE" as a
// throwaway wrapper. This mirrors MO2: only peel wrappers the data-dir check
// says are safe to peel.
struct StagingNormalizeResult {
    // True when a FOMOD (fomod/ModuleConfig.xml) was found at the root or
    // inside a single-dir wrapper. FomodStage owns those archives - the tree
    // must be left untouched.
    bool fomod = false;
    // True when the root already looked like the game's data dir (no peel).
    bool simple = false;
    // True when a lone "<dataFolderName>" wrapper around only data + readme
    // files was merged up into the root (MO2's isDataTextArchiveTopLayer).
    bool merged_data_dir = false;
    // Name of the first peeled single-dir wrapper ("" when nothing was peeled).
    // Callers may use it as a name hint - never for a data-named wrapper.
    std::string peeled_folder_hint;
};

// MO2-faithful archive root normalization. Repeats getSimpleArchiveBase's loop:
// if the root already looks like the game's data dir (a known data-dir folder
// or a known data file extension, case-insensitive) or is a DataText top layer,
// it's the base; else peel a single-dir wrapper and retry; else give up and
// leave the root as-is. data_folder_name is the game's data dir name
// (ctx.deploy_prefix, e.g. "Data"). FOMOD archives are detected first (via
// find_fomod_dir) and never reshaped. Returns what happened to the tree on
// disk (wrappers peeled / data dir merged).
[[nodiscard]] StagingNormalizeResult normalize_staging_root(
    const std::filesystem::path& staging_root,
    const std::string& data_folder_name);

class ExtractStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override { return "Extract"; }
    std::string description() const override {
        return "Unpacks the archive into the staging cache and reads metadata";
    }
    std::string condition() const override { return "Archive extracted to cache"; }
};

}  // namespace engine
