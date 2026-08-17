#include "engine/core/instance/toml_utils.h"

#include <fstream>
#include <sstream>

namespace engine {

namespace {

// Legacy files (pre-toml++ migration) stored the `executables` array as
// JSON-style inline tables: {"path":"SkyrimSE.exe","env":[...]}. TOML inline
// tables require `key = value` (bare keys), so a strict parse rejects them.
// This bounded repair converts JSON-style inline tables to TOML syntax:
//
//   {"path":"SkyrimSE.exe"}  ->  { path = "SkyrimSE.exe" }
//
// It only runs when a strict parse fails, and only rewrites the `"key":`
// separator pattern that is invalid TOML. Quoted strings are copied verbatim
// (escapes included); a closing quote followed by ':' becomes the TOML key
// separator. In valid TOML a quoted string is never followed by ':', so the
// transform is a no-op on well-formed files.
std::string repair_legacy_json_inline_tables(const std::string& content) {
    std::string out;
    out.reserve(content.size());
    const size_t n = content.size();
    size_t i = 0;
    while (i < n) {
        const char c = content[i];
        if (c != '"') {
            out += c;
            ++i;
            continue;
        }
        // Copy the quoted string verbatim (handling backslash escapes).
        out += c;
        ++i;
        while (i < n) {
            out += content[i];
            if (content[i] == '\\' && i + 1 < n) {
                out += content[i + 1];
                i += 2;
                continue;
            }
            if (content[i] == '"') {
                ++i;
                break;
            }
            ++i;
        }
        // After a closing quote, skip whitespace; a following ':' is the
        // JSON key separator -> TOML's ` = `.
        size_t j = i;
        while (j < n && (content[j] == ' ' || content[j] == '\t')) ++j;
        if (j < n && content[j] == ':') {
            out += " =";
            i = j + 1;
        }
    }
    return out;
}

}  // namespace

std::optional<toml::table> parse_instance_toml_content(
    const std::string& content) {
    try {
        return toml::parse(content);
    } catch (const toml::parse_error&) {
        // Legacy JSON-style inline tables (pre-toml++ migration).
        try {
            return toml::parse(repair_legacy_json_inline_tables(content));
        } catch (const toml::parse_error&) {
            return std::nullopt;
        }
    }
}

std::optional<toml::table> parse_instance_toml(
    const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return parse_instance_toml_content(content);
}

std::string serialize_instance_toml(const toml::table& tbl) {
    std::ostringstream ss;
    ss << toml::toml_formatter(tbl);
    ss << '\n';
    return ss.str();
}

}  // namespace engine