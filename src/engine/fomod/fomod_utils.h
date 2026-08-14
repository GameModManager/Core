#pragma once

// Qt-free helpers shared by the FOMOD engine modules (ported from FOMOD
// Plus's share/stringutil.h / share/xml/XmlHelper.h where applicable).
//
// Separator/case handling lives in engine/fs_utils.h (the single canonical
// Windows-native resolver, engine::resolve_path) - keep no copies here.

#include "engine/util/fs_utils.h"
#include "engine/filetree/file_tree.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// FOMOD archive layout constants (FOMOD Plus share/stringutil.h).
namespace fomod_files {
inline constexpr std::string_view FOMOD_DIR = "fomod";
inline constexpr std::string_view INFO_XML = "info.xml";
inline constexpr std::string_view MODULE_CONFIG = "ModuleConfig.xml";

inline constexpr std::string_view TYPE_REQUIRED = "Required";
inline constexpr std::string_view TYPE_OPTIONAL = "Optional";
inline constexpr std::string_view TYPE_RECOMMENDED = "Recommended";
inline constexpr std::string_view TYPE_NOT_USABLE = "NotUsable";
inline constexpr std::string_view TYPE_COULD_BE_USABLE = "CouldBeUsable";
}  // namespace fomod_files

// FOMOD Plus findFomodDirectory: a directory named "fomod" (any casing) wins;
// else if the current directory has exactly one entry and it is a directory,
// descend into it. Returns the fomod dir path (the content root is its parent)
// or nullopt when the tree holds no FOMOD installer.
[[nodiscard]] inline std::optional<std::filesystem::path> find_fomod_dir(
    const std::filesystem::path& dir)
{
    std::error_code ec;
    std::optional<std::filesystem::path> singleChild;
    int entryCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            return std::nullopt;
        }
        if (entry.is_directory() && name_matches_ci(entry.path(), std::string(fomod_files::FOMOD_DIR))) {
            return entry.path();
        }
        singleChild = entry.path();
        ++entryCount;
    }
    if (entryCount == 1 && singleChild && std::filesystem::is_directory(*singleChild, ec)) {
        return find_fomod_dir(*singleChild);
    }
    return std::nullopt;
}

// Tree variant of find_fomod_dir: the same descent rule (a "fomod" directory
// of any casing wins; else descend the lone single-dir child) over a lazy file
// tree, so the same FOMOD detection runs on in-memory/archive trees. Returns
// the fomod directory node (the content root is its parent) or null.
[[nodiscard]] inline std::shared_ptr<const FileTree> find_fomod_dir(
    const std::shared_ptr<const FileTree>& tree)
{
    if (!tree) return nullptr;
    for (const auto& entry : *tree) {
        if (entry->is_dir() &&
            name_equals(entry->name(), std::string(fomod_files::FOMOD_DIR),
                        NameCompare::CaseInsensitive)) {
            return entry->as_tree();
        }
    }
    if (tree->size() == 1) {
        auto only = tree->at(0);
        if (only->is_dir()) return find_fomod_dir(only->as_tree());
    }
    return nullptr;
}

inline std::string& ltrim(std::string& s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}

inline std::string& rtrim(std::string& s)
{
    s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
        s.end());
    return s;
}

inline std::string& trim(std::string& s)
{
    ltrim(s);
    rtrim(s);
    return s;
}

inline void trim(const std::vector<std::string>& strings)
{
    for (auto s : strings) {
        trim(s);
    }
}

}  // namespace engine
