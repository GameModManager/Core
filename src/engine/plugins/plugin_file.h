#pragma once

// Qt-free predicate shared by every component that must decide what the game
// treats as a plugin file (.esm/.esp/.esl/.esh/.esu). Lives in its own header
// so lightweight consumers (the saves missing-assets resolver) can agree with
// the PluginDatabase without pulling in its whole dependency tree.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace engine {

inline bool is_plugin_file(const std::filesystem::path& p) {
    std::string ext;
    if (p.has_extension()) ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".esm" || ext == ".esp" || ext == ".esl" || ext == ".esh" ||
           ext == ".esu";
}

}  // namespace engine
