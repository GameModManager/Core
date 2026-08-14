#include "engine/registry/game_features/mod_data_checker.h"

#include "engine/util/fs_utils.h"

#include <unordered_set>

namespace engine {

const std::unordered_set<std::string>& ModDataChecker::folder_names() {
    static const std::unordered_set<std::string> s = {
        "fonts", "interface", "menus", "meshes", "music", "scripts", "shaders",
        "sound", "strings", "textures", "trees", "video", "facegen", "materials",
        "skse", "obse", "mwse", "nvse", "fose", "f4se", "distantlod", "asi",
        "skyproc patchers", "tools", "mcm", "icons", "bookart", "distantland",
        "mits", "splash", "dllplugins", "calientetools", "netscriptframework",
        "shadersfx", "source",
    };
    return s;
}

const std::unordered_set<std::string>& ModDataChecker::file_extensions() {
    static const std::unordered_set<std::string> s = {
        "esp", "esm", "esl", "bsa", "ba2", "modgroups", "ini",
    };
    return s;
}

bool ModDataChecker::data_looks_valid(const std::shared_ptr<const FileTree>& tree) {
    if (!tree) return false;
    const auto& folders = folder_names();
    const auto& extensions = file_extensions();
    for (const auto& entry : *tree) {
        if (entry->is_dir()) {
            if (folders.count(toLower(entry->name())) > 0) return true;
        } else if (extensions.count(toLower(entry->suffix())) > 0) {
            return true;
        }
    }
    return false;
}

}  // namespace engine
