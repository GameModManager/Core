#include "engine/mod/filetree/staging_layout.h"

#include "engine/util/fs_utils.h"
#include "engine/mod/fomod/fomod_utils.h"
#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_features/mod_data_checker.h"

#include <unordered_set>

namespace engine {

namespace {

  // MO2 InstallerQuick::isDataTextArchiveTopLayer (installerquick.cpp:66-91): a
  // "DataText" archive has exactly one folder named like data_folder_name plus
  // one or more "useless" files (text/pdf/md/images) and nothing else.
  bool is_data_text_top_layer(const std::shared_ptr<const FileTree> &tree,
                              const std::string &data_folder_name) {
    static const std::unordered_set<std::string> junk = {
        "txt", "pdf", "md", "jpg", "jpeg", "png", "bmp",
    };
    if (data_folder_name.empty())
      return false;
    bool data_found = false;
    bool txt_found  = false;
    for (const auto &entry : *tree) {
      if (entry->is_dir()) {
        if (data_found || !name_equals(entry->name(), data_folder_name,
                                       NameCompare::CaseInsensitive)) {
          return false;
        }
        data_found = true;
      } else {
        if (!junk.count(toLower(entry->suffix())))
          return false;
        txt_found = true;
      }
    }
    return data_found && txt_found;
  }

  // Recursive core of analyze_staging_layout: exactly getSimpleArchiveBase's
  // loop, with GMM's FOMOD guard first. Descending a single-dir wrapper records
  // it in peel_chain (the first one becomes the name hint); the bottom of a
  // wrapper chain that never matched data raises needs_review and keeps the
  // chain so the caller can still peel - GMM's historical, more lenient
  // behavior vs MO2's nullptr.
  void analyze_rec(const std::shared_ptr<const FileTree> &tree,
                   const std::string &data_folder_name,
                   const std::shared_ptr<const ModDataCheckerFeature> &checker,
                   StagingNormalizeResult &result) {
    if (find_fomod_dir(tree)) {
      result.fomod = true;
      return;
    }
    // The declaring game's own allow-lists decide this; the engine's static
    // Bethesda set is only the fallback for a game that declares none.
    if (checker ? checker->data_looks_valid(tree)
                : ModDataChecker::data_looks_valid(tree)) {
      result.simple = true;
      return;
    }
    if (is_data_text_top_layer(tree, data_folder_name)) {
      result.simple          = true;
      result.merged_data_dir = true;
      return;
    }
    if (tree->size() == 1) {
      auto only = tree->at(0);
      if (only->is_dir()) {
        if (result.peeled_folder_hint.empty()) {
          result.peeled_folder_hint = only->name();
        }
        result.peel_chain.push_back(only->name());
        analyze_rec(only->as_tree(), data_folder_name, checker, result);
        return;
      }
    }
    // MO2 returns nullptr here (getSimpleArchiveBase, installerquick.cpp:109):
    // no level in this chain is the game's data dir, none is a DataText top
    // layer, and there is no single-dir wrapper left to descend through. That
    // is the one branch that drops an archive into the manual layout dialog, so
    // it has to be said out loud rather than falling through silently.
    result.needs_review = true;
  }

}  // namespace

StagingNormalizeResult
analyze_staging_layout(const std::shared_ptr<const FileTree> &tree,
                       const std::string &data_folder_name,
                       std::shared_ptr<const ModDataCheckerFeature> checker) {
  StagingNormalizeResult result;
  if (tree)
    analyze_rec(tree, data_folder_name, checker, result);
  return result;
}

StagingNormalizeResult
analyze_staging_root(const std::filesystem::path &staging_root,
                     const std::string &data_folder_name,
                     std::shared_ptr<const ModDataCheckerFeature> checker) {
  // Mirror the disk exactly: a meta.ini at the staging root is a real entry
  // (ignore_meta_ini is for mod-folder roots, not extracted archives).
  auto tree =
      FileTree::make_tree_from_directory(staging_root, NameCompare::CaseInsensitive,
                                         /*ignore_meta_ini=*/false);
  if (!tree)
    return {};
  return analyze_staging_layout(tree, data_folder_name, std::move(checker));
}

LayoutVerdict layout_verdict(const std::filesystem::path &content_root,
                             std::shared_ptr<const ModDataCheckerFeature> checker) {
  // No declaration means nothing to check against, which is a different answer
  // from "checked and it does not match" - the dialog says so instead of
  // guessing.
  if (!checker)
    return LayoutVerdict::Unknown;
  // Mirror analyze_staging_root: an extracted archive's meta.ini is a real
  // entry, ignore_meta_ini is for mod-folder roots.
  auto tree = FileTree::make_tree_from_directory(
      content_root, NameCompare::CaseInsensitive, /*ignore_meta_ini=*/false);
  if (!tree)
    return LayoutVerdict::Unknown;
  return checker->data_looks_valid(tree) ? LayoutVerdict::Valid
                                         : LayoutVerdict::Invalid;
}

StagingNormalizeResult
normalize_staging_root(const std::filesystem::path &staging_root,
                       const std::string &data_folder_name,
                       std::shared_ptr<const ModDataCheckerFeature> checker,
                       const std::filesystem::path &designated) {
  StagingNormalizeResult result;
  if (data_folder_name.empty())
    return result;

  // The subtree the user designated is the root of the peel, so the levels
  // leading to it and the wrappers inside it are one chain - recorded below and
  // applied by the single rename loop.
  const auto start = designated.empty() ? staging_root : staging_root / designated;
  result           = analyze_staging_root(start, data_folder_name, checker);
  if (result.fomod)
    return result;

  std::vector<std::string> chain;
  for (const auto &level : designated)
    chain.push_back(level.string());
  chain.insert(chain.end(), result.peel_chain.begin(), result.peel_chain.end());
  result.peel_chain = std::move(chain);
  // The designated level is the mod's own folder, so it names the mod when its
  // own wrappers gave no hint.
  if (result.peeled_folder_hint.empty() && !designated.empty())
    result.peeled_folder_hint = designated.filename().string();

  std::error_code ec;
  // Peel the recorded wrappers: move each wrapper's children up into the
  // root, then drop the wrapper (MO2's install() detach+merge, on disk).
  for (const auto &wrapper_name : result.peel_chain) {
    const auto wrapper = staging_root / wrapper_name;
    for (auto it = std::filesystem::directory_iterator(wrapper, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (ec)
        break;
      std::filesystem::rename(it->path(), staging_root / it->path().filename(), ec);
      if (ec)
        break;
    }
    std::filesystem::remove_all(wrapper, ec);
  }

  // Merge a DataText data dir (<root>/<data_folder_name>/* -> <root>).
  if (result.merged_data_dir) {
    for (auto it = std::filesystem::directory_iterator(staging_root, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (ec)
        break;
      if (it->is_directory(ec) && name_matches_ci(it->path(), data_folder_name)) {
        const auto data_dir = it->path();
        for (auto c = std::filesystem::directory_iterator(data_dir, ec);
             c != std::filesystem::directory_iterator(); c.increment(ec)) {
          if (ec)
            break;
          std::filesystem::rename(c->path(), staging_root / c->path().filename(), ec);
          if (ec)
            break;
        }
        std::filesystem::remove_all(data_dir, ec);
        break;
      }
    }
  }
  return result;
}

}  // namespace engine
