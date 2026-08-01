#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/theme/theme_manager.h"

namespace engine {

// Qt-aware style manager - loads QSS from embedded default or filesystem themes,
// applies them via qApp->setStyleSheet(), and supports live-reload through
// a QFileSystemWatcher on the active theme files.
class StyleManager : public QObject {
    Q_OBJECT
public:
    explicit StyleManager(ThemeManager& theme_manager, QObject* parent = nullptr);
    ~StyleManager() override;

    // Apply the default palette-based theme (embedded in the binary).
    // This is always available and requires no filesystem access.
    void apply_default();

    // Apply a theme by name (from ThemeManager's discovered themes), or the
    // default palette-based theme for "default"/""/unknown names.
    // Returns true if a discovered theme was applied.
    bool apply_theme(const std::string& name);

    // Load and apply a discovered theme. Watches its files for live-reload.
    bool load_theme(const ThemeManager::ThemeInfo& theme);

    // Re-apply the current stylesheet (for live-reload triggers).
    void reload_current();

    // Get the name of the currently active theme ("default" or a theme name).
    std::string current_theme_name() const { return current_theme_; }

    // Names of all discovered themes (excluding the built-in default).
    std::vector<std::string> theme_names() const;

    // Set a callback for when the theme changes (for file-watcher wiring).
    using ThemeChangedCallback = std::function<void()>;
    void on_theme_changed(ThemeChangedCallback cb) { theme_changed_cb_ = std::move(cb); }

signals:
    void theme_applied(const QString& name);

private:
    void apply_qss(const std::string& qss_content);
    void watch_theme_files();

    ThemeManager& theme_manager_;
    std::string current_theme_;
    std::string current_qss_;
    std::filesystem::path current_qss_path_;
    std::filesystem::path current_tokens_path_;
    ThemeChangedCallback theme_changed_cb_;
    QFileSystemWatcher watcher_;
};

} // namespace engine
