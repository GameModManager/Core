#pragma once

// Qt-free helpers shared by the FOMOD engine modules (ported from FOMOD
// Plus's share/stringutil.h / share/xml/XmlHelper.h where applicable).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
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

inline std::string toLower(std::string str)
{
    std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return str;
}

// FOMOD paths are Windows-native: the spec's source/destination values use
// backslash separators, and authors build on a case-insensitive filesystem so
// any casing is possible. Translate '\' to '/' so the path resolves on Linux
// (where '\' is a legal filename character, not a separator).
inline std::string normalize_separators(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// Resolve a FOMOD-relative path against a root, matching each component
// case-insensitively (Windows-archive behaviour). Returns the real on-disk
// path (with the tree's actual casing) or an empty path when not found.
// Absolute paths and ".." traversal are rejected; when `escaped` is non-null
// it is set to true for rejection reasons that are not "file absent" (so
// callers can skip traversal attempts silently instead of reporting them
// missing).
inline std::filesystem::path resolve_path_ci(const std::filesystem::path& root,
    const std::string& relative, bool* escaped = nullptr)
{
    if (escaped) {
        *escaped = false;
    }
    const std::filesystem::path rel(normalize_separators(relative));
    if (rel.is_absolute() || rel.empty()) {
        if (escaped) {
            *escaped = true;
        }
        return {};
    }
    std::filesystem::path cur = root;
    for (const auto& part : rel) {
        const std::string comp = part.string();
        if (comp.empty() || comp == ".") {
            continue;
        }
        if (comp == "..") {
            if (escaped) {
                *escaped = true;
            }
            return {};
        }
        const std::string lowerComp = toLower(comp);
        std::error_code ec;
        std::filesystem::path match;
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
            if (ec) {
                return {};
            }
            const std::string name = entry.path().filename().string();
            if (name == comp || toLower(name) == lowerComp) {
                match = entry.path();
                found = true;
                break;
            }
        }
        if (!found) {
            return {};
        }
        cur = match;
    }
    return cur;
}

}  // namespace engine
