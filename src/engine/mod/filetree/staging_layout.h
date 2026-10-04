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

class ModDataCheckerFeature;

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
  // True when the loop gave up: no level in the chain looked like the game's
  // data directory, no level was a DataText top layer, and there was no
  // single-dir wrapper left to descend through. That is MO2's
  // getSimpleArchiveBase returning nullptr, the one branch that drops an
  // archive into the manual layout dialog. It says the engine cannot vouch for
  // the root, NOT that the content is wrong.
  bool needs_review = false;
  // Name of the first peeled single-dir wrapper ("" when nothing was peeled).
  // Callers may use it as a name hint - never for a data-named wrapper.
  std::string peeled_folder_hint;
  // Wrappers to peel, in peel order (top-level first). The tree analysis
  // records the whole chain so the physical driver can reproduce the peel
  // even when the bottom level was not data-looking.
  std::vector<std::string> peel_chain;
};

// Pure decision. Repeats getSimpleArchiveBase's loop on a tree: if the root
// already looks like the game's data dir (the game's own ModDataChecker) or is
// a DataText top layer, it's the base; else descend a single-dir wrapper and
// retry; else give up. A FOMOD tree is detected first (tree find_fomod_dir) and
// never reshaped. Returns the verdict; the caller applies peel_chain / merges
// data_dir physically. data_folder_name is the game's data dir name (e.g.
// "Data"). checker is the game's declared data allow-lists (a registered
// mod_data_checker feature, else the mod_valid_dirs / mod_valid_exts hooks);
// null falls back to the engine's own Bethesda set, so a game that declares
// nothing behaves as it always has.
[[nodiscard]] StagingNormalizeResult
analyze_staging_layout(const std::shared_ptr<const FileTree> &tree,
                       const std::string &data_folder_name,
                       std::shared_ptr<const ModDataCheckerFeature> checker = nullptr);

// Which of the manual layout dialog's three states a subtree is in. MO2 spells
// the same three out in InstallDialog::testForProblem (installdialog.cpp:86-93)
// and its rendering (installdialog.cpp:95-122): the subtree matches the game's
// declaration, it does not, or the game declared nothing to match against.
enum class LayoutVerdict {
  Valid,
  Invalid,
  Unknown,
};

// The verdict for ONE subtree - the node the user has designated as the data
// directory and nothing below it, which is the point: the answer has to describe
// the tree that will be installed, so it must not descend through a wrapper
// looking for a better level. checker is the game's declaration; null means the
// game registered none, which is Unknown rather than Invalid - there is nothing
// to judge against, so nothing is claimed.
[[nodiscard]] LayoutVerdict
layout_verdict(const std::filesystem::path &content_root,
               std::shared_ptr<const ModDataCheckerFeature> checker);

// Same decision, on an extracted archive's staging root: builds the tree the
// way normalize_staging_root does and answers without touching disk. Callers
// that need the verdict but not the peel (ExtractStage, which only asks
// whether the root is already game data) use this.
[[nodiscard]] StagingNormalizeResult
analyze_staging_root(const std::filesystem::path &staging_root,
                     const std::string &data_folder_name,
                     std::shared_ptr<const ModDataCheckerFeature> checker = nullptr);

// MO2-faithful archive root normalization ON DISK: analyze_staging_layout over
// a DirectoryFileTree, then applies the verdict (peels the recorded wrappers,
// merges a DataText data dir up into the root). FOMOD archives are never
// reshaped. Returns what happened to the tree on disk.
//
// `designated` is the subtree the user picked as the data directory in the
// manual layout dialog, as a path relative to staging_root. It becomes the root
// of the peel, so the levels leading to it and its own wrappers are one chain
// and the single loop below applies all of them - the automatic path and the
// user-driven path stay one implementation. Empty is the automatic path.
[[nodiscard]] StagingNormalizeResult
normalize_staging_root(const std::filesystem::path &staging_root,
                       const std::string &data_folder_name,
                       std::shared_ptr<const ModDataCheckerFeature> checker = nullptr,
                       const std::filesystem::path &designated              = {});

}  // namespace engine
