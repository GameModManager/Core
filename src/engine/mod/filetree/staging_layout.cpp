#include "engine/mod/filetree/staging_layout.h"

#include "engine/core/util/fs_utils.h"
#include "engine/mod/fomod/fomod_utils.h"
#include "engine/game/registry/game_features/mod_data_checker.h"

#include <unordered_set>

namespace engine {

namespace {

// MO2 InstallerQuick::isDataTextArchiveTopLayer (installerquick.cpp:66-91): a
// "DataText" archive has exactly one folder named like data_folder_name plus
// one or more "useless" files (text/pdf/md/images) and nothing else.
bool is_data_text_top_layer(const std::shared_ptr<const FileTree>& tree,
                            const std::string& data_folder_name) {
    static const std::unordered_set<std::string> junk = {
        "txt", "pdf", "md", "jpg", "jpeg", "png", "bmp",
    };
    if (data_folder_name.empty()) return false;
    bool data_found = false;
    bool txt_found = false;
    for (const auto& entry : *tree) {
        if (entry->is_dir()) {
            if (data_found ||
                !name_equals(entry->name(), data_folder_name,
                             NameCompare::CaseInsensitive)) {
                return false;
            }
            data_found = true;
        } else {
            if (!junk.count(toLower(entry->suffix()))) return false;
            txt_found = true;
        }
    }
    return data_found && txt_found;
}

// Recursive core of analyze_staging_layout: exactly getSimpleArchiveBase's
// loop, with GMM's FOMOD guard first. Descending a single-dir wrapper records
// it in peel_chain (the first one becomes the name hint); the bottom of a
// wrapper chain that never matched data is plain (no flags, chain kept so the
// caller can still peel - GMM's historical, more lenient behavior vs MO2's
// nullptr).
void analyze_rec(const std::shared_ptr<const FileTree>& tree,
                 const std::string& data_folder_name,
                 StagingNormalizeResult& result) {
    if (find_fomod_dir(tree)) {
        result.fomod = true;
        return;
    }
    if (ModDataChecker::data_looks_valid(tree)) {
        result.simple = true;
        return;
    }
    if (is_data_text_top_layer(tree, data_folder_name)) {
        result.simple = true;
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
            analyze_rec(only->as_tree(), data_folder_name, result);
            return;
        }
    }
    // Not simple - leave the root as-is.
}

}  // namespace

StagingNormalizeResult analyze_staging_layout(
    const std::shared_ptr<const FileTree>& tree, const std::string& data_folder_name) {
    StagingNormalizeResult result;
    if (tree) analyze_rec(tree, data_folder_name, result);
    return result;
}

StagingNormalizeResult normalize_staging_root(
    const std::filesystem::path& staging_root, const std::string& data_folder_name) {
    StagingNormalizeResult result;
    if (data_folder_name.empty()) return result;

    // Mirror the disk exactly: a meta.ini at the staging root is a real entry
    // (ignore_meta_ini is for mod-folder roots, not extracted archives).
    auto tree = FileTree::make_tree_from_directory(staging_root,
                                                   NameCompare::CaseInsensitive,
                                                   /*ignore_meta_ini=*/false);
    if (!tree) return result;
    result = analyze_staging_layout(tree, data_folder_name);
    if (result.fomod) return result;

    std::error_code ec;
    // Peel the recorded wrappers: move each wrapper's children up into the
    // root, then drop the wrapper (MO2's install() detach+merge, on disk).
    for (const auto& wrapper_name : result.peel_chain) {
        const auto wrapper = staging_root / wrapper_name;
        for (auto it = std::filesystem::directory_iterator(wrapper, ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::filesystem::rename(it->path(), staging_root / it->path().filename(), ec);
            if (ec) break;
        }
        std::filesystem::remove_all(wrapper, ec);
    }

    // Merge a DataText data dir (<root>/<data_folder_name>/* -> <root>).
    if (result.merged_data_dir) {
        for (auto it = std::filesystem::directory_iterator(staging_root, ec);
             it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec) && name_matches_ci(it->path(), data_folder_name)) {
                const auto data_dir = it->path();
                for (auto c = std::filesystem::directory_iterator(data_dir, ec);
                     c != std::filesystem::directory_iterator(); c.increment(ec)) {
                    if (ec) break;
                    std::filesystem::rename(c->path(),
                                            staging_root / c->path().filename(), ec);
                    if (ec) break;
                }
                std::filesystem::remove_all(data_dir, ec);
                break;
            }
        }
    }
    return result;
}

}  // namespace engine
