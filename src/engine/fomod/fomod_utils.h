#pragma once

// Qt-free helpers shared by the FOMOD engine modules (ported from FOMOD
// Plus's share/stringutil.h / share/xml/XmlHelper.h where applicable).

#include <algorithm>
#include <cctype>
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

}  // namespace engine
