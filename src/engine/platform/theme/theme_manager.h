#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Directories that should be searched for themes, in precedence order
// (earlier wins on name conflicts). The app dir is where the binary lives.
// Themes live as subdirectories, each containing a *.qss (template) and an
// optional tokens.json (see ThemeManager::load_tokens).
std::vector<std::filesystem::path> theme_search_dirs(const std::filesystem::path& app_dir);

// Qt-free theme engine - manages token maps and QSS template substitution.
// UI layer is responsible for applying the resulting QSS and watching for file changes.
class ThemeManager {
public:
    struct ThemeInfo {
        std::string name;
        std::filesystem::path qss_path;
        std::filesystem::path tokens_path;
        // Optional Qt base style (e.g. "Fusion") declared in theme.json.
        // Empty means the theme is style-agnostic and the active style is kept.
        std::string base_style;
    };

    // Scan a directory for themes (subdirs containing *.qss + optional *.json)
    void scan_themes(const std::filesystem::path& themes_dir);

    // Scan every directory returned by theme_search_dirs(app_dir), deduped by
    // name (first occurrence wins). Themes are sorted alphabetically.
    void discover_themes(const std::filesystem::path& app_dir);

    // Load token map from a JSON file (simple key-value: { "$primary": "#1e1e2e" })
    bool load_tokens(const std::filesystem::path& tokens_file);

    // Set a single token override
    void set_token(const std::string& key, const std::string& value);

    // Apply token substitution to a QSS template string
    [[nodiscard]] std::string apply_template(const std::string& qss_template) const;

    // Apply token substitution to a QSS file, write result to output path
    bool render_theme(const std::filesystem::path& qss_template,
                      const std::filesystem::path& output_path) const;

    // Get all discovered themes
    [[nodiscard]] const std::vector<ThemeInfo>& themes() const { return themes_; }

    // Get the current token map
    [[nodiscard]] const std::unordered_map<std::string, std::string>& tokens() const { return tokens_; }

    // Find a theme by name
    [[nodiscard]] const ThemeInfo* find_theme(const std::string& name) const;

    // Callback for when theme data changes (for UI live-reload wiring)
    using ThemeChangedCallback = std::function<void(const std::string& rendered_qss)>;
    void on_theme_changed(ThemeChangedCallback cb) { callback_ = std::move(cb); }

    // Notify listeners that tokens changed (UI calls this after file watcher fires)
    void notify_changed();

private:
    // Scan a single directory and append its themes (deduped by name, sorted).
    void scan_dir(const std::filesystem::path& themes_dir);

    std::vector<ThemeInfo> themes_;
    std::unordered_map<std::string, std::string> tokens_;
    ThemeChangedCallback callback_;
};

}  // namespace engine
