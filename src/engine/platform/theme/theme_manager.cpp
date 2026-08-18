#include "engine/platform/theme/theme_manager.h"
#include "engine/core/log/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace engine {

namespace {

// Extract the string value of `key` from a flat JSON file such as
// { "base_style": "Fusion" }. Returns an empty string when the key is
// missing or the file cannot be parsed.
std::string read_json_string(const std::filesystem::path& file, const std::string& key) {
    std::ifstream in(file);
    if (!in.is_open()) return {};

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    const std::string needle = "\"" + key + "\"";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();

    // Skip whitespace, expect ':'
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) ++pos;
    if (pos >= content.size() || content[pos] != ':') return {};
    ++pos;
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) ++pos;
    if (pos >= content.size() || content[pos] != '"') return {};

    ++pos;
    std::string value;
    while (pos < content.size() && content[pos] != '"') {
        value += content[pos];
        ++pos;
    }
    return value;
}

}  // namespace

std::vector<std::filesystem::path> theme_search_dirs(const std::filesystem::path& app_dir) {
    std::vector<std::filesystem::path> dirs;

    // 1. User themes (highest precedence) - where custom/edited themes go.
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::filesystem::path user_dir;
    if (xdg && *xdg) {
        user_dir = std::filesystem::path(xdg) / "GameModManager" / "themes";
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        user_dir = std::filesystem::path(home) / ".local" / "share" / "GameModManager" / "themes";
    }
    if (!user_dir.empty()) dirs.push_back(user_dir);

    // 2. Portable layout: themes/ beside the executable.
    dirs.push_back(app_dir / "themes");
    // 3. Dev/source layout: the GameModManager-Themes submodule (Core/themes).
    dirs.push_back(app_dir / ".." / "themes");
    // 4. Installed layout: share/GameModManager/themes.
    dirs.push_back(app_dir / ".." / "share" / "GameModManager" / "themes");
    // 5. Dev bundled themes: Core/resources/themes.
    dirs.push_back(app_dir / ".." / "resources" / "themes");

    return dirs;
}

void ThemeManager::scan_themes(const std::filesystem::path& themes_dir) {
    themes_.clear();
    scan_dir(themes_dir);
}

void ThemeManager::discover_themes(const std::filesystem::path& app_dir) {
    themes_.clear();
    for (const auto& dir : theme_search_dirs(app_dir)) {
        if (std::filesystem::exists(dir)) {
            scan_dir(dir);
        }
    }
    Logger::instance().debug("Discovered " + std::to_string(themes_.size()) + " themes");
}

void ThemeManager::scan_dir(const std::filesystem::path& themes_dir) {
    if (!std::filesystem::exists(themes_dir)) {
        Logger::instance().warn("Theme directory does not exist: " + themes_dir.string());
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(themes_dir)) {
        if (!entry.is_directory()) continue;

        ThemeInfo info;
        info.name = entry.path().filename().string();

        // Look for *.qss, tokens.json and optional theme.json metadata.
        for (const auto& f : std::filesystem::directory_iterator(entry.path())) {
            if (f.is_regular_file()) {
                auto ext = f.path().extension().string();
                if (ext == ".qss") {
                    info.qss_path = f.path();
                } else if (ext == ".json") {
                    if (f.path().filename() == "theme.json") {
                        info.base_style = read_json_string(f.path(), "base_style");
                    } else {
                        info.tokens_path = f.path();
                    }
                }
            }
        }

        if (!info.qss_path.empty() && !find_theme(info.name)) {
            themes_.push_back(std::move(info));
        }
    }

    std::sort(themes_.begin(), themes_.end(),
        [](const ThemeInfo& a, const ThemeInfo& b) { return a.name < b.name; });
}

bool ThemeManager::load_tokens(const std::filesystem::path& tokens_file) {
    std::ifstream file(tokens_file);
    if (!file.is_open()) {
        Logger::instance().error("Failed to open tokens file: " + tokens_file.string());
        return false;
    }

    // Simple JSON parser for flat key-value pairs: { "$key": "value" }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    tokens_.clear();

    std::string key, value;
    bool in_key = false, in_value = false;

    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];

        if (c == '"') {
            if (!in_key && !in_value) {
                // Start of a new string - figure out if it's key or value
                if (key.empty()) {
                    in_key = true;
                } else {
                    in_value = true;
                }
            } else if (in_key) {
                in_key = false;
            } else if (in_value) {
                in_value = false;
                tokens_[key] = value;
                key.clear();
                value.clear();
            }
        } else if (in_key) {
            key += c;
        } else if (in_value) {
            value += c;
        }
    }

    Logger::instance().debug("Loaded " + std::to_string(tokens_.size()) + " tokens from " +
        tokens_file.string());
    return true;
}

void ThemeManager::set_token(const std::string& key, const std::string& value) {
    tokens_[key] = value;
}

std::string ThemeManager::apply_template(const std::string& qss_template) const {
    std::string result = qss_template;

    // Sort tokens by key length descending so longer keys match first
    // (avoids partial replacement when one key is a prefix of another)
    std::vector<std::pair<std::string, std::string>> sorted_tokens(tokens_.begin(), tokens_.end());
    std::sort(sorted_tokens.begin(), sorted_tokens.end(),
        [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    for (const auto& [key, value] : sorted_tokens) {
        size_t pos = 0;
        while ((pos = result.find(key, pos)) != std::string::npos) {
            result.replace(pos, key.size(), value);
            pos += value.size();
        }
    }

    return result;
}

bool ThemeManager::render_theme(const std::filesystem::path& qss_template,
                                 const std::filesystem::path& output_path) const {
    std::ifstream in(qss_template);
    if (!in.is_open()) {
        Logger::instance().error("Failed to open QSS template: " + qss_template.string());
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    std::string rendered = apply_template(content);

    std::ofstream out(output_path);
    if (!out.is_open()) {
        Logger::instance().error("Failed to write rendered theme: " + output_path.string());
        return false;
    }

    out << rendered;
    return true;
}

const ThemeManager::ThemeInfo* ThemeManager::find_theme(const std::string& name) const {
    for (const auto& theme : themes_) {
        if (theme.name == name) return &theme;
    }
    return nullptr;
}

void ThemeManager::notify_changed() {
    if (callback_) {
        // Render the first available theme (or current one)
        if (!themes_.empty() && !themes_[0].qss_path.empty()) {
            std::ifstream in(themes_[0].qss_path);
            if (in.is_open()) {
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                callback_(apply_template(content));
            }
        }
    }
}

}  // namespace engine
