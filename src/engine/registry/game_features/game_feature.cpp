#include "engine/registry/game_features/game_feature.h"

#include "engine/util/fs_utils.h"

namespace engine {

bool ModDataCheckerFeature::data_looks_valid(
    const std::shared_ptr<const FileTree>& tree) const {
    if (!tree) return false;
    for (const auto& entry : *tree) {
        if (entry->is_dir()) {
            for (const auto& d : folders_)
                if (toLower(entry->name()) == toLower(d)) return true;
        } else {
            for (const auto& e : extensions_)
                if (toLower(entry->suffix()) == toLower(e)) return true;
        }
    }
    return false;
}

}  // namespace engine
