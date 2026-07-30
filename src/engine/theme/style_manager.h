#pragma once

#include <QObject>
#include <QString>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace engine {

class ThemeManager;

// Qt-aware style manager — loads QSS from embedded default or filesystem themes,
// applies them via qApp->setStyleSheet(), and supports live-reload through
// ThemeManager's callback mechanism.
class StyleManager : public QObject {
    Q_OBJECT
public:
    explicit StyleManager(ThemeManager& theme_manager, QObject* parent = nullptr);
    ~StyleManager() override;

    // Apply the default palette-based theme (embedded in the binary).
    // This is always available and requires no filesystem access.
    void apply_default();

    // Load and apply a named theme from the GameModManager-Themes submodule
    // or from a custom path. Returns true if the theme loaded successfully.
    bool load_theme(const std::filesystem::path& qss_path,
                    const std::filesystem::path& tokens_path = {});

    // Re-apply the current stylesheet (for live-reload triggers).
    void reload_current();

    // Get the name of the currently active theme.
    std::string current_theme_name() const { return current_theme_; }

    // Set a callback for when the theme changes (for file-watcher wiring).
    using ThemeChangedCallback = std::function<void()>;
    void on_theme_changed(ThemeChangedCallback cb) { theme_changed_cb_ = std::move(cb); }

signals:
    void theme_applied(const QString& name);

private:
    void apply_qss(const std::string& qss_content);

    ThemeManager& theme_manager_;
    std::string current_theme_;
    std::string current_qss_;
    std::filesystem::path current_qss_path_;
    ThemeChangedCallback theme_changed_cb_;
};

} // namespace engine
