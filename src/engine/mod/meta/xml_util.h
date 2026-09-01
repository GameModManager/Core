#pragma once

#include <string>

namespace engine {

// Simple XML tag extraction without an XML library dependency.
// Returns trimmed content between <tag> and </tag>, or empty if not found.
inline std::string xml_find_tag(const std::string &xml,
                                const std::string &tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    auto pos = xml.find(open);
    if (pos == std::string::npos) return {};
    pos += open.size();
    auto end = xml.find(close, pos);
    if (end == std::string::npos) return {};
    auto content = xml.substr(pos, end - pos);
    auto first = content.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return {};
    auto last = content.find_last_not_of(" \t\n\r");
    return content.substr(first, last - first + 1);
}

}  // namespace engine
