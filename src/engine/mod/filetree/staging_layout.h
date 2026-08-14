#pragma once

// Staging-root layout analysis (PLAN §19 P1.1). Ports MO2 InstallerQuick's
// getSimpleArchiveBase loop plus GMM's FOMOD guard so the wrapper-peel DECISION
// runs on a file tree - over a mod folder on disk or an in-memory archive tree
// alike - instead of on raw paths. The physical fs mutation (moving children
// up) lives only in normalize_staging_root; analyze_staging_layout never
// touches disk. Qt-free.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/mod/filetree/file_tree.h"

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
    // Wrappers to peel, in peel order (top-level first). The tree analysis
    // records the whole chain so the physical driver can reproduce the peel
    // even when the bottom level was not data-looking.
    std::vector<std::string> peel_chain;
};

// Pure decision. Repeats getSimpleArchiveBase's loop on a tree: if the root
// already looks like the game's data dir (ModDataChecker) or is a DataText top
// layer, it's the base; else descend a single-dir wrapper and retry; else give
// up. A FOMOD tree is detected first (tree find_fomod_dir) and never reshaped.
// Returns the verdict; the caller applies peel_chain / merges data_dir
// physically. data_folder_name is the game's data dir name (e.g. "Data").
[[nodiscard]] StagingNormalizeResult analyze_staging_layout(
    const std::shared_ptr<const FileTree>& tree,
    const std::string& data_folder_name);

// MO2-faithful archive root normalization ON DISK: analyze_staging_layout over
// a DirectoryFileTree, then applies the verdict (peels the recorded wrappers,
// merges a DataText data dir up into the root). FOMOD archives are never
// reshaped. Returns what happened to the tree on disk.
[[nodiscard]] StagingNormalizeResult normalize_staging_root(
    const std::filesystem::path& staging_root,
    const std::string& data_folder_name);

}  // namespace engine
